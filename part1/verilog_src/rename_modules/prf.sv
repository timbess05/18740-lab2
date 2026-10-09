`default_nettype none

module PRF (
    input logic clk, reset,
    input logic [PPL_WIDTH-1:0] inserted_mask,
    input logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] inserted_preg,
    input logic [PPL_WIDTH-1:0] executed_mask,
    input logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] executed_preg,
    input logic [PPL_WIDTH-1:0] committed_mask,
    input logic [PPL_WIDTH-1:0] removed_mask,
    input logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] removed_preg,
    input logic [PPL_WIDTH-1:0][ARCH_BIT-1:0] removed_areg,
    output logic [PHYS_REG-1:0][1:0] preg_states,
    output logic [ARCH_REG-1:0][PHYS_BIT-1:0] archRAT
);

    typedef enum logic [1:0] {
        AVAILABLE = 2'b00,
        RENAMED_NOT_VALID = 2'b01,
        RENAMED_VALID = 2'b10,
        ARCHITECTURAL = 2'b11
    } preg_state_t;

    logic [ARCH_REG-1:0][PHYS_BIT-1:0] archRAT_working;
    logic [PHYS_REG-1:0][1:0] preg_states_working;

    always_comb begin
        archRAT_working = archRAT;
        preg_states_working = preg_states;
        for (int i = 0; i < PPL_WIDTH; i++) begin
            if (inserted_mask[i])
                preg_states_working[inserted_preg[i]] = RENAMED_NOT_VALID;
        end
        for (int i = 0; i < PPL_WIDTH; i++) begin
            if (executed_mask[i] && preg_states[executed_preg[i]] == RENAMED_NOT_VALID)
                preg_states_working[executed_preg[i]] = RENAMED_VALID;
        end
        // Removal has priority over every execution lane, not just the same lane.
        for (int i = 0; i < PPL_WIDTH; i++) begin
            if (committed_mask[i]) begin
                preg_states_working[archRAT_working[removed_areg[i]]] = AVAILABLE;
                archRAT_working[removed_areg[i]] = removed_preg[i];
                preg_states_working[removed_preg[i]] = ARCHITECTURAL;
            end
            else if (removed_mask[i]) begin
                preg_states_working[removed_preg[i]] = AVAILABLE;
            end
        end
    end

    always_ff @(posedge clk) begin
        if (reset) begin
            for (int i = 0; i < ARCH_REG; i++) archRAT[i] <= PHYS_BIT'(i);
            for (int i = 0; i < PHYS_REG; i++) begin
                if (i < ARCH_REG) preg_states[i] <= ARCHITECTURAL;
                else preg_states[i] <= AVAILABLE;
            end
        end
        else begin
            archRAT <= archRAT_working;
            preg_states <= preg_states_working;
        end
    end

endmodule : PRF