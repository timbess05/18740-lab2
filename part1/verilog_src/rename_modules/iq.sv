`default_nettype none

module IQ (
    input wire logic clk, reset,
    input wire logic [PPL_WIDTH-1:0] inserted_mask,
    input wire iq_entry_t [PPL_WIDTH-1:0] inserted_entries,
    input wire logic [PPL_WIDTH-1:0] inserted_is_branch,
    input wire logic [PPL_WIDTH-1:0] executed_mask,
    input wire logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] executed_preg,
    input wire logic issue_hold,
    output logic [PPL_WIDTH-1:0] issued_mask, issued_is_branch,
    output iq_entry_t [PPL_WIDTH-1:0] issued_entries,
    output logic full, stall,
    input wire logic flush_en,
    input wire logic [ROB_BIT-1:0] flush_index, rob_head
);
    // One fixed bank per dispatch lane, and at most one issue per bank.
    // All seven lab configurations have IQ_SIZE divisible by PPL_WIDTH.
    localparam int DEPTH = IQ_SIZE / PPL_WIDTH;
    localparam int SW = (DEPTH > 1) ? $clog2(DEPTH) : 1;
    localparam int BCW = $clog2(DEPTH + 1);
    localparam int PAD = 1 << $clog2(DEPTH);

    iq_entry_t mem [0:PPL_WIDTH-1][0:DEPTH-1];
    logic is_branch [0:PPL_WIDTH-1][0:DEPTH-1];
    logic [BCW-1:0] used [0:PPL_WIDTH-1];
    logic [IQ_BIT:0] used_total;
    iq_entry_t [PPL_WIDTH-1:0] prepared;
    logic [PPL_WIDTH-1:0] candidate_valid, candidate_branch, free_valid;
    logic [PPL_WIDTH-1:0] allocate, sweep_hit;
    logic [SW-1:0] candidate_slot [0:PPL_WIDTH-1];
    logic [SW-1:0] free_slot [0:PPL_WIDTH-1];
    logic [ROB_BIT-1:0] sweep_age [0:PPL_WIDTH-1];
    logic sweep_busy, branch_chosen;
    logic [SW-1:0] sweep_slot;
    logic [ROB_BIT-1:0] sweep_head, sweep_branch_age;

    generate
        for (genvar bank = 0; bank < PPL_WIDTH; bank++) begin : gen_bank
            logic ready_tree [1:2*PAD-1];
            logic empty_tree [1:2*PAD-1];
            logic [SW-1:0] ready_index [1:2*PAD-1];
            logic [SW-1:0] empty_index [1:2*PAD-1];
            // Balanced priority trees have only log2(DEPTH) selection stages.
            for (genvar s = 0; s < PAD; s++) begin : gen_leaf
                if (s < DEPTH) begin : gen_real
                    assign ready_tree[PAD+s] = mem[bank][s].valid &&
                                                mem[bank][s].src1_ready &&
                                                mem[bank][s].src2_ready;
                    assign empty_tree[PAD+s] = !mem[bank][s].valid;
                end else begin : gen_padding
                    assign ready_tree[PAD+s] = 1'b0;
                    assign empty_tree[PAD+s] = 1'b0;
                end
                assign ready_index[PAD+s] = SW'(s);
                assign empty_index[PAD+s] = SW'(s);
            end
            for (genvar n = 1; n < PAD; n++) begin : gen_select
                assign ready_tree[n] = ready_tree[2*n] || ready_tree[2*n+1];
                assign ready_index[n] = ready_tree[2*n] ? ready_index[2*n] : ready_index[2*n+1];
                assign empty_tree[n] = empty_tree[2*n] || empty_tree[2*n+1];
                assign empty_index[n] = empty_tree[2*n] ? empty_index[2*n] : empty_index[2*n+1];
            end
            assign candidate_valid[bank] = ready_tree[1];
            assign candidate_slot[bank] = ready_index[1];
            assign candidate_branch[bank] = candidate_valid[bank] &&
                                            is_branch[bank][candidate_slot[bank]];
            assign free_valid[bank] = empty_tree[1];
            assign free_slot[bank] = empty_index[1];
            assign allocate[bank] = !reset && !sweep_busy && !flush_en &&
                                    inserted_mask[bank] && free_valid[bank];
            assign sweep_age[bank] = ROB_BIT'(mem[bank][sweep_slot].rob_index - sweep_head);
            assign sweep_hit[bank] = sweep_busy && mem[bank][sweep_slot].valid &&
                                     (sweep_age[bank] > sweep_branch_age);

            always_ff @(posedge clk) begin
                if (reset) used[bank] <= '0;
                else used[bank] <= used[bank] + BCW'(allocate[bank])
                                            - BCW'(issued_mask[bank])
                                            - BCW'(sweep_hit[bank]);
            end
            // Constant physical-slot write enables avoid whole-array shifting.
            for (genvar s = 0; s < DEPTH; s++) begin : gen_entry
                always_ff @(posedge clk) begin
                    if (reset) begin
                        mem[bank][s].valid <= 1'b0;
                    end else if (allocate[bank] && free_slot[bank] == SW'(s)) begin
                        mem[bank][s] <= prepared[bank];
                        is_branch[bank][s] <= inserted_is_branch[bank];
                    end else if ((issued_mask[bank] && candidate_slot[bank] == SW'(s)) ||
                                 (sweep_hit[bank] && sweep_slot == SW'(s))) begin
                        mem[bank][s].valid <= 1'b0;
                    end else if (mem[bank][s].valid) begin
                        for (int lane = 0; lane < PPL_WIDTH; lane++) begin
                            if (executed_mask[lane]) begin
                                if (mem[bank][s].src1 == executed_preg[lane])
                                    mem[bank][s].src1_ready <= 1'b1;
                                if (mem[bank][s].src2 == executed_preg[lane])
                                    mem[bank][s].src2_ready <= 1'b1;
                            end
                        end
                    end
                end
            end
        end
    endgenerate

    always_comb begin
        used_total = '0;
        stall = sweep_busy;
        for (int bank = 0; bank < PPL_WIDTH; bank++) begin
            used_total = used_total + (IQ_BIT+1)'(used[bank]);
            // Bank imbalance is backpressure, not a false global full indication.
            if (used[bank] == BCW'(DEPTH)) stall = 1'b1;
        end
        full = (IQ_SIZE - used_total) < PPL_WIDTH;

        issued_mask = '0;
        issued_entries = '0;
        issued_is_branch = '0;
        branch_chosen = 1'b0;
        if (!reset && !issue_hold && !sweep_busy) begin
            for (int bank = 0; bank < PPL_WIDTH; bank++) begin
                // Select a branch alone; wait for its response before issuing again.
                // Otherwise all ready banks may issue, including out-of-order entries.
                if (|candidate_branch) begin
                    if (candidate_branch[bank] && !branch_chosen) begin
                        issued_mask[bank] = 1'b1;
                        issued_entries[bank] = mem[bank][candidate_slot[bank]];
                        issued_is_branch[bank] = 1'b1;
                        branch_chosen = 1'b1;
                    end
                end else if (candidate_valid[bank]) begin
                    issued_mask[bank] = 1'b1;
                    issued_entries[bank] = mem[bank][candidate_slot[bank]];
                end
            end
        end

        prepared = inserted_entries;
        for (int bank = 0; bank < PPL_WIDTH; bank++) begin
            prepared[bank].valid = 1'b1;
            for (int lane = 0; lane < PPL_WIDTH; lane++) begin
                if (executed_mask[lane]) begin
                    if (inserted_entries[bank].src1 == executed_preg[lane])
                        prepared[bank].src1_ready = 1'b1;
                    if (inserted_entries[bank].src2 == executed_preg[lane])
                        prepared[bank].src2_ready = 1'b1;
                end
            end
        end
    end

    // Recovery examines one slot in each bank per cycle, rather than comparing
    // and population-counting every IQ entry on the same combinational path.
    always_ff @(posedge clk) begin
        if (reset) begin
            sweep_busy <= 1'b0;
            sweep_slot <= '0;
            sweep_head <= '0;
            sweep_branch_age <= '0;
        end else if (flush_en) begin
            sweep_busy <= 1'b1;
            sweep_slot <= '0;
            sweep_head <= rob_head;
            sweep_branch_age <= ROB_BIT'(flush_index - rob_head);
        end else if (sweep_busy) begin
            if (sweep_slot == SW'(DEPTH-1)) begin
                sweep_busy <= 1'b0;
                sweep_slot <= '0;
            end else sweep_slot <= sweep_slot + 1'b1;
        end
    end
endmodule : IQ
