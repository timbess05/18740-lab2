`default_nettype none

module ROB (
    // Clock and synchronous active high reset
    input logic clk, reset,

    // Incoming entry signals; on a flush these are cancellation records only.
    input logic [PPL_WIDTH-1:0] inserted_mask,
    input rob_entry_t [PPL_WIDTH-1:0] inserted_entries,
    output logic [PPL_WIDTH-1:0][ROB_BIT-1:0] inserted_index,

    // Raw execution responses, including responses that must be discarded.
    input logic [PPL_WIDTH-1:0] executed_mask,
    input logic [PPL_WIDTH-1:0][ROB_BIT-1:0] executed_index,
    input logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] executed_preg,
    output logic [PPL_WIDTH-1:0] live_executed_mask,

    // Instructions presented to the execution interface this cycle.
    input logic [PPL_WIDTH-1:0] issued_mask,
    input iq_entry_t [PPL_WIDTH-1:0] issued_entries,
    output logic [ROB_SIZE-1:0] blocked_branches,

    // Output entry signals
    output logic [PPL_WIDTH-1:0] removed_mask,
    output logic [PPL_WIDTH-1:0] committed_mask,
    output rob_entry_t [PPL_WIDTH-1:0] removed_entries,
    output logic full,

    // Branch signals
    input logic flush_en,
    input logic [ROB_BIT-1:0] flush_index,
    output logic accepted_flush,
    output logic recovery_busy,
    output logic rob_stall,
    output logic [ROB_BIT-1:0] rob_head
);

    rob_entry_t rob_mem [ROB_SIZE-1:0];
    logic [ROB_BIT-1:0] rob_tail;
    logic [ROB_BIT:0] occupancy;
    logic [$clog2(PPL_WIDTH+1)-1:0] retire_cnt, insert_cnt, squash_cnt;
    logic [PPL_WIDTH-1:0][ROB_BIT-1:0] removed_indexes;
    logic still_retiring, scan_active;

    logic flushing;
    logic [ROB_BIT-1:0] flush_ptr, flush_target, next_ptr;
    logic [ROB_BIT-1:0] incoming_age, active_age;
    logic [ROB_BIT-1:0] entry_age [ROB_SIZE-1:0];
    logic [ROB_SIZE-1:0] killed, pending_result;
    logic older_branch_pending;
    logic [ROB_BIT-1:0] branch_scan_index;
    logic [$clog2(PPL_WIDTH+1)-1:0] insert_offset;

    assign full = (ROB_SIZE - occupancy) < PPL_WIDTH;
    // Registered recovery state controls interface transactions and dispatch.
    assign recovery_busy = flushing;
    assign rob_stall = flushing;
    assign insert_cnt = flushing ? '0 : $countones(inserted_mask);
    assign incoming_age = flush_index - rob_head;
    assign active_age = ROB_BIT'(flush_target - 1'b1) - rob_head;
    assign accepted_flush = !reset && flush_en &&
                            rob_mem[flush_index].valid &&
                            rob_mem[flush_index].is_branch &&
                            !rob_mem[flush_index].is_completed &&
                            (!flushing || incoming_age < active_age);

    // Raw flushes affect next state and result acceptance, not current removals.
    always_comb begin
        for (int j = 0; j < ROB_SIZE; j++) begin
            entry_age[j] = ROB_BIT'(j) - rob_head;
            killed[j] = rob_mem[j].valid &&
                        ((flushing && entry_age[j] > active_age) ||
                         (accepted_flush && entry_age[j] > incoming_age));
        end

        live_executed_mask = '0;
        for (int i = 0; i < PPL_WIDTH; i++) begin
            if (!reset && executed_mask[i] &&
                rob_mem[executed_index[i]].valid &&
                pending_result[executed_index[i]] &&
                !killed[executed_index[i]] &&
                rob_mem[executed_index[i]].preg == executed_preg[i]) begin
                live_executed_mask[i] = 1'b1;
            end
        end
    end

    // Resolve branches in order; nonbranch instructions remain out of order.
    always_comb begin
        blocked_branches = '0;
        older_branch_pending = 1'b0;
        branch_scan_index = rob_head;
        for (int j = 0; j < ROB_SIZE; j++) begin
            branch_scan_index = ROB_BIT'(rob_head + j);
            if (rob_mem[branch_scan_index].valid &&
                rob_mem[branch_scan_index].is_branch &&
                !rob_mem[branch_scan_index].is_completed) begin
                blocked_branches[branch_scan_index] = older_branch_pending;
                older_branch_pending = 1'b1;
            end
        end
    end

    always_comb begin
        removed_mask = '0;
        committed_mask = '0;
        removed_entries = '0;
        removed_indexes = '0;
        inserted_index = '0;
        retire_cnt = '0;
        squash_cnt = '0;
        insert_offset = '0;
        still_retiring = 1'b1;
        scan_active = 1'b1;
        next_ptr = flush_ptr;

        for (int i = 0; i < PPL_WIDTH; i++) begin
            inserted_index[i] = ROB_BIT'(rob_tail + insert_offset);
            if (inserted_mask[i]) insert_offset = insert_offset + 1'b1;
        end

        if (!reset) begin
            if (flushing) begin
                for (int i = 0; i < PPL_WIDTH; i++) begin
                    if (scan_active && next_ptr != rob_tail) begin
                        if (!rob_mem[next_ptr].valid) begin
                            next_ptr = ROB_BIT'(next_ptr + 1'b1);
                        end
                        else if (pending_result[next_ptr]) begin
                            // Do not reuse this allocation while a late response exists.
                            scan_active = 1'b0;
                        end
                        else begin
                            removed_indexes[squash_cnt] = next_ptr;
                            removed_entries[squash_cnt] = rob_mem[next_ptr];
                            removed_mask[squash_cnt] = 1'b1;
                            squash_cnt = squash_cnt + 1'b1;
                            next_ptr = ROB_BIT'(next_ptr + 1'b1);
                        end
                    end
                end
            end
            else begin
                for (int i = 0; i < PPL_WIDTH; i++) begin
                    if (still_retiring && i < occupancy &&
                        rob_mem[ROB_BIT'(rob_head + i)].valid &&
                        rob_mem[ROB_BIT'(rob_head + i)].is_completed) begin
                        removed_indexes[i] = ROB_BIT'(rob_head + i);
                        removed_entries[i] = rob_mem[ROB_BIT'(rob_head + i)];
                        removed_mask[i] = 1'b1;
                        committed_mask[i] = 1'b1;
                        retire_cnt = retire_cnt + 1'b1;
                    end
                    else still_retiring = 1'b0;
                end
            end
        end
    end

    always_ff @(posedge clk) begin
        if (reset) begin
            for (int j = 0; j < ROB_SIZE; j++) rob_mem[j] <= '0;
            rob_head <= '0;
            rob_tail <= '0;
            occupancy <= '0;
            pending_result <= '0;
            flushing <= 1'b0;
            flush_ptr <= '0;
            flush_target <= '0;
        end
        else begin
            // Apply exactly the removal transaction presented during this cycle.
            for (int i = 0; i < PPL_WIDTH; i++) begin
                if (removed_mask[i]) begin
                    rob_mem[removed_indexes[i]].valid <= 1'b0;
                    pending_result[removed_indexes[i]] <= 1'b0;
                end
            end

            // A coincident flush retains these records only for cancellation reporting.
            if (!flushing) begin
                for (int i = 0; i < PPL_WIDTH; i++) begin
                    if (inserted_mask[i]) begin
                        rob_mem[inserted_index[i]] <= inserted_entries[i];
                        rob_mem[inserted_index[i]].valid <= 1'b1;
                        rob_mem[inserted_index[i]].is_completed <= 1'b0;
                        pending_result[inserted_index[i]] <= 1'b0;
                    end
                end
                rob_tail <= ROB_BIT'(rob_tail + insert_cnt);
            end

            // The supplied execution interface cancels older in-flight wrong-path work.
            if (flush_en) begin
                for (int j = 0; j < ROB_SIZE; j++) begin
                    if (killed[j]) pending_result[j] <= 1'b0;
                end
            end
            // Issues observed in the flush cycle are appended after that cancellation.
            for (int i = 0; i < PPL_WIDTH; i++) begin
                if (issued_mask[i])
                    pending_result[issued_entries[i].rob_index] <= 1'b1;
            end
            for (int i = 0; i < PPL_WIDTH; i++) begin
                if (executed_mask[i] && rob_mem[executed_index[i]].valid &&
                    rob_mem[executed_index[i]].preg == executed_preg[i])
                    pending_result[executed_index[i]] <= 1'b0;
                if (live_executed_mask[i])
                    rob_mem[executed_index[i]].is_completed <= 1'b1;
            end

            rob_head <= ROB_BIT'(rob_head + retire_cnt);
            occupancy <= occupancy + insert_cnt - retire_cnt - squash_cnt;

            if (flushing) begin
                flush_ptr <= next_ptr;
                if (!accepted_flush && next_ptr == rob_tail) begin
                    rob_tail <= flush_target;
                    flushing <= 1'b0;
                end
            end
            if (accepted_flush) begin
                flush_target <= ROB_BIT'(flush_index + 1'b1);
                flush_ptr <= ROB_BIT'(flush_index + 1'b1);
                flushing <= 1'b1;
            end
        end
    end

endmodule : ROB