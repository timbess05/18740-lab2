`default_nettype none

module RENAME_TOP (
    input logic clk, reset,
    input logic [PPL_WIDTH-1:0] inserted_mask,
    input instruction_t [PPL_WIDTH-1:0] inserted_instructions,
    output logic [PPL_WIDTH-1:0][ROB_BIT-1:0] inserted_index,
    output logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] renamed_preg,
    input logic [PPL_WIDTH-1:0] executed_mask,
    input logic [PPL_WIDTH-1:0][ROB_BIT-1:0] executed_index,
    input logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] executed_preg,
    input logic flush_en,
    input logic [ROB_BIT-1:0] flush_index,
    output logic [PPL_WIDTH-1:0] issued_mask,
    output iq_entry_t [PPL_WIDTH-1:0] issued_entries,
    output logic [PPL_WIDTH-1:0] removed_mask,
    output logic [PPL_WIDTH-1:0] committed_mask,
    output logic [PPL_WIDTH-1:0][INST_BIT-1:0] removed_id,
    output logic [PHYS_REG-1:0][1:0] preg_states,
    output logic [ARCH_REG-1:0][PHYS_BIT-1:0] archRAT,
    output logic rob_full, iq_full, rru_full,
    output logic stall
);

    rob_entry_t [PPL_WIDTH-1:0] rob_inserted_entries, rob_removed_entries;
    iq_entry_t [PPL_WIDTH-1:0] iq_inserted_entries;
    logic [ROB_BIT-1:0] rob_head;
    logic [PPL_WIDTH-1:0] rru_removed_is_branch;
    logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] prf_removed_preg;
    logic [PPL_WIDTH-1:0][ARCH_BIT-1:0] prf_removed_areg;
    logic rru_stall, rob_stall, accepted_flush;
    logic [PPL_WIDTH-1:0] offer_mask, live_dispatch_mask, live_executed_mask;
    logic [ROB_SIZE-1:0] blocked_branches;

    assign stall = rru_stall || rob_stall;

    // Keep register offers stable for the whole cycle. A late flush cancels
    // execution/mapping effects, but its offered allocations still need release.
    assign offer_mask = (reset || stall || rob_full || iq_full || rru_full)
                      ? '0 : inserted_mask;
    assign live_dispatch_mask = flush_en ? '0 : offer_mask;

    always_comb begin
        for (int i = 0; i < PPL_WIDTH; i++) begin
            prf_removed_preg[i] = rob_removed_entries[i].preg;
            prf_removed_areg[i] = rob_removed_entries[i].areg;
            removed_id[i] = rob_removed_entries[i].inst_ID;
            rru_removed_is_branch[i] = rob_removed_entries[i].is_branch;
        end
    end

    RRU rru (
        .clk(clk), .reset(reset),
        .inserted_mask(offer_mask), .inserted_entries(inserted_instructions),
        .committed_mask(committed_mask), .removed_mask(removed_mask),
        .removed_preg(prf_removed_preg), .removed_areg(prf_removed_areg),
        .removed_is_branch(rru_removed_is_branch),
        .renamed_preg(renamed_preg), .renamed_rob_entries(rob_inserted_entries),
        .renamed_iq_entries(iq_inserted_entries),
        .preg_states(preg_states), .archRAT(archRAT),
        .inserted_index(inserted_index), .full(rru_full), .stall(rru_stall),
        .flush_en(accepted_flush), .flush_index(flush_index)
    );

    PRF prf (
        .clk(clk), .reset(reset),
        .inserted_mask(offer_mask), .inserted_preg(renamed_preg),
        .executed_mask(live_executed_mask), .executed_preg(executed_preg),
        .committed_mask(committed_mask), .removed_mask(removed_mask),
        .removed_preg(prf_removed_preg), .removed_areg(prf_removed_areg),
        .preg_states(preg_states), .archRAT(archRAT)
    );

    ROB rob (
        .clk(clk), .reset(reset),
        .inserted_mask(offer_mask), .inserted_entries(rob_inserted_entries),
        .inserted_index(inserted_index),
        .executed_mask(executed_mask), .executed_index(executed_index),
        .executed_preg(executed_preg), .live_executed_mask(live_executed_mask),
        .issued_mask(issued_mask), .issued_entries(issued_entries),
        .blocked_branches(blocked_branches),
        .removed_mask(removed_mask), .committed_mask(committed_mask),
        .removed_entries(rob_removed_entries), .full(rob_full),
        .flush_en(flush_en), .flush_index(flush_index),
        .accepted_flush(accepted_flush), .recovery_busy(),
        .rob_stall(rob_stall), .rob_head(rob_head)
    );

    IQ iq (
        .clk(clk), .reset(reset),
        .inserted_mask(live_dispatch_mask), .inserted_entries(iq_inserted_entries),
        .executed_mask(live_executed_mask), .executed_preg(executed_preg),
        .issued_mask(issued_mask), .issued_entries(issued_entries),
        .full(iq_full), .flush_en(accepted_flush), .flush_index(flush_index),
        .rob_head(rob_head), .blocked_branches(blocked_branches)
    );

endmodule : RENAME_TOP