`default_nettype none

module RENAME_TOP (

    input  logic clk, reset,

    // Incoming instructions
    input  logic            [PPL_WIDTH-1:0] inserted_mask,
    input  instruction_t    [PPL_WIDTH-1:0] inserted_instructions,
    output logic            [PPL_WIDTH-1:0][ROB_BIT-1:0] inserted_index,

    // Renamed destination pregs for incoming instructions
    output logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] renamed_preg,

    // Executed instructions
    input  logic [PPL_WIDTH-1:0] executed_mask,
    input  logic [PPL_WIDTH-1:0][ROB_BIT-1:0] executed_index,
    input  logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] executed_preg,

    // Flushed instruction
    input  logic flush_en,
    input  logic [ROB_BIT-1:0] flush_index,

    // Issued instructions
    output logic [PPL_WIDTH-1:0] issued_mask,
    output iq_entry_t [PPL_WIDTH-1:0] issued_entries,

    // Committed instructions
    output logic [PPL_WIDTH-1:0] removed_mask,
    output logic [PPL_WIDTH-1:0] committed_mask,
    output logic [PPL_WIDTH-1:0][INST_BIT-1:0] removed_id,

    // PRF FSMs and archRAT
	  output logic [PHYS_REG-1:0][1:0] preg_states,
    output logic [ARCH_REG-1:0][PHYS_BIT-1:0] archRAT,

    output logic rob_full, iq_full, rru_full,

    // Top-level stall signal (use it optionally)
    output logic stall
);


    rob_entry_t [PPL_WIDTH-1:0] rob_inserted_entries;
    rob_entry_t [PPL_WIDTH-1:0] rob_removed_entries;

    iq_entry_t  [PPL_WIDTH-1:0] iq_renamed_entries;   // from the RRU, rob_index not yet known
    iq_entry_t  [PPL_WIDTH-1:0] iq_inserted_entries;  // fed to the IQ

    // ROB head, forwarded to the IQ so it can tell which of its entries are
    // younger than a mispredicting branch
    logic       [ROB_BIT-1:0] rob_head;

    // Which retiring entries were branches, so the RRU can release their
    // checkpoints
    logic       [PPL_WIDTH-1:0] rru_removed_is_branch;
    logic                       rru_stall;

    logic       [PPL_WIDTH-1:0][PHYS_BIT-1:0] prf_inserted_preg;
    logic       [PPL_WIDTH-1:0][PHYS_BIT-1:0] prf_removed_preg;
    logic       [PPL_WIDTH-1:0][ARCH_BIT-1:0] prf_removed_areg;

    // Wire global signals together
    always_comb begin
        for (int i = 0; i < PPL_WIDTH; i++) begin
            prf_inserted_preg[i]    = rob_inserted_entries[i].preg;
            prf_removed_preg[i]     = rob_removed_entries[i].preg;
            prf_removed_areg[i]     = rob_removed_entries[i].areg;
            removed_id[i]           = rob_removed_entries[i].inst_ID;
            rru_removed_is_branch[i] = rob_removed_entries[i].is_branch;

            // The RRU has no way to know which ROB slot an instruction lands
            // in, but the ROB allocates it combinationally in the same cycle,
            // so splice it into the IQ entry on the way past.
            iq_inserted_entries[i]              = iq_renamed_entries[i];
            iq_inserted_entries[i].rob_index    = inserted_index[i];
        end
    end

    // Any module that cannot accept a new batch for a reason its full flag does
    // not already express drives the top-level stall. Only the RRU has one --
    // the branch checkpoint stack. ROB and IQ capacity is fully covered by
    // rob_full and iq_full.
    assign stall = rru_stall;


    // Module instantiations
    RRU rru (.clk(clk), .reset(reset),
             .inserted_mask(inserted_mask), .inserted_entries(inserted_instructions),
             .committed_mask(committed_mask), .removed_mask(removed_mask),
             .removed_preg(prf_removed_preg), .removed_areg(prf_removed_areg),
             .removed_is_branch(rru_removed_is_branch), .stall(rru_stall),
             .renamed_preg(renamed_preg), .renamed_rob_entries(rob_inserted_entries), 
             .renamed_iq_entries(iq_renamed_entries),
             .preg_states(preg_states), .archRAT(archRAT), .inserted_index(inserted_index),
             .full(rru_full), .flush_en(flush_en), .flush_index(flush_index));

    PRF prf (.clk(clk), .reset(reset),
             .inserted_mask(inserted_mask), .inserted_preg(prf_inserted_preg),
             .executed_mask(executed_mask), .executed_preg(executed_preg),
             .removed_preg(prf_removed_preg), .removed_areg(prf_removed_areg), 
             .removed_mask(removed_mask), .committed_mask(committed_mask),
             .preg_states(preg_states), .archRAT(archRAT));

    ROB rob (.clk(clk), .reset(reset),
             .inserted_mask(inserted_mask), .inserted_entries(rob_inserted_entries), 
             .inserted_index(inserted_index),
             .executed_mask(executed_mask), .executed_index(executed_index),
             .removed_mask(removed_mask), .committed_mask(committed_mask), .removed_entries(rob_removed_entries),
             .full(rob_full), .flush_en(flush_en), .flush_index(flush_index), .rob_head(rob_head));

    IQ iq (.clk(clk), .reset(reset),
           .inserted_mask(inserted_mask), .inserted_entries(iq_inserted_entries),
           .executed_mask(executed_mask), .executed_preg(executed_preg),
           .issued_mask(issued_mask), .issued_entries(issued_entries),
           .full(iq_full),
           .flush_en(flush_en), .flush_index(flush_index), .rob_head(rob_head));


endmodule: RENAME_TOP
