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

	typedef enum logic [1:0] {
		AVAILABLE         = 2'b00,
		RENAMED_NOT_VALID = 2'b01,
		RENAMED_VALID     = 2'b10,
		ARCHITECTURAL     = 2'b11	
	} preg_state_t;

	logic [ARCH_REG-1:0][PHYS_BIT-1:0] archRAT_working; 

	always_ff @(posedge clk) begin
		if (reset) begin
			for (int i = 0; i < ARCH_REG; i++) archRAT[i] <= PHYS_BIT'(i);
			for (int i = 0; i < PHYS_REG; i++) begin
				if (i < ARCH_REG) begin
					preg_states[i] <= ARCHITECTURAL;
				end
				else begin
					preg_states[i] <= AVAILABLE;
				end
			end
		end
		else begin
			archRAT_working = archRAT;
			for (int i = 0; i < PPL_WIDTH; i++) begin 
				if (inserted_mask[i]) begin
					preg_states[inserted_preg[i]] <= RENAMED_NOT_VALID;
				end
				if (executed_mask[i]) begin
					preg_states[executed_preg[i]] <= RENAMED_VALID;
				end
				if (committed_mask[i]) begin
					preg_states[archRAT_working[removed_areg[i]]] <= AVAILABLE;
					archRAT_working[removed_areg[i]] = removed_preg[i];
					preg_states[removed_preg[i]] <= ARCHITECTURAL;
				end
			end
			archRAT <= archRAT_working;
		end
	end

endmodule: PRF
