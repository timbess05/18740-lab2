`default_nettype none

module PRF (

	// Clock and synchronous active high reset
	input  logic clk, reset,
	
	// Incoming entry signals
	input  logic [PPL_WIDTH-1:0] inserted_mask,
	input  logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] inserted_preg,

	// Executed instruction signals
	input  logic [PPL_WIDTH-1:0] executed_mask,
	input  logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] executed_preg,
	
	// Committed instruction signals
	input  logic [PPL_WIDTH-1:0] committed_mask,
	input  logic [PPL_WIDTH-1:0] removed_mask,
	input  logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] removed_preg,
	input  logic [PPL_WIDTH-1:0][ARCH_BIT-1:0] removed_areg,
	
	// PRF FSMs
	output logic [PHYS_REG-1:0][1:0] preg_states,

	// ArchRAT
	output logic [ARCH_REG-1:0][PHYS_BIT-1:0] archRAT
);

    /********************
    * ADD YOUR CODE HERE
    *********************/

endmodule: PRF
