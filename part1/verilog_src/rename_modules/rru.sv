`default_nettype none

module RRU (
    input logic clk, reset,

    // Incoming register offers, including offers canceled by a same-cycle flush.
    input logic [PPL_WIDTH-1:0] inserted_mask,
    input instruction_t [PPL_WIDTH-1:0] inserted_entries,

    input logic [PPL_WIDTH-1:0] committed_mask,
    input logic [PPL_WIDTH-1:0] removed_mask,
    input logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] removed_preg,
    input logic [PPL_WIDTH-1:0][ARCH_BIT-1:0] removed_areg,
    input logic [PPL_WIDTH-1:0] removed_is_branch,

    output logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] renamed_preg,
    output rob_entry_t [PPL_WIDTH-1:0] renamed_rob_entries,
    output iq_entry_t [PPL_WIDTH-1:0] renamed_iq_entries,

    input logic [PHYS_REG-1:0][1:0] preg_states,
    input logic [ARCH_REG-1:0][PHYS_BIT-1:0] archRAT,
    input logic [PPL_WIDTH-1:0][ROB_BIT-1:0] inserted_index,
    output logic full,
    output logic stall,
    input logic flush_en,
    input logic [ROB_BIT-1:0] flush_index
);

    logic [ARCH_REG-1:0][PHYS_BIT-1:0] specRAT, specRAT_working;
    logic [ARCH_REG-1:0][PHYS_BIT-1:0] archRAT_working;
    logic [PHYS_BIT-1:0] src1_preg, src2_preg;
    logic [ARCH_REG-1:0] rat_dirty;
    logic [PHYS_REG-1:0] preg_free, preg_free_working, preg_claimable;
    instruction_t inserted_entry;
    logic [PHYS_BIT:0] occupancy, preg_start;
    logic [$clog2(PPL_WIDTH+1)-1:0] commit_cnt, insert_cnt, squash_cnt;

    logic [ARCH_REG-1:0][PHYS_BIT-1:0] rat_checkpoint [ROB_SIZE-1:0];
    logic [PPL_WIDTH-1:0][ARCH_REG-1:0][PHYS_BIT-1:0] checkpoint_data;
    logic [PPL_WIDTH-1:0] checkpoint_write;

    assign commit_cnt = $countones(committed_mask);
    // Canceled offers remain reserved until their noncommitting removal is reported.
    assign insert_cnt = $countones(inserted_mask);
    assign squash_cnt = $countones(removed_mask & ~committed_mask);
    assign full = (PHYS_REG - occupancy) < PPL_WIDTH;
    assign stall = 1'b0;

    always_comb begin
        renamed_preg = '0;
        renamed_rob_entries = '0;
        renamed_iq_entries = '0;
        checkpoint_write = '0;
        checkpoint_data = '0;
        specRAT_working = specRAT;
        archRAT_working = archRAT;
        preg_free_working = preg_free;
        preg_claimable = preg_free;
        rat_dirty = '0;
        preg_start = '0;
        inserted_entry = '0;
        src1_preg = '0;
        src2_preg = '0;

        // Process the four lanes in order, preserving same-bundle dependencies.
        for (int i = 0; i < PPL_WIDTH; i++) begin
            if (committed_mask[i]) begin
                preg_free_working[archRAT_working[removed_areg[i]]] = 1'b1;
                archRAT_working[removed_areg[i]] = removed_preg[i];
            end
            else if (removed_mask[i]) begin
                preg_free_working[removed_preg[i]] = 1'b1;
            end

            // Keep the offered destination stable even when a late flush arrives.
            if (inserted_mask[i]) begin
                inserted_entry = inserted_entries[i];
                src1_preg = specRAT_working[inserted_entry.src1];
                src2_preg = specRAT_working[inserted_entry.src2];
                renamed_iq_entries[i].src1 = src1_preg;
                renamed_iq_entries[i].src2 = src2_preg;
                renamed_iq_entries[i].src1_ready =
                    !rat_dirty[inserted_entry.src1] &&
                    (preg_states[src1_preg] == 2'b10 || preg_states[src1_preg] == 2'b11);
                renamed_iq_entries[i].src2_ready =
                    !rat_dirty[inserted_entry.src2] &&
                    (preg_states[src2_preg] == 2'b10 || preg_states[src2_preg] == 2'b11);

                for (int j = 0; j < PHYS_REG; j++) begin
                    if (j >= preg_start && preg_claimable[j]) begin
                        preg_free_working[j] = 1'b0;
                        preg_claimable[j] = 1'b0;
                        specRAT_working[inserted_entry.dest] = PHYS_BIT'(j);
                        rat_dirty[inserted_entry.dest] = 1'b1;
                        renamed_preg[i] = PHYS_BIT'(j);
                        renamed_rob_entries[i].areg = inserted_entry.dest;
                        renamed_rob_entries[i].preg = PHYS_BIT'(j);
                        renamed_rob_entries[i].inst_ID = inserted_entry.inst_ID;
                        renamed_rob_entries[i].is_branch = inserted_entry.is_branch;
                        renamed_iq_entries[i].rob_index = inserted_index[i];
                        renamed_iq_entries[i].inst_ID = inserted_entry.inst_ID;
                        if (inserted_entry.is_branch) begin
                            checkpoint_write[i] = 1'b1;
                            checkpoint_data[i] = specRAT_working;
                        end
                        preg_start = (PHYS_BIT+1)'(j + 1);
                        break;
                    end
                end
            end
        end
    end

    always_ff @(posedge clk) begin
        if (reset) begin
            for (int i = 0; i < ARCH_REG; i++) specRAT[i] <= PHYS_BIT'(i);
            for (int i = 0; i < PHYS_REG; i++) begin
                if (i < ARCH_REG) preg_free[i] <= 1'b0;
                else preg_free[i] <= 1'b1;
            end
            occupancy <= ARCH_REG;
        end
        else begin
            // The current canceled bundle must not change the restored mapping.
            specRAT <= flush_en ? rat_checkpoint[flush_index] : specRAT_working;
            preg_free <= preg_free_working;
            occupancy <= occupancy - commit_cnt - squash_cnt + insert_cnt;
            if (!flush_en) begin
                for (int i = 0; i < PPL_WIDTH; i++) begin
                    if (checkpoint_write[i])
                        rat_checkpoint[inserted_index[i]] <= checkpoint_data[i];
                end
            end
        end
    end

endmodule : RRU