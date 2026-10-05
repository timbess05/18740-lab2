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
	iq_entry_t iq_mem [IQ_SIZE-1:0];
	iq_entry_t iq_mem_working [IQ_SIZE-1:0];
	logic [IQ_BIT:0] insert_start, issue_start;
	logic [$clog2(PPL_WIDTH+1)-1:0] insert_cnt, issue_cnt;
	logic [IQ_BIT:0] occupancy;

	assign insert_cnt = $countones(inserted_mask);
	assign full = (IQ_SIZE - occupancy) < PPL_WIDTH;

	always_comb begin
		iq_mem_working = iq_mem;
		insert_start = '0;
		issue_start = '0;
		issued_mask = '0;
		issued_entries = '0;
		issue_cnt = '0;
		for (int i = 0; i < PPL_WIDTH; i++) begin
			for (int j = issue_start; j < IQ_SIZE; j++) begin
				if (iq_mem[j].valid && iq_mem[j].src1_ready && iq_mem[j].src2_ready) begin
					issued_mask[i] = 1'b1;
					issued_entries[i] = iq_mem[j];
					iq_mem_working[j].valid = 1'b0;
					issue_start = j + 1'b1;
					issue_cnt = issue_cnt + 1'b1;
					break;
				end
			end
		end	
		for (int i = 0; i < PPL_WIDTH; i++) begin
			if (inserted_mask[i]) begin
				for (int j = insert_start; j < IQ_SIZE; j++) begin
					if (!iq_mem_working[j].valid) begin
						iq_mem_working[j] = inserted_entries[i];
						iq_mem_working[j].valid = 1'b1;
						insert_start = j + 1'b1;
						break;
					end
				end
			end
		end
		for (int i = 0; i < PPL_WIDTH; i++) begin
			if (executed_mask[i]) begin
				for (int j = 0; j < IQ_SIZE; j++) begin
					if (iq_mem_working[j].valid && (iq_mem_working[j].src1 == executed_preg[i])) begin
						iq_mem_working[j].src1_ready = 1'b1;
					end
					if (iq_mem_working[j].valid && (iq_mem_working[j].src2 == executed_preg[i])) begin
						iq_mem_working[j].src2_ready = 1'b1;
					end
				end
			end
		end
	end

	always_ff @(posedge clk) begin
		if (reset) begin
			for (int i = 0; i < IQ_SIZE; i++) iq_mem[i] <= '0;
			occupancy <= '0;
		end
		else begin
			iq_mem <= iq_mem_working;
			occupancy <= occupancy - issue_cnt + insert_cnt;
		end
	end	
endmodule: IQ
