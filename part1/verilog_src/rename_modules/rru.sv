`default_nettype none

module RRU (
    input wire logic clk, reset,

    // Offers stay stable if a branch resolves later in this cycle.
    input wire logic [PPL_WIDTH-1:0] inserted_mask,
    input wire instruction_t [PPL_WIDTH-1:0] inserted_entries,
    input wire logic [PPL_WIDTH-1:0][ROB_BIT-1:0] inserted_index,

    input wire logic [PPL_WIDTH-1:0] committed_mask, removed_mask,
    input wire logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] removed_old_preg,
    input wire logic [PPL_WIDTH-1:0][ARCH_BIT-1:0] removed_areg,
    input wire logic [PHYS_REG-1:0][1:0] preg_states,

    output logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] renamed_preg,
    output logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] renamed_old_preg,
    output rob_entry_t [PPL_WIDTH-1:0] renamed_rob_entries,
    output iq_entry_t [PPL_WIDTH-1:0] renamed_iq_entries,
    output logic full, stall
);
    localparam int CW = $clog2(PPL_WIDTH + 1);
    localparam int OW = $clog2(PHYS_REG + 1);

    logic [ARCH_REG-1:0][PHYS_BIT-1:0] specRAT;
    logic [ARCH_REG-1:0][PHYS_BIT-1:0] map_stage [0:PPL_WIDTH];
    logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] cache;
    logic [CW-1:0] cache_count, insert_count, remove_count;
    logic [PHYS_BIT-1:0] scan_ptr;
    logic [OW-1:0] occupancy;
    logic probe_free;
    logic [CW-1:0] lane_rank [0:PPL_WIDTH-1];

    // Full describes actual allocated registers, not cached free-register offers.
    assign full = (PHYS_REG - occupancy) < PPL_WIDTH;
    // Scan one physical register per cycle until a complete offer bundle is ready.
    assign stall = cache_count != CW'(PPL_WIDTH);
    assign insert_count = CW'($countones(inserted_mask));
    assign remove_count = CW'($countones(removed_mask));

    always_comb begin
        probe_free = (preg_states[scan_ptr] == 2'b00);
        for (int k = 0; k < PPL_WIDTH; k++) begin
            if (k < cache_count && cache[k] == scan_ptr)
                probe_free = 1'b0;
        end
    end

    generate
        for (genvar lane = 0; lane < PPL_WIDTH; lane++) begin : gen_tags
            if (lane == 0) begin : gen_first
                assign lane_rank[lane] = '0;
            end else begin : gen_later
                assign lane_rank[lane] = CW'($countones(inserted_mask[lane-1:0]));
            end
            assign renamed_preg[lane] = inserted_mask[lane]
                                      ? cache[lane_rank[lane]] : '0;
        end
        // Only PPL_WIDTH shallow mapping stages; no ROB-indexed RAT snapshots.
        for (genvar a = 0; a < ARCH_REG; a++) begin : gen_map
            assign map_stage[0][a] = specRAT[a];
            for (genvar lane = 0; lane < PPL_WIDTH; lane++) begin : gen_lane
                assign map_stage[lane+1][a] =
                    inserted_mask[lane] && inserted_entries[lane].dest == ARCH_BIT'(a)
                    ? renamed_preg[lane] : map_stage[lane][a];
            end
            always_ff @(posedge clk) begin
                if (reset)
                    specRAT[a] <= PHYS_BIT'(a);
                // ROB rolls back youngest first, one record on lane zero per cycle.
                else if (removed_mask[0] && !committed_mask[0] &&
                         removed_areg[0] == ARCH_BIT'(a))
                    specRAT[a] <= removed_old_preg[0];
                else
                    specRAT[a] <= map_stage[PPL_WIDTH][a];
            end
        end
    endgenerate

    // Preserve exact source tags, including repeated writes within one bundle.
    always_comb begin
        renamed_old_preg = '0;
        renamed_rob_entries = '0;
        renamed_iq_entries = '0;
        for (int lane = 0; lane < PPL_WIDTH; lane++) begin
            if (inserted_mask[lane]) begin
                renamed_old_preg[lane] = map_stage[lane][inserted_entries[lane].dest];
                renamed_rob_entries[lane].areg = inserted_entries[lane].dest;
                renamed_rob_entries[lane].preg = renamed_preg[lane];
                renamed_rob_entries[lane].inst_ID = inserted_entries[lane].inst_ID;
                renamed_rob_entries[lane].is_branch = inserted_entries[lane].is_branch;
                renamed_iq_entries[lane].rob_index = inserted_index[lane];
                renamed_iq_entries[lane].inst_ID = inserted_entries[lane].inst_ID;
                renamed_iq_entries[lane].src1 = specRAT[inserted_entries[lane].src1];
                renamed_iq_entries[lane].src2 = specRAT[inserted_entries[lane].src2];
                renamed_iq_entries[lane].src1_ready =
                    preg_states[specRAT[inserted_entries[lane].src1]][1];
                renamed_iq_entries[lane].src2_ready =
                    preg_states[specRAT[inserted_entries[lane].src2]][1];
                for (int older = 0; older < PPL_WIDTH; older++) begin
                    if (older < lane && inserted_mask[older]) begin
                        if (inserted_entries[older].dest == inserted_entries[lane].src1) begin
                            renamed_iq_entries[lane].src1 = renamed_preg[older];
                            renamed_iq_entries[lane].src1_ready = 1'b0;
                        end
                        if (inserted_entries[older].dest == inserted_entries[lane].src2) begin
                            renamed_iq_entries[lane].src2 = renamed_preg[older];
                            renamed_iq_entries[lane].src2_ready = 1'b0;
                        end
                    end
                end
            end
        end
    end

    always_ff @(posedge clk) begin
        if (reset) begin
            cache <= '0;
            cache_count <= '0;
            scan_ptr <= PHYS_BIT'((ARCH_REG < PHYS_REG) ? ARCH_REG : 0);
            occupancy <= OW'(ARCH_REG);
        end else begin
            occupancy <= occupancy + OW'(insert_count) - OW'(remove_count);
            if (|inserted_mask) begin
                // Unused cached tags in a partial bundle were never allocated.
                cache_count <= '0;
            end else if (cache_count < CW'(PPL_WIDTH)) begin
                if (probe_free) begin
                    cache[cache_count] <= scan_ptr;
                    cache_count <= cache_count + 1'b1;
                end
                scan_ptr <= (scan_ptr == PHYS_BIT'(PHYS_REG-1))
                          ? '0 : scan_ptr + 1'b1;
            end
        end
    end
endmodule : RRU
