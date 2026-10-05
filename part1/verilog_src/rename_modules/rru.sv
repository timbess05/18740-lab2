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

	logic [ARCH_REG-1:0][PHYS_BIT-1:0] specRAT;
	logic [ARCH_REG-1:0][PHYS_BIT-1:0] specRAT_working;
	logic [ARCH_REG-1:0][PHYS_BIT-1:0] archRAT_working;
	logic [PHYS_BIT-1:0] src1_preg;
	logic [PHYS_BIT-1:0] src2_preg;
	logic [ARCH_REG-1:0] rat_dirty;
	logic [PHYS_REG-1:0] preg_free;
	logic [PHYS_REG-1:0] preg_free_working, preg_claimable;
	instruction_t inserted_entry;

	logic [PHYS_BIT:0] occupancy;
	logic [$clog2(PPL_WIDTH+1)-1:0] commit_cnt, insert_cnt;

	assign commit_cnt = $countones(committed_mask);
	assign insert_cnt = $countones(inserted_mask);
	assign full = (PHYS_REG - occupancy) < PPL_WIDTH;

	assign stall = 1'b0;

	logic [PHYS_BIT:0] preg_start;

	always_comb begin
		for (int i = 0; i < PPL_WIDTH; i++) begin
			renamed_preg[i] = '0;
			renamed_rob_entries[i] = '0;
			renamed_iq_entries [i] = '0;
		end
		specRAT_working = specRAT;
		archRAT_working = archRAT;
		preg_free_working = preg_free;
		preg_claimable = preg_free;
		rat_dirty = '0;
		preg_start = '0;
		for (int i = 0; i < PPL_WIDTH; i++) begin
			if (committed_mask[i]) begin
				preg_free_working[archRAT_working[removed_areg[i]]] = 1'b1;
				archRAT_working[removed_areg[i]] = removed_preg[i];
			end
			if (inserted_mask[i]) begin
				inserted_entry = inserted_entries[i];
				src1_preg = specRAT_working[inserted_entry.src1];
				src2_preg = specRAT_working[inserted_entry.src2];

				renamed_iq_entries[i].src1 = src1_preg;
				renamed_iq_entries[i].src2 = src2_preg;
				if (!rat_dirty[inserted_entry.src1] && ((preg_states[src1_preg] == 2'b10) || (preg_states[src1_preg] == 2'b11))) begin
					renamed_iq_entries[i].src1_ready = 1'b1;
				end
				else begin
					renamed_iq_entries[i].src1_ready = 1'b0;
				end
				if (!rat_dirty[inserted_entry.src2] && ((preg_states[src2_preg] == 2'b10) || (preg_states[src2_preg] == 2'b11))) begin
					renamed_iq_entries[i].src2_ready = 1'b1;
				end
				else begin
					renamed_iq_entries[i].src2_ready = 1'b0;
				end
				
				for (int j = preg_start; j < PHYS_REG; j++) begin 
					if (preg_claimable[j]) begin
						preg_free_working[j] = 1'b0;
						preg_claimable[j] = 1'b0;
						specRAT_working[inserted_entry.dest] = j;
						rat_dirty[inserted_entry.dest] = 1'b1;
						renamed_preg[i] = j;
						renamed_rob_entries[i].areg = inserted_entry.dest; 
						renamed_rob_entries[i].preg = j;
						renamed_rob_entries[i].inst_ID = inserted_entry.inst_ID;
						renamed_rob_entries[i].is_branch = inserted_entry.is_branch;
						renamed_iq_entries[i].rob_index = inserted_index[i];
						renamed_iq_entries[i].inst_ID = inserted_entry.inst_ID;
						preg_start = j + 1;
						break;
					end
				end
			end
		end
	end

	always_ff @(posedge clk) begin
		if (reset) begin
			for (int i = 0; i < ARCH_REG; i++) specRAT[i] <= PHYS_BIT'(i);
			for (int i = 0; i < PHYS_REG; i++) begin
				if (i < ARCH_REG) preg_free[i] <= 1'b0;
				else preg_free[i] <= 1'b1;
			end
			occupancy <= ARCH_REG;
		end
		else begin
			specRAT <= specRAT_working;
			preg_free <= preg_free_working;
			occupancy <= occupancy - commit_cnt + insert_cnt;
		end
	end

endmodule: RRU
