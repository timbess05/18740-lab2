`default_nettype none

module ROB (

	// Clock and synchronous active high reset
	input  logic clk, reset,
	
	// Incoming entry signals
	input  logic [PPL_WIDTH-1:0] inserted_mask,
	input  rob_entry_t [PPL_WIDTH-1:0] inserted_entries,
	output logic [PPL_WIDTH-1:0][ROB_BIT-1:0] inserted_index,

	// Executed instruction signals
	input  logic [PPL_WIDTH-1:0] executed_mask,
	input  logic [PPL_WIDTH-1:0][ROB_BIT-1:0] executed_index,
	
	// Output entry signals
	output logic [PPL_WIDTH-1:0] removed_mask,
    output logic [PPL_WIDTH-1:0] committed_mask,
	output rob_entry_t [PPL_WIDTH-1:0] removed_entries,
	
	// Full flag
	output logic  full,

	// Branch signals
	input  logic  flush_en,
	input  logic [ROB_BIT-1:0] flush_index,

	// Current head of the ROB
	output logic [ROB_BIT-1:0] rob_head
);

    /********************
    * ADD YOUR CODE HERE
    *********************/

endmodule: ROB
