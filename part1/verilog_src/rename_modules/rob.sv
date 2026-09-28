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

	rob_entry_t rob_mem [ROB_SIZE-1:0];
	logic [ROB_BIT-1:0] rob_tail;
	logic [ROB_BIT:0] occupancy;
	logic [$clog2(PPL_WIDTH+1)-1:0] retire_cnt, insert_cnt;
	logic still_retiring;

	assign full = (ROB_SIZE - occupancy) < PPL_WIDTH;
	assign insert_cnt = $countones(inserted_mask);

	always_comb begin
		removed_mask = '0;
		committed_mask = '0;
		removed_entries = '0;
		retire_cnt = '0;
		still_retiring = 1'b1;
		
		for (int i = 0; i < PPL_WIDTH; i++) begin
			inserted_index[i] = ROB_BIT'(rob_tail + i);

			if (rob_mem[ROB_BIT'(rob_head+i)].is_completed && rob_mem[ROB_BIT'(rob_head+i)].valid && still_retiring) begin
				removed_mask[i] = 1'b1;
				committed_mask[i] = 1'b1;
				removed_entries[i] = rob_mem[ROB_BIT'(rob_head+i)];
				retire_cnt = retire_cnt + 1'b1;
			end
			else begin
				still_retiring = 1'b0;
			end
		end
	end

	always_ff @(posedge clk) begin
		if (reset) begin
			for (int i = 0; i < ROB_SIZE; i++) rob_mem[i] <= '0;
			rob_head <= '0;
			rob_tail <= '0;
			occupancy <= '0;
		end
		else begin
			for (int i = 0; i < PPL_WIDTH; i++) begin
                if (removed_mask[i])
                    rob_mem[ROB_BIT'(rob_head + i)].valid <= 1'b0;
            end
			
			for (int i = 0; i < PPL_WIDTH; i++) begin
				if (inserted_mask[i]) begin
					rob_mem[ROB_BIT'(rob_tail + i)] <= inserted_entries[i];
					rob_mem[ROB_BIT'(rob_tail + i)].valid <= 1'b1;
					rob_mem[ROB_BIT'(rob_tail + i)].is_completed <= 1'b0;
				end
			end
			
			for (int i = 0; i < PPL_WIDTH; i++) begin
				if (executed_mask[i])
					rob_mem[executed_index[i]].is_completed <= 1'b1;
			end

			rob_head <= ROB_BIT'(rob_head + retire_cnt);
			rob_tail <= ROB_BIT'(rob_tail + insert_cnt);
			occupancy <= occupancy + insert_cnt - retire_cnt;
		end
	end

endmodule: ROB
