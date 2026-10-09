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

    assign full = (IQ_SIZE - occupancy) < PPL_WIDTH;
    assign insert_cnt = $countones(allocated_mask);
    assign removed_indexes = issued_indexes | flush_mask;
    assign removed_count = $countones(removed_indexes);

    always_comb begin
        issued_indexes    = '0;
        issued_mask       = '0;
        issued_entries    = '0;
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

        // The issue transaction depends on registered state, not a newly driven flush.
        if (!reset) begin
            for (int lane = 0; lane < PPL_WIDTH; lane++) begin
                for (int j = 0; j < IQ_SIZE; j++) begin
                    if (iq_mem[j].valid &&
                        iq_mem[j].src1_ready && iq_mem[j].src2_ready &&
                        !issued_indexes[j] &&
                        !blocked_branches[iq_mem[j].rob_index]) begin
                        issued_mask[lane]    = 1'b1;
                        issued_entries[lane] = iq_mem[j];
                        issued_indexes[j]   = 1'b1;
                        break;
                    end
                end
            end

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
        end else begin
            for (int j = 0; j < IQ_SIZE; j++) begin
                if (removed_indexes[j]) begin
                    iq_mem[j].valid <= 1'b0;
                    avail_indexes[j] <= 1'b1;
                end else if (iq_mem[j].valid && !allocated_indexes[j]) begin
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