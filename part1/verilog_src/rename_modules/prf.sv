`default_nettype none

module PRF (
    input wire logic clk, reset,
    input wire logic [PPL_WIDTH-1:0] inserted_mask,
    input wire logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] inserted_preg,
    input wire logic [PPL_WIDTH-1:0] executed_mask,
    input wire logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] executed_preg,
    input wire logic [PPL_WIDTH-1:0] committed_mask, removed_mask,
    input wire logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] removed_preg,
    input wire logic [PPL_WIDTH-1:0][ARCH_BIT-1:0] removed_areg,
    output logic [PHYS_REG-1:0][1:0] preg_states,
    output logic [ARCH_REG-1:0][PHYS_BIT-1:0] archRAT
);
    localparam logic [1:0] AVAILABLE = 2'b00;
    localparam logic [1:0] RENAMED_NOT_VALID = 2'b01;
    localparam logic [1:0] RENAMED_VALID = 2'b10;
    localparam logic [1:0] ARCHITECTURAL = 2'b11;
    logic [PHYS_BIT-1:0] release_preg;

    // The area-first ROB emits only lane zero for both commit and undo.
    assign release_preg = committed_mask[0]
                        ? archRAT[removed_areg[0]] : removed_preg[0];
    generate
        for (genvar a = 0; a < ARCH_REG; a++) begin : gen_arch
            always_ff @(posedge clk) begin
                if (reset) archRAT[a] <= PHYS_BIT'(a);
                else if (committed_mask[0] && removed_areg[0] == ARCH_BIT'(a))
                    archRAT[a] <= removed_preg[0];
            end
        end
        for (genvar p = 0; p < PHYS_REG; p++) begin : gen_preg
            logic [PPL_WIDTH-1:0] insert_hit, execute_hit;
            logic [1:0] next_state;
            for (genvar lane = 0; lane < PPL_WIDTH; lane++) begin : gen_hits
                assign insert_hit[lane] = inserted_mask[lane] && inserted_preg[lane] == PHYS_BIT'(p);
                assign execute_hit[lane] = executed_mask[lane] && executed_preg[lane] == PHYS_BIT'(p);
            end
            always_comb begin
                next_state = preg_states[p];
                if (|insert_hit) next_state = RENAMED_NOT_VALID;
                if ((|execute_hit) && preg_states[p] == RENAMED_NOT_VALID)
                    next_state = RENAMED_VALID;
                // Reclamation and commitment take priority over every response lane.
                if (removed_mask[0] && release_preg == PHYS_BIT'(p))
                    next_state = AVAILABLE;
                if (committed_mask[0] && removed_preg[0] == PHYS_BIT'(p))
                    next_state = ARCHITECTURAL;
            end
            always_ff @(posedge clk) begin
                if (reset) preg_states[p] <= (p < ARCH_REG) ? ARCHITECTURAL : AVAILABLE;
                else preg_states[p] <= next_state;
            end
        end
    endgenerate
endmodule : PRF
