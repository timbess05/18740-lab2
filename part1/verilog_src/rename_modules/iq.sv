`default_nettype none

module IQ (

	// Clock and synchronous active high reset
	input  logic clk, reset,
	
	// Incoming entry signals
	input  logic [PPL_WIDTH-1:0] inserted_mask,
	input  iq_entry_t [PPL_WIDTH-1:0] inserted_entries,

	// Executed instruction signals
	input  logic [PPL_WIDTH-1:0] executed_mask,
	input  logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] executed_preg,
	
	// Issued entry signals
	output logic [PPL_WIDTH-1:0] issued_mask,
    output iq_entry_t [PPL_WIDTH-1:0] issued_entries,
	
	// Full flag
	output logic  full,

	// Branch signals
	input  logic  flush_en,
	input  logic [ROB_BIT-1:0] flush_index,
	input  logic [ROB_BIT-1:0] rob_head
);

    /********************
    * ADD YOUR CODE HERE
    *********************/

endmodule: IQ
