`default_nettype none

module RRU (

	// Clock and synchronous active high reset
	input  logic clk, reset,
	
	// Incoming entry signals
	input  logic [PPL_WIDTH-1:0] inserted_mask,
	input  instruction_t [PPL_WIDTH-1:0] inserted_entries,

	// Committed/Removed instruction signals
	input  logic [PPL_WIDTH-1:0] committed_mask,
	input  logic [PPL_WIDTH-1:0] removed_mask,
	input  logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] removed_preg,
	input  logic [PPL_WIDTH-1:0][ARCH_BIT-1:0] removed_areg,

	// Which removed entries were branches
	// A branch releases its checkpoint when it commits.
	input  logic [PPL_WIDTH-1:0] removed_is_branch,
	
	// Renamed entry signals
	output logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] renamed_preg,
	output rob_entry_t [PPL_WIDTH-1:0] renamed_rob_entries,
	output iq_entry_t [PPL_WIDTH-1:0] renamed_iq_entries,

	// PRF FSMs and archRAT are visible to RRU
	input  logic [PHYS_REG-1:0][1:0] preg_states,
	input  logic [ARCH_REG-1:0][PHYS_BIT-1:0] archRAT,

	// ROB slot assigned to each incoming instruction this cycle
	input  logic [PPL_WIDTH-1:0][ROB_BIT-1:0] inserted_index,

	// full flag
	output logic full,

	// Stall signal
	output logic stall,

	// Branch signals
	input  logic  flush_en,
	input  logic [ROB_BIT-1:0] flush_index
);

    /********************
    * ADD YOUR CODE HERE
    *********************/

endmodule: RRU
