`default_nettype none

module RENAME_TOP (
    input wire logic clk, reset,
    input wire logic [PPL_WIDTH-1:0] inserted_mask,
    input wire instruction_t [PPL_WIDTH-1:0] inserted_instructions,
    output logic [PPL_WIDTH-1:0][ROB_BIT-1:0] inserted_index,
    output logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] renamed_preg,
    input wire logic [PPL_WIDTH-1:0] executed_mask,
    input wire logic [PPL_WIDTH-1:0][ROB_BIT-1:0] executed_index,
    input wire logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] executed_preg,
    input wire logic flush_en,
    input wire logic [ROB_BIT-1:0] flush_index,
    output logic [PPL_WIDTH-1:0] issued_mask,
    output iq_entry_t [PPL_WIDTH-1:0] issued_entries,
    output logic [PPL_WIDTH-1:0] removed_mask, committed_mask,
    output logic [PPL_WIDTH-1:0][INST_BIT-1:0] removed_id,
    output logic [PHYS_REG-1:0][1:0] preg_states,
    output logic [ARCH_REG-1:0][PHYS_BIT-1:0] archRAT,
    output logic rob_full, iq_full, rru_full,
    output logic stall
);
    rob_entry_t [PPL_WIDTH-1:0] rob_insert, rob_remove;
    iq_entry_t [PPL_WIDTH-1:0] iq_insert;
    logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] old_preg, removed_old_preg, removed_preg;
    logic [PPL_WIDTH-1:0][ARCH_BIT-1:0] removed_areg;
    logic [PPL_WIDTH-1:0] offer_mask, live_insert_mask, live_executed_mask;
    logic [PPL_WIDTH-1:0] inserted_is_branch, issued_is_branch;
    logic [ROB_BIT-1:0] rob_head;
    logic rru_stall, iq_stall, rob_recovery, accepted_flush, issue_hold;

    // Stall is a function of registered resource/recovery state, not raw flush.
    assign stall = rru_stall || iq_stall || rob_recovery;
    assign offer_mask = (reset || stall || rob_full || iq_full || rru_full)
                      ? '0 : inserted_mask;
    // The fixed interface has already observed these destination offers before a
    // same-cycle flush. Retain them in the ROB only as cancellation records.
    assign live_insert_mask = accepted_flush ? '0 : offer_mask;
    always_comb begin
        for (int lane = 0; lane < PPL_WIDTH; lane++) begin
            removed_preg[lane] = rob_remove[lane].preg;
            removed_areg[lane] = rob_remove[lane].areg;
            removed_id[lane] = rob_remove[lane].inst_ID;
            inserted_is_branch[lane] = inserted_instructions[lane].is_branch;
        end
    end

    RRU rru (
        .clk(clk), .reset(reset), .inserted_mask(offer_mask),
        .inserted_entries(inserted_instructions), .inserted_index(inserted_index),
        .committed_mask(committed_mask), .removed_mask(removed_mask),
        .removed_old_preg(removed_old_preg), .removed_areg(removed_areg),
        .preg_states(preg_states), .renamed_preg(renamed_preg),
        .renamed_old_preg(old_preg), .renamed_rob_entries(rob_insert),
        .renamed_iq_entries(iq_insert), .full(rru_full), .stall(rru_stall)
    );
    ROB rob (
        .clk(clk), .reset(reset), .inserted_mask(offer_mask),
        .inserted_entries(rob_insert), .inserted_old_preg(old_preg),
        .inserted_index(inserted_index), .executed_mask(executed_mask),
        .executed_index(executed_index), .executed_preg(executed_preg),
        .live_executed_mask(live_executed_mask), .issued_mask(issued_mask),
        .issued_is_branch(issued_is_branch), .issued_entries(issued_entries),
        .issue_hold(issue_hold), .removed_mask(removed_mask),
        .committed_mask(committed_mask), .removed_entries(rob_remove),
        .removed_old_preg(removed_old_preg), .full(rob_full),
        .recovery_busy(rob_recovery), .flush_en(flush_en), .flush_index(flush_index),
        .accepted_flush(accepted_flush), .rob_head(rob_head)
    );
    IQ iq (
        .clk(clk), .reset(reset), .inserted_mask(live_insert_mask),
        .inserted_entries(iq_insert), .inserted_is_branch(inserted_is_branch),
        .executed_mask(live_executed_mask), .executed_preg(executed_preg),
        .issue_hold(issue_hold), .issued_mask(issued_mask),
        .issued_is_branch(issued_is_branch), .issued_entries(issued_entries),
        .full(iq_full), .stall(iq_stall), .flush_en(accepted_flush),
        .flush_index(flush_index), .rob_head(rob_head)
    );
    PRF prf (
        .clk(clk), .reset(reset), .inserted_mask(offer_mask),
        .inserted_preg(renamed_preg), .executed_mask(live_executed_mask),
        .executed_preg(executed_preg), .committed_mask(committed_mask),
        .removed_mask(removed_mask), .removed_preg(removed_preg),
        .removed_areg(removed_areg), .preg_states(preg_states), .archRAT(archRAT)
    );
endmodule : RENAME_TOP
