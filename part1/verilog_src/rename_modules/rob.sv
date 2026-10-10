`default_nettype none

module ROB (
    input wire logic clk, reset,
    // A flush-cycle offer is retained only until its cancellation is reported.
    input wire logic [PPL_WIDTH-1:0] inserted_mask,
    input wire rob_entry_t [PPL_WIDTH-1:0] inserted_entries,
    input wire logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] inserted_old_preg,
    output logic [PPL_WIDTH-1:0][ROB_BIT-1:0] inserted_index,

    input wire logic [PPL_WIDTH-1:0] executed_mask,
    input wire logic [PPL_WIDTH-1:0][ROB_BIT-1:0] executed_index,
    input wire logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] executed_preg,
    output logic [PPL_WIDTH-1:0] live_executed_mask,

    input wire logic [PPL_WIDTH-1:0] issued_mask, issued_is_branch,
    input wire iq_entry_t [PPL_WIDTH-1:0] issued_entries,
    output logic issue_hold,

    output logic [PPL_WIDTH-1:0] removed_mask, committed_mask,
    output rob_entry_t [PPL_WIDTH-1:0] removed_entries,
    output logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] removed_old_preg,
    output logic full, recovery_busy,
    input wire logic flush_en,
    input wire logic [ROB_BIT-1:0] flush_index,
    output logic accepted_flush,
    output logic [ROB_BIT-1:0] rob_head
);
    localparam int CW = $clog2(PPL_WIDTH + 1);
    localparam int OW = $clog2(ROB_SIZE + 1);
    rob_entry_t rob_mem [0:ROB_SIZE-1];
    logic [PHYS_BIT-1:0] old_preg_mem [0:ROB_SIZE-1];
    logic [ROB_BIT-1:0] rob_tail, flush_target, last_index, remove_index;
    logic [ROB_BIT-1:0] branch_index, bound_age;
    logic [ROB_BIT-1:0] response_age [0:PPL_WIDTH-1];
    logic [OW-1:0] occupancy;
    logic [CW-1:0] insert_count, offset;
    logic flushing, branch_inflight, remove_fire;

    assign full = (ROB_SIZE - occupancy) < PPL_WIDTH;
    assign recovery_busy = flushing;
    // No new issues while a branch is in execution. This is registered control,
    // not a combinational cancellation of already-presented issue outputs.
    assign issue_hold = flushing || branch_inflight;
    assign accepted_flush = !reset && flush_en && branch_inflight &&
                            flush_index == branch_index &&
                            rob_mem[branch_index].valid &&
                            rob_mem[branch_index].is_branch;
    assign insert_count = flushing ? '0 : CW'($countones(inserted_mask));
    assign last_index = ROB_BIT'(rob_tail - 1'b1);
    assign remove_index = flushing ? last_index : rob_head;
    assign bound_age = flushing
                     ? ROB_BIT'(flush_target - 1'b1 - rob_head)
                     : ROB_BIT'(flush_index - rob_head);

    // Check only the completion lanes, not every ROB slot.
    always_comb begin
        live_executed_mask = '0;
        for (int lane = 0; lane < PPL_WIDTH; lane++) begin
            response_age[lane] = ROB_BIT'(executed_index[lane] - rob_head);
            if (!reset && executed_mask[lane] &&
                rob_mem[executed_index[lane]].valid &&
                rob_mem[executed_index[lane]].preg == executed_preg[lane] &&
                (!(flushing || accepted_flush) || response_age[lane] <= bound_age))
                live_executed_mask[lane] = 1'b1;
        end
    end

    // One commit OR one undo per cycle; PPL_WIDTH external ports are unchanged.
    // This transaction depends on registered state, not the newly driven flush.
    always_comb begin
        removed_mask = '0;
        committed_mask = '0;
        removed_entries = '0;
        removed_old_preg = '0;
        inserted_index = '0;
        remove_fire = 1'b0;
        offset = '0;
        for (int lane = 0; lane < PPL_WIDTH; lane++) begin
            inserted_index[lane] = ROB_BIT'(rob_tail + offset);
            if (inserted_mask[lane]) offset = offset + 1'b1;
        end
        if (!reset && occupancy != 0) begin
            if (flushing)
                remove_fire = (rob_tail != flush_target) && rob_mem[last_index].valid;
            else
                remove_fire = rob_mem[rob_head].valid && rob_mem[rob_head].is_completed;
        end
        if (remove_fire) begin
            removed_mask[0] = 1'b1;
            committed_mask[0] = !flushing;
            removed_entries[0] = rob_mem[remove_index];
            removed_old_preg[0] = old_preg_mem[remove_index];
        end
    end

    always_ff @(posedge clk) begin
        if (reset) begin
            for (int j = 0; j < ROB_SIZE; j++) begin
                rob_mem[j].valid <= 1'b0;
                rob_mem[j].is_completed <= 1'b0;
            end
            rob_head <= '0;
            rob_tail <= '0;
            flush_target <= '0;
            occupancy <= '0;
            flushing <= 1'b0;
            branch_inflight <= 1'b0;
            branch_index <= '0;
        end else begin
            occupancy <= occupancy + OW'(insert_count) - OW'(remove_fire);
            if (remove_fire) begin
                rob_mem[remove_index].valid <= 1'b0;
                if (flushing)
                    rob_tail <= last_index;
                else
                    rob_head <= ROB_BIT'(rob_head + 1'b1);
            end
            if (!flushing) begin
                for (int lane = 0; lane < PPL_WIDTH; lane++) begin
                    if (inserted_mask[lane]) begin
                        rob_mem[inserted_index[lane]] <= inserted_entries[lane];
                        rob_mem[inserted_index[lane]].valid <= 1'b1;
                        rob_mem[inserted_index[lane]].is_completed <= 1'b0;
                        old_preg_mem[inserted_index[lane]] <= inserted_old_preg[lane];
                    end
                end
                rob_tail <= ROB_BIT'(rob_tail + insert_count);
            end
            for (int lane = 0; lane < PPL_WIDTH; lane++) begin
                if (live_executed_mask[lane])
                    rob_mem[executed_index[lane]].is_completed <= 1'b1;
                if (branch_inflight && executed_mask[lane] &&
                    executed_index[lane] == branch_index &&
                    executed_preg[lane] == rob_mem[branch_index].preg)
                    branch_inflight <= 1'b0;
                if (issued_mask[lane] && issued_is_branch[lane]) begin
                    branch_inflight <= 1'b1;
                    branch_index <= issued_entries[lane].rob_index;
                end
            end
            if (flushing &&
                (rob_tail == flush_target || (remove_fire && last_index == flush_target)))
                flushing <= 1'b0;
            if (accepted_flush) begin
                flush_target <= ROB_BIT'(flush_index + 1'b1);
                flushing <= 1'b1;
            end
        end
    end
endmodule : ROB
`default_nettype wire