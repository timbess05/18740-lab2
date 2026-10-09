`default_nettype none

module IQ (
    input  logic clk, reset,

    input  logic [PPL_WIDTH-1:0] inserted_mask,
    input  iq_entry_t [PPL_WIDTH-1:0] inserted_entries,

    input  logic [PPL_WIDTH-1:0] executed_mask,
    input  logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] executed_preg,

    output logic [PPL_WIDTH-1:0] issued_mask,
    output iq_entry_t [PPL_WIDTH-1:0] issued_entries,
    output logic full,

    input  logic flush_en,
    input  logic [ROB_BIT-1:0] flush_index,
    input  logic [ROB_BIT-1:0] rob_head,
    input  logic [ROB_SIZE-1:0] blocked_branches
);

    iq_entry_t iq_mem [IQ_SIZE-1:0];
    iq_entry_t [PPL_WIDTH-1:0] prepared_entries;

    logic [IQ_SIZE-1:0] issued_indexes, avail_indexes;
    logic [IQ_SIZE-1:0] allocated_indexes;
    logic [PPL_WIDTH-1:0] allocated_mask;
    logic [PPL_WIDTH-1:0][IQ_BIT-1:0] inserted_indexes;

    logic [$clog2(PPL_WIDTH+1)-1:0] insert_cnt;
    logic [IQ_BIT:0] occupancy, removed_count;

    logic [IQ_SIZE-1:0] flush_mask, removed_indexes;
    logic [ROB_BIT-1:0] branch_dist;
    logic [ROB_BIT-1:0] entry_dist [IQ_SIZE-1:0];

    localparam int PICK_W = $clog2(PPL_WIDTH + 1);
    localparam int IQ_LEVELS = $clog2(IQ_SIZE);
    localparam int IQ_PAD = 1 << IQ_LEVELS;
    localparam int ENTRY_W = $bits(iq_entry_t);
    localparam int OCC_W = IQ_BIT + 1;

    logic [IQ_SIZE-1:0] issue_ready;
    logic [PICK_W-1:0] ready_prefix [0:IQ_LEVELS][0:IQ_SIZE-1];
    logic [PPL_WIDTH-1:0][IQ_SIZE-1:0] issue_grant;
    logic [ENTRY_W-1:0] issue_mux [0:PPL_WIDTH-1][1:2*IQ_PAD-1];
    logic [PPL_WIDTH-1:0] surviving_issue_mask;
    logic [IQ_BIT:0] flush_count;

    assign full = (IQ_SIZE - occupancy) < PPL_WIDTH;
    assign insert_cnt = $countones(allocated_mask);
    assign removed_indexes = issued_indexes | flush_mask;
    assign flush_count = OCC_W'($countones(flush_mask));

    // Normal cycles count eligible requests directly, without waiting for grants.
    // During a flush, count its entries plus issued entries NOT in the flush set.
    assign removed_count = flush_en
                         ? flush_count + OCC_W'($countones(surviving_issue_mask))
                         : OCC_W'(ready_prefix[IQ_LEVELS][IQ_SIZE-1]);

    // The issue transaction depends on registered state, not a newly driven flush.
    generate
        for (genvar j = 0; j < IQ_SIZE; j++) begin : gen_issue_ready
            assign issue_ready[j] = !reset && iq_mem[j].valid &&
                                    iq_mem[j].src1_ready &&
                                    iq_mem[j].src2_ready &&
                                    !blocked_branches[iq_mem[j].rob_index];

            assign ready_prefix[0][j] = PICK_W'(issue_ready[j]);
        end

        // Inclusive prefix counts, saturated at PPL_WIDTH. Each stage combines
        // ranges twice as large as the previous stage: 1, 2, 4, 8, ... entries.
        for (genvar s = 0; s < IQ_LEVELS; s++) begin : gen_ready_prefix
            for (genvar j = 0; j < IQ_SIZE; j++) begin : gen_slot
                if (j >= (1 << s)) begin : gen_add
                    logic [PICK_W:0] sum;

                    assign sum = {1'b0, ready_prefix[s][j]} +
                                 {1'b0, ready_prefix[s][j-(1 << s)]};

                    assign ready_prefix[s+1][j] =
                        (sum >= (PICK_W+1)'(PPL_WIDTH))
                        ? PICK_W'(PPL_WIDTH)
                        : sum[PICK_W-1:0];
                end
                else begin : gen_copy
                    assign ready_prefix[s+1][j] = ready_prefix[s][j];
                end
            end
        end

        // A ready slot with k ready predecessors wins lane k, for k < PPL_WIDTH.
        for (genvar j = 0; j < IQ_SIZE; j++) begin : gen_issue_grants
            logic [PICK_W-1:0] rank_before;

            if (j == 0) begin : gen_first
                assign rank_before = '0;
            end
            else begin : gen_later
                assign rank_before = ready_prefix[IQ_LEVELS][j-1];
            end

            assign issued_indexes[j] = issue_ready[j] &&
                                       (rank_before < PICK_W'(PPL_WIDTH));

            for (genvar lane = 0; lane < PPL_WIDTH; lane++) begin : gen_lane
                assign issue_grant[lane][j] = issue_ready[j] &&
                                             (rank_before == PICK_W'(lane));
            end
        end

        // Each lane has a one-hot grant: select payload with a balanced OR tree,
        // not a priority overwrite chain. Unselected lanes produce all zeros.
        for (genvar lane = 0; lane < PPL_WIDTH; lane++) begin : gen_issue_payload
            assign issued_mask[lane] =
                ready_prefix[IQ_LEVELS][IQ_SIZE-1] > PICK_W'(lane);

            assign surviving_issue_mask[lane] =
                |(issue_grant[lane] & ~flush_mask);

            for (genvar j = 0; j < IQ_PAD; j++) begin : gen_leaf
                if (j < IQ_SIZE) begin : gen_real
                    assign issue_mux[lane][IQ_PAD+j] =
                        iq_mem[j] & {ENTRY_W{issue_grant[lane][j]}};
                end
                else begin : gen_padding
                    assign issue_mux[lane][IQ_PAD+j] = '0;
                end
            end

            for (genvar n = 1; n < IQ_PAD; n++) begin : gen_or
                assign issue_mux[lane][n] =
                    issue_mux[lane][2*n] | issue_mux[lane][2*n+1];
            end

            assign issued_entries[lane] = iq_entry_t'(issue_mux[lane][1]);
        end
    endgenerate

    always_comb begin
        inserted_indexes  = '0;
        allocated_indexes = '0;
        allocated_mask    = '0;

        branch_dist = flush_index - rob_head;

        for (int j = 0; j < IQ_SIZE; j++) begin
            entry_dist[j] = iq_mem[j].rob_index - rob_head;
            flush_mask[j] = flush_en && iq_mem[j].valid &&
                            (entry_dist[j] > branch_dist);
        end

        for (int lane = 0; lane < PPL_WIDTH; lane++) begin
            prepared_entries[lane] = inserted_entries[lane];
            prepared_entries[lane].valid = 1'b1;

            for (int b = 0; b < PPL_WIDTH; b++) begin
                if (executed_mask[b]) begin
                    if (inserted_entries[lane].src1 == executed_preg[b])
                        prepared_entries[lane].src1_ready = 1'b1;

                    if (inserted_entries[lane].src2 == executed_preg[b])
                        prepared_entries[lane].src2_ready = 1'b1;
                end
            end
        end

        // Allocation policy and wakeup timing are unchanged.
        if (!reset) begin
            // Canceled incoming instructions never enter the IQ.
            if (!flush_en && !full) begin
                for (int lane = 0; lane < PPL_WIDTH; lane++) begin
                    if (inserted_mask[lane]) begin
                        for (int j = 0; j < IQ_SIZE; j++) begin
                            if (avail_indexes[j] && !allocated_indexes[j]) begin
                                inserted_indexes[lane] = IQ_BIT'(j);
                                allocated_indexes[j] = 1'b1;
                                allocated_mask[lane] = 1'b1;
                                break;
                            end
                        end
                    end
                end
            end
        end
    end

    always_ff @(posedge clk) begin
        if (reset) begin
            for (int j = 0; j < IQ_SIZE; j++)
                iq_mem[j] <= '0;

            avail_indexes <= '1;
            occupancy <= '0;
        end
        else begin
            for (int j = 0; j < IQ_SIZE; j++) begin
                if (removed_indexes[j]) begin
                    iq_mem[j].valid <= 1'b0;
                    avail_indexes[j] <= 1'b1;
                end
                else if (iq_mem[j].valid && !allocated_indexes[j]) begin
                    for (int b = 0; b < PPL_WIDTH; b++) begin
                        if (executed_mask[b]) begin
                            if (iq_mem[j].src1 == executed_preg[b])
                                iq_mem[j].src1_ready <= 1'b1;

                            if (iq_mem[j].src2 == executed_preg[b])
                                iq_mem[j].src2_ready <= 1'b1;
                        end
                    end
                end
            end

            for (int lane = 0; lane < PPL_WIDTH; lane++) begin
                if (allocated_mask[lane]) begin
                    iq_mem[inserted_indexes[lane]] <= prepared_entries[lane];
                    avail_indexes[inserted_indexes[lane]] <= 1'b0;
                end
            end

            occupancy <= occupancy + insert_cnt - removed_count;
        end
    end

endmodule : IQ