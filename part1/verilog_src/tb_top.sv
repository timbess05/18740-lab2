`default_nettype none


module TB_TOP ();

    logic clk, reset;

    // Incoming instruction signals
    logic [PPL_WIDTH-1:0]               inserted_mask;
    instruction_t [PPL_WIDTH-1:0]       inserted_instructions;
    logic [PPL_WIDTH-1:0][ROB_BIT-1:0]  inserted_rob_index;
    logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] inserted_renamed_preg;

    // Executed instruction signals
    logic [PPL_WIDTH-1:0]               executed_mask;
    logic [PPL_WIDTH-1:0][ROB_BIT-1:0]  executed_rob_index;
    logic [PPL_WIDTH-1:0][PHYS_BIT-1:0] executed_preg;

    // Issued instruction signals
    logic [PPL_WIDTH-1:0]               issued_mask;
    iq_entry_t [PPL_WIDTH-1:0]          issued_iq_entries;

    // Committed instruction signals
    logic [PPL_WIDTH-1:0]               removed_mask, committed_mask;
    logic [PPL_WIDTH-1:0][INST_BIT-1:0] removed_inst_id;

    // PRF FSMs and archRAT
    logic [PHYS_REG-1:0][1:0]           preg_states;
    logic [ARCH_REG-1:0][PHYS_BIT-1:0]  archRAT;

    // Full flags
    logic                               rob_full, iq_full, rru_full;

    // Flush signals
    logic                               flush_en;
    logic [ROB_BIT-1:0]                 flush_rob_index;

    // Top-level stall signal
    logic                               stall;

    // Top-level module instantiation
    RENAME_TOP dut (.clk(clk), .reset(reset), .inserted_mask(inserted_mask), .inserted_instructions(inserted_instructions), 
                    .inserted_index(inserted_rob_index), .renamed_preg(inserted_renamed_preg),
                    .executed_mask(executed_mask), .executed_index(executed_rob_index), .executed_preg(executed_preg),
                    .issued_mask(issued_mask), .issued_entries(issued_iq_entries),
                    .removed_mask(removed_mask), .committed_mask(committed_mask), .removed_id(removed_inst_id),
                    .preg_states(preg_states), .archRAT(archRAT), .flush_en(flush_en), .flush_index(flush_rob_index),
                    .rob_full(rob_full), .iq_full(iq_full), .rru_full(rru_full), .stall(stall));

    // GLOBAL VARIABLES
    instruction_t    dispatch_entry_queue [$];  // Holds instructions to be dispatched to RRU/ROB/IQ
    executed_entry_t executed_entry_queue [$];  // Keeps track of executing instructions

    scoreboard_entry_t  scoreboard[];                   // Holds scoreboard information for tb checking
    areg_map_t          producer_map;                   // For each areg, keeps track of instruction producing its value
    areg_map_t          producer_map_checkpoints[int];  // Holds checkpoints by inst_ID when branches show up
    bit                 preg_avail[PHYS_REG];           // For each preg, keeps track of whether it is free or not
    int                 cycle_counter;                  // Keeps track of global clock cycles
    int                 full_counter;                   // Keeps track of cycles where RRU/ROB/IQ are full
    int                 stall_counter;                  // Keeps track of cycles where RENAME_TOP was stalled
    int                 prf_mismatch[];                 // Keeps track of the cycles when the PRF FSMs did not match the ArchRAT
    int                 prf_num_reg_err[];              // Keeps track of the cycles when the PRF doesn't have exactly ARCH_REG aregs
    int                 bad_preg_alloc[];               // Keeps track of the cycles when the RRU allocated a preg already being used
    int                 dut_index_errors;               // Counts x/out-of-range inst_IDs and pregs handed back by the DUT

    localparam MAX_INDEX_ERR_DISPLAY = 20;              // Cap on printed index errors, so a broken DUT cannot flood the log

    // Timing offsets within a cycle. The clock period is 10, so both of these
    // land comfortably between one posedge and the next.
    localparam DRIVE_DELAY  = 1;                        // Step off the clock edge before driving the DUT
    localparam COMB_SETTLE  = 1;                        // Let combinational DUT outputs settle before sampling them


    // DISPLAY FUNCTIONS

    // Display all scoreboard entries
    function display_scoreboard();
    scoreboard_entry_t scoreboard_entry;
        $display("===================================================");
        $display("Displaying Scoreboard...");
        $display("TS means timestamp. TS of -1 means that event never happened");
        $display("===================================================");
        $display("|  ID  | SRC 1 | SRC 2 | PHYS | ARCH | DISPATCH_TS | ISSUED_TS    | EXECUTED_TS | COMMITTED_TS | REMOVED_TS | FLUSHED_TS  |");

        for (int i = 0; i < scoreboard.size(); i++) begin
        scoreboard_entry = scoreboard[i];

        $display("| %d | %d   | %d   | %d  | %d   | %d | %d  | %d | %d  | %d | %d |",
                scoreboard_entry.inst_ID, scoreboard_entry.src1_areg, scoreboard_entry.src2_areg,
                scoreboard_entry.preg, scoreboard_entry.areg,
                scoreboard_entry.dispatched_timestamp, scoreboard_entry.issued_timestamp,
                scoreboard_entry.executed_timestamp, scoreboard_entry.committed_timestamp,
                scoreboard_entry.removed_timestamp, scoreboard_entry.flushed_timestamp);
        end
    endfunction

    // Display renamed register states
    function display_renameRegs();
        $display("===================================================");
        $display("Displaying Rename Register States...");
        $display("STATE=0: Available; STATE=1: Renamed Not Valid; STATE=2: Renamed Valid; STATE=3: Architectural");
        $display("===================================================");
        $display("|  PHYS REG   |  STATE  |");

        for (int i = 0; i < PHYS_REG; i++) begin
            $display("| %d |   %d     |", i, preg_states[i]);
        end
    endfunction

    // Display architectural RAT
    function display_archRAT();
        $display("===================================================");
        $display("Displaying Architectural RAT...");
        $display("|  ARCH REG   |  PHYS REG  |");

        for (int i = 0; i < ARCH_REG; i++) begin
            $display("| %d |   %d     |", i, archRAT[i]);
        end
    endfunction


    // CHECK FUNCTIONS

    // ROB ==========

    // == COMMIT IN-ORDER ==
    // Check that each instruction captured by the 
    // scoreboard committed in order
    function int commit_in_order_check();
        commit_in_order_check = 1;
        for (int i = 1; i < scoreboard.size(); i++) begin
            if (scoreboard[i].flushed_timestamp == -1  && scoreboard[i-1].flushed_timestamp == -1) begin
                if (scoreboard[i].committed_timestamp < scoreboard[i-1].committed_timestamp) begin
                    $display("[ERROR] (Instruction %d) is committed before (Instruction %d)", i, i-1);
                    commit_in_order_check = 0;
                end
            end
        end
    endfunction

    // == NO DOUBLE COMMITS ==
    // Check that each instruction committed 
    // only once: in this scenario, all instructions
    // are unique
    function int commit_once_check();
        commit_once_check = 1;
        for (int i = 0; i < scoreboard.size(); i++) begin
            if (scoreboard[i].flushed_timestamp == -1) begin
                if (scoreboard[i].committed_timestamp != -1 && scoreboard[i].commit_count > 1) begin
                    $display("[ERROR] (Instruction %d) committed more than once (%d times)", i, scoreboard[i].commit_count);
                    commit_once_check = 0;
                end
            end
        end
    endfunction

    // == EVERYTHING COMMITTED ==
    // Check that all instructions that
    // weren't flushes were committed
    function int everything_committed_check();
        everything_committed_check = 1;
        for (int i = 0; i < scoreboard.size(); i++) begin
            if (scoreboard[i].flushed_timestamp == -1) begin
                if (scoreboard[i].committed_timestamp == -1) begin
                    $display("[ERROR] (Instruction %d) never committed", i);
                    everything_committed_check = 0;
                end
            end
        end
    endfunction


    // IQ ===========

    // == ISSUE BEFORE COMMIT ==
    // Check that each instruction captured
    // by the scoreboard issued before they committed
    function int issue_before_commit_check();
        issue_before_commit_check = 1;
        for (int i = 0; i < scoreboard.size(); i++) begin
            // Only meaningful once both events actually happened. A -1 on
            // either side just means the instruction never got that far, which
            // everything_issued_check / everything_committed_check report.
            if (scoreboard[i].flushed_timestamp  == -1 &&
                scoreboard[i].issued_timestamp   != -1 &&
                scoreboard[i].committed_timestamp != -1) begin
                    if (scoreboard[i].issued_timestamp >= scoreboard[i].committed_timestamp) begin
                    $display("[ERROR] (Instruction %d) is committed before or same cycle as issued", i);
                    issue_before_commit_check = 0;
                end
            end
        end
    endfunction

    // == ISSUED AFTER DEPENDENCIES ==
    // Check that each instruction captured by
    // the scoreboard was issued after both of its dependencies
    // were met
    function int issue_after_dependencies_check();
        int source1_id;
        int source2_id;
        issue_after_dependencies_check = 1;
        for (int i = 0; i < scoreboard.size(); i++) begin
            // An instruction that never issued has no ordering to check here.
            // everything_issued_check reports that case on its own; repeating it
            // would bury the real violations under one error per source operand.
            if (scoreboard[i].flushed_timestamp == -1 && scoreboard[i].issued_timestamp != -1) begin
                source1_id = scoreboard[i].src1_ID;
                source2_id = scoreboard[i].src2_ID;

                // A source ID of -1 means no in-trace producer wrote that architectural
                // register: the value is architectural from reset and always ready, so
                // there is no ordering requirement to check. Producers that were
                // themselves flushed are likewise skipped.
                if (source1_id != -1 && scoreboard[source1_id].flushed_timestamp == -1) begin
                    if (scoreboard[source1_id].executed_timestamp == -1) begin
                        $display("[ERROR] (Instruction %d) issued but its (Source1 producer %d) never executed",
                                 scoreboard[i].inst_ID, source1_id);
                        issue_after_dependencies_check = 0;
                    end
                    else if (scoreboard[i].issued_timestamp <= scoreboard[source1_id].executed_timestamp) begin
                        $display("[ERROR] (Instruction %d) with (Source1 %d) was issued before or same cycle as (Instruction %d) with (Dest %d) was executed",
                                 scoreboard[i].inst_ID, scoreboard[i].src1_preg, source1_id, scoreboard[source1_id].preg);
                        issue_after_dependencies_check = 0;
                    end
                end

                if (source2_id != -1 && scoreboard[source2_id].flushed_timestamp == -1) begin
                    if (scoreboard[source2_id].executed_timestamp == -1) begin
                        $display("[ERROR] (Instruction %d) issued but its (Source2 producer %d) never executed",
                                 scoreboard[i].inst_ID, source2_id);
                        issue_after_dependencies_check = 0;
                    end
                    else if (scoreboard[i].issued_timestamp <= scoreboard[source2_id].executed_timestamp) begin
                        $display("[ERROR] (Instruction %d) with (Source2 %d) was issued before or same cycle as (Instruction %d) with (Dest %d) was executed",
                                 scoreboard[i].inst_ID, scoreboard[i].src2_preg, source2_id, scoreboard[source2_id].preg);
                        issue_after_dependencies_check = 0;
                    end
                end
            end
        end
    endfunction

    // == NO DOUBLE ISSUES ==
    // Check that all instruction IDs 
    // issue only once
    function int issue_once_check();
        issue_once_check = 1;
        for (int i = 0; i < scoreboard.size(); i++) begin
            if (scoreboard[i].flushed_timestamp == -1) begin
                if (scoreboard[i].issue_count > 1) begin
                    $display("[ERROR] (Instruction %d) issued more than once (%d times)", i, scoreboard[i].issue_count);
                    issue_once_check = 0;
                end
            end
        end
    endfunction

    // == EVERYTHING ISSUED ==
    // Check that by the end of the trace
    // all instructions issued
    function int everything_issued_check();
        everything_issued_check = 1;
        for (int i = 0; i < scoreboard.size(); i++) begin
            if (scoreboard[i].flushed_timestamp == -1) begin
                if (scoreboard[i].issued_timestamp == -1) begin
                    $display("[ERROR] (Instruction %d) never issued", i);
                    everything_issued_check = 0;
                end
            end
        end
    endfunction

    // PRF ==========

    // == CORRECT NUMBER OF AREGS ==
    // Check that PRF FSMs always point to 
    // at most 32 registers
    logic [ARCH_BIT:0] num_archRegs;
    always_comb begin
        num_archRegs = '0;
        for (int i = 0; i < PHYS_REG; i++) begin
            if (preg_states[i] == 2'b11) num_archRegs += 'd1;
        end
    end

    // disable iff (reset): preg_states is still x on the very first clock edge,
    // before the PRF's reset assignment lands, so the property would always
    // report one spurious failure at time 0 on every run.
    assert property (@(posedge clk) disable iff (reset) num_archRegs == ARCH_REG)
    else begin
        prf_num_reg_err = new[prf_num_reg_err.size() + 1](prf_num_reg_err);
        prf_num_reg_err[prf_num_reg_err.size() - 1] = cycle_counter;
    end

    function int num_archRegs_check();
        num_archRegs_check = 1;
        if (prf_num_reg_err.size() > 0) begin
            $display("[ERROR] Number of architectural registers was not equal to %d, %d times", ARCH_REG, prf_num_reg_err.size());
            $display("==================");
            for (int i = 0; i < prf_num_reg_err.size(); i++) begin
                $display("Clock cycle : %d", prf_num_reg_err[i]);
            end
            num_archRegs_check = 0;
        end
    endfunction

    // == PRF ARCHRAT MATCH ==
    // Check that the architectural RAT
    // matches the rename FSM
    genvar i_match;
    generate
        for (i_match = 0; i_match < ARCH_REG; i_match++) begin: match_statement
        
            assert property (@(posedge clk) disable iff (reset) preg_states[archRAT[i_match]] == 2'b11)
            else begin
                prf_mismatch = new[prf_mismatch.size() + 1](prf_mismatch);
                prf_mismatch[prf_mismatch.size() - 1] = cycle_counter;
            end
        end
    endgenerate

    function int archRAT_check();
        archRAT_check = 1;
        if (prf_mismatch.size() > 0) begin
            $display("[ERROR] Architectural RAT did not match Physical Register States %d times:", prf_mismatch.size());
            $display("==================");
            for (int i = 0; i < prf_mismatch.size(); i++) begin
                $display("Clock cycle : %d", prf_mismatch[i]);
            end
            archRAT_check = 0;
        end
    endfunction

    // RRU ==========

    // == CORRECT DEPENDENCIES ==
    // Check that each instruction 
    // used the correct src pregs
    function int correct_dependencies_check();
        int source1_id;
        int source2_id;
        correct_dependencies_check = 1;
        for (int i = 0; i < scoreboard.size(); i++) begin
            if (scoreboard[i].flushed_timestamp == -1) begin
                source1_id = scoreboard[i].src1_ID;
                source2_id = scoreboard[i].src2_ID;

                if (source1_id != -1) begin
                    if (scoreboard[source1_id].preg != scoreboard[i].src1_preg) begin
                        $display("[ERROR] (Instruction %d) recorded Source 1 physical register (%d) does not match with expected (%d)", 
                                i, scoreboard[i].src1_preg, scoreboard[source1_id].preg);
                        correct_dependencies_check = 0;
                    end
                end
                
                if (source2_id != -1) begin
                    if (scoreboard[source2_id].preg != scoreboard[i].src2_preg) begin
                        $display("[ERROR] (Instruction %d) recorded Source 2 physical register (%d) does not match with expected (%d)", 
                                i, scoreboard[i].src2_preg, scoreboard[source2_id].preg);
                        correct_dependencies_check = 0;
                    end
                end
                
            end
        end
    endfunction

    // == CORRECT PREG REUSE ==
    // Check that instructions don't
    // reuse unavailable pregs
    function int correct_preg_reuse_check();
        correct_preg_reuse_check = 1;
        if (bad_preg_alloc.size() > 0) begin
            $display("[ERROR] Illegally used unavailable registers %d times during the following cycles:", bad_preg_alloc.size());
            $display("==================");
            for (int i = 0; i < bad_preg_alloc.size(); i++) begin
                $display("Clock cycle : %d", bad_preg_alloc[i]);
            end
            correct_preg_reuse_check = 0;
        end
    endfunction


    // FLUSH TESTS

    // == FLUSHED PREGS RECLAIMED ==
    // Once the pipe has drained, every physical register should have settled:
    // either Architectural (2'b11) for the 32 backing the archRAT, or Available
    // (2'b00). Anything still in a Renamed state was never reclaimed, and the
    // usual cause is a squashed instruction whose register was dropped rather
    // than returned to the free list.
    function int flushed_pregs_reclaimed_check();
        flushed_pregs_reclaimed_check = 1;
        for (int i = 0; i < PHYS_REG; i++) begin
            if (preg_states[i] == 2'b01 || preg_states[i] == 2'b10) begin
                $display("[ERROR] (Physical register %d) is still renamed (state %b) after the pipes drained",
                         i, preg_states[i]);
                flushed_pregs_reclaimed_check = 0;
            end
        end
    endfunction

    // == FLUSHED INSTRUCTIONS NEVER COMMIT ==
    // A squashed instruction must leave the ROB through removed_mask with its
    // committed_mask bit low, so it never reaches the archRAT.
    function int flushed_not_committed_check();
        flushed_not_committed_check = 1;
        for (int i = 0; i < scoreboard.size(); i++) begin
            if (scoreboard[i].flushed_timestamp != -1 &&
                scoreboard[i].committed_timestamp != -1) begin
                $display("[ERROR] (Instruction %d) was flushed at cycle %d but committed at cycle %d",
                         i, scoreboard[i].flushed_timestamp, scoreboard[i].committed_timestamp);
                flushed_not_committed_check = 0;
            end
        end
    endfunction

    // == FLUSHED INSTRUCTIONS NEVER ISSUE AFTER THE FLUSH ==
    // Issuing *before* the branch resolves is legal -- that is what speculation
    // is, and the testbench models those executions. What must not happen is an
    // entry leaving the IQ after it has been squashed, because by then the RRU
    // may have recycled its destination register to a different instruction.
    function int flushed_not_issued_check();
        flushed_not_issued_check = 1;
        for (int i = 0; i < scoreboard.size(); i++) begin
            if (scoreboard[i].flushed_timestamp != -1 &&
                scoreboard[i].issued_timestamp  != -1 &&
                scoreboard[i].issued_timestamp > scoreboard[i].flushed_timestamp) begin
                $display("[ERROR] (Instruction %d) issued at cycle %d, after being flushed at cycle %d",
                         i, scoreboard[i].issued_timestamp, scoreboard[i].flushed_timestamp);
                flushed_not_issued_check = 0;
            end
        end
    endfunction

    // == NO SURVIVOR DEPENDS ON A SQUASHED PRODUCER ==
    // An instruction that outlives the flush must not have been renamed against
    // a producer that got squashed. If it was, the RRU failed to roll its
    // speculative RAT back to the branch's checkpoint.
    function int no_flushed_source_check();
        int s1, s2;
        no_flushed_source_check = 1;
        for (int i = 0; i < scoreboard.size(); i++) begin
            if (scoreboard[i].flushed_timestamp == -1 &&
                scoreboard[i].issued_timestamp  != -1) begin
                s1 = scoreboard[i].src1_ID;
                s2 = scoreboard[i].src2_ID;
                if (s1 != -1 && scoreboard[s1].flushed_timestamp != -1) begin
                    $display("[ERROR] (Instruction %d) issued using (Source1 producer %d), which was flushed at cycle %d",
                             i, s1, scoreboard[s1].flushed_timestamp);
                    no_flushed_source_check = 0;
                end
                if (s2 != -1 && scoreboard[s2].flushed_timestamp != -1) begin
                    $display("[ERROR] (Instruction %d) issued using (Source2 producer %d), which was flushed at cycle %d",
                             i, s2, scoreboard[s2].flushed_timestamp);
                    no_flushed_source_check = 0;
                end
            end
        end
    endfunction

    // How many instructions the trace actually squashed, so a run that reports
    // all flush checks passing on a trace with no mispredicts is not mistaken
    // for evidence that recovery works.
    function int flushed_count();
        flushed_count = 0;
        for (int i = 0; i < scoreboard.size(); i++) begin
            if (scoreboard[i].flushed_timestamp != -1) flushed_count++;
        end
    endfunction

    // == USABLE DUT INDICES ==
    // Check that every inst_ID / preg the DUT handed back was usable. Any
    // failure here means other checks saw incomplete data, because the
    // offending update was skipped rather than applied to the scoreboard.
    function int dut_index_check();
        dut_index_check = 1;
        if (dut_index_errors > 0) begin
            $display("[ERROR] DUT returned an unusable instruction ID or physical register %d times", dut_index_errors);
            if (dut_index_errors > MAX_INDEX_ERR_DISPLAY)
                $display("[INFO] Only the first %d were printed above", MAX_INDEX_ERR_DISPLAY);
            $display("[INFO] Other check results are unreliable while this fails");
            dut_index_check = 0;
        end
    endfunction

    // == PERFORMANCE ==
    // Cycle on which the last instruction committed. This, and not
    // cycle_counter, is how long the design actually took: cycle_counter also
    // includes the fixed DRAIN_CYCLES the testbench runs afterwards, which is
    // the same constant for every design and would only dilute the comparison.
    function int completion_cycle();
        completion_cycle = 0;
        for (int i = 0; i < scoreboard.size(); i++) begin
            if (scoreboard[i].committed_timestamp > completion_cycle)
                completion_cycle = scoreboard[i].committed_timestamp;
        end
    endfunction

    // Main check function (runs all tests)
    function scoreboard_check();
        bit all_passed;
        all_passed = 1'b1;

        // TESTBENCH SANITY: run first, it gates how much the rest can be trusted
        $display("[INFO] Printing testbench sanity checks...");
        if (dut_index_check()) $display("[PASS] DUT index usability check passed");
        else begin $display("[ERROR] DUT index usability check failed"); all_passed = 1'b0; end

        // ROB TESTS
        $display("[INFO] Printing passed ROB checks...");
        if (commit_in_order_check()) $display("[PASS] Commit in order check passed");
        else begin $display("[ERROR] Commit in order check failed"); all_passed = 1'b0; end
        if (commit_once_check()) $display("[PASS] Commit once check passed");
        else begin $display("[ERROR] Commit once check failed"); all_passed = 1'b0; end
        if (everything_committed_check()) $display("[PASS] Everything committed check passed");
        else begin $display("[ERROR] Everything committed check failed"); all_passed = 1'b0; end

        // IQ TESTS
        $display("[INFO] Printing passed IQ checks...");
        if (issue_before_commit_check()) $display("[PASS] Issue before commit check passed");
        else begin $display("[ERROR] Issue before commit check failed"); all_passed = 1'b0; end
        if (issue_after_dependencies_check()) $display("[PASS] Issue after dependencies check passed");
        else begin $display("[ERROR] Issue after dependencies check failed"); all_passed = 1'b0; end
        if (issue_once_check()) $display("[PASS] Issue once check passed");
        else begin $display("[ERROR] Issue once check failed"); all_passed = 1'b0; end
        if (everything_issued_check()) $display("[PASS] Everything issued check passed");
        else begin $display("[ERROR] Everything issued check failed"); all_passed = 1'b0; end

        // PRF TESTS
        $display("[INFO] Printing passed PRF checks...");
        if (num_archRegs_check()) $display("[PASS] Number of arhcitectural registers check passed");
        else begin $display("[ERROR] Number of arhcitectural registers check failed"); all_passed = 1'b0; end
        if (archRAT_check()) $display("[PASS] Architectural RAT check passed");
        else begin $display("[ERROR] Architectural RAT check failed"); all_passed = 1'b0; end

        // RRU TESTS
        $display("[INFO] Printing passed RRU checks...");
        if (correct_dependencies_check()) $display("[PASS] Correct dependency use check passed");
        else begin $display("[ERROR] Correct dependency use check failed"); all_passed = 1'b0; end
        if (correct_preg_reuse_check()) $display("[PASS] Correct physical register reuse test passed");
        else begin $display("[ERROR] Correct physical register reuse test failed"); all_passed = 1'b0; end

        // FLUSH TESTS
        $display("[INFO] Printing flush checks... (%d instructions were flushed by this trace)", flushed_count());
        if (flushed_pregs_reclaimed_check()) $display("[PASS] Flushed physical register reclaim check passed");
        else begin $display("[ERROR] Flushed physical register reclaim check failed"); all_passed = 1'b0; end
        if (flushed_not_committed_check()) $display("[PASS] Flushed instructions never committed check passed");
        else begin $display("[ERROR] Flushed instructions never committed check failed"); all_passed = 1'b0; end
        if (flushed_not_issued_check()) $display("[PASS] Flushed instructions never issued check passed");
        else begin $display("[ERROR] Flushed instructions never issued check failed"); all_passed = 1'b0; end
        if (no_flushed_source_check()) $display("[PASS] No flushed source dependency check passed");
        else begin $display("[ERROR] No flushed source dependency check failed"); all_passed = 1'b0; end

        // On any failure, dump the per-instruction state. Which timestamp is
        // the first -1 on a row tells you where in the pipeline it got stuck,
        // and a row with REMOVED_TS set but COMMITTED_TS at -1 means the ROB
        // retired the entry without ever asserting its committed_mask bit.
        if (!all_passed) begin
            $display("\n[INFO] Checks failed. Dumping final state for debug...\n");
            display_scoreboard();
            display_renameRegs();
            display_archRAT();
        end
    endfunction



    // HELPER FUNCTIONS/TASKS: Look at main() for main tb loop

    // == DUT-SUPPLIED INDEX GUARDS ==
    // inst_IDs and pregs coming back out of the DUT are used directly as
    // indices into the scoreboard and preg_avail. A broken design can hand back
    // an x or an out-of-range value, which would otherwise corrupt testbench
    // state or crash the simulator instead of reporting a useful error.

    // Validate an instruction ID before using it to index the scoreboard.
    // Takes a 4-state vector, NOT an int: assigning x to an int silently
    // collapses it to 0 and the check would never fire.
    function automatic bit valid_inst_ID(input logic [INST_BIT-1:0] id, input string source);
        valid_inst_ID = 1'b1;
        if (^id === 1'bx) begin
            if (dut_index_errors < MAX_INDEX_ERR_DISPLAY)
                $display("[ERROR] %s returned an unknown (x/z) instruction ID at cycle %d", source, cycle_counter);
            dut_index_errors = dut_index_errors + 1;
            valid_inst_ID = 1'b0;
        end
        else if (id >= scoreboard.size()) begin
            if (dut_index_errors < MAX_INDEX_ERR_DISPLAY)
                $display("[ERROR] %s returned out-of-range instruction ID %d (trace holds %d instructions) at cycle %d",
                         source, id, scoreboard.size(), cycle_counter);
            dut_index_errors = dut_index_errors + 1;
            valid_inst_ID = 1'b0;
        end
    endfunction

    // Validate a physical register before using it to index preg_avail
    function automatic bit valid_preg(input logic [PHYS_BIT-1:0] preg, input string source);
        valid_preg = 1'b1;
        if (^preg === 1'bx) begin
            if (dut_index_errors < MAX_INDEX_ERR_DISPLAY)
                $display("[ERROR] %s returned an unknown (x/z) physical register at cycle %d", source, cycle_counter);
            dut_index_errors = dut_index_errors + 1;
            valid_preg = 1'b0;
        end
    endfunction

    // Parse traces and prepare set up structs
    function parse_trace();
        int instruction_trace_file; // file descriptor
        int number_read;
        int counter = 0;
        scoreboard_entry_t scoreboard_entry;
        instruction_t dispatch_entry;

        scoreboard = new[1];

        instruction_trace_file = $fopen("traces/instructions.txt", "r");

        counter = 1;
        while (!$feof(instruction_trace_file)) begin

            // Set up a scoreboard entry for each line inside the trace
            number_read = $fscanf(instruction_trace_file, "%d | %d, %d -> %d | %d, %d\n",
                              scoreboard_entry.inst_ID, scoreboard_entry.src1_areg, scoreboard_entry.src2_areg,
                              scoreboard_entry.areg, scoreboard_entry.exec_duration, scoreboard_entry.inst_type);

            // $feof only asserts *after* a read runs off the end of the file, so the
            // final loop iteration would otherwise append a garbage scoreboard entry
            // built from a failed scan. Bail out unless all 6 fields were converted.
            if (number_read != 6) break;

            scoreboard_entry.dispatched_timestamp   = -1;
            scoreboard_entry.issued_timestamp       = -1;
            scoreboard_entry.executed_timestamp     = -1;
            scoreboard_entry.committed_timestamp    = -1;
            scoreboard_entry.removed_timestamp      = -1;
            scoreboard_entry.flushed_timestamp      = -1;

            scoreboard_entry.issue_count            = 0;
            scoreboard_entry.commit_count           = 0;
            scoreboard_entry.preg                   = 'x;
            scoreboard_entry.src1_ID                = -1;
            scoreboard_entry.src2_ID                = -1;

            // Populate dispatch queue entry accordingly
            dispatch_entry.inst_ID      = scoreboard_entry.inst_ID;
            dispatch_entry.src1         = scoreboard_entry.src1_areg;
            dispatch_entry.src2         = scoreboard_entry.src2_areg;
            dispatch_entry.dest         = scoreboard_entry.areg;
            dispatch_entry.is_branch    = (scoreboard_entry.inst_type == TYPE_BRANCH | 
                                           scoreboard_entry.inst_type == TYPE_FLUSH);

            // Register new instruction entry inside scoreboard and dispatch queue
            scoreboard = new[counter](scoreboard);
            scoreboard[scoreboard_entry.inst_ID] = scoreboard_entry;
            dispatch_entry_queue.push_back(dispatch_entry);
            counter++;
        end

        $fclose(instruction_trace_file);
    endfunction    

    // Drive all module inputs to 0
    task automatic drive_0();

        // Incoming instruction signals
        inserted_mask           = '0;
        inserted_instructions   = '0;

        // Executed instruction signals
        executed_mask       = '0;
        executed_rob_index  = '0;
        executed_preg       = '0;

        // Flush signals
        flush_en            = '0;
        flush_rob_index     = '0;
    endtask


    // Drive ROB and IQ with new instruction
    task automatic drive_dispatch_entry();
        instruction_t next_dispatch_entry;
        scoreboard_entry_t current_entry;
        // Instruction IDs driven this cycle, in dispatch-slot order
        int dispatched_IDs [$];
        // Need this intermediate variable to store queue size before pop
        int dispatch_entry_queue_size;
        dispatch_entry_queue_size = dispatch_entry_queue.size();

        // Perform a dispatch only if both the ROB and IQ have space
        if (iq_full || rob_full || rru_full) begin
            inserted_mask = '0;
        end

        // Assert the appropriate mask bits depending on size of queue
        else if (dispatch_entry_queue_size >= PPL_WIDTH) begin
            inserted_mask = '1;
        end
        else begin
            for (int ii = 0; ii < dispatch_entry_queue_size; ii++) begin
                inserted_mask[ii] = 1'b1;
            end
            for (int ii = dispatch_entry_queue_size; ii < PPL_WIDTH; ii++) begin
                inserted_mask[ii] = 1'b0;
            end
        end

        // PHASE 1: drive the whole batch onto the DUT's inputs.
        // Nothing is sampled here. The RRU renames the entire batch as one
        // combinational function of inserted_mask and inserted_instructions, so
        // none of its outputs mean anything until every slot has been driven.
        dispatched_IDs.delete();
        for (int ii = 0; ii < PPL_WIDTH; ii++) begin
            if (!iq_full && !rob_full && !rru_full && dispatch_entry_queue.size() >= 1) begin
                next_dispatch_entry = dispatch_entry_queue.pop_front();

                // Feed DUT with next
                inserted_instructions[ii].src1      = next_dispatch_entry.src1;
                inserted_instructions[ii].src2      = next_dispatch_entry.src2;
                inserted_instructions[ii].dest      = next_dispatch_entry.dest;
                inserted_instructions[ii].inst_ID   = next_dispatch_entry.inst_ID;
                inserted_instructions[ii].is_branch = next_dispatch_entry.is_branch;

                dispatched_IDs.push_back(next_dispatch_entry.inst_ID);
            end
        end

        // Let the DUT's combinational rename and ROB index allocation settle.
        // Blocking assignments above do not advance time, so without this the
        // testbench reads renamed_preg / inserted_index as they were *before*
        // this batch was driven, and records a stale preg on the scoreboard.
        #COMB_SETTLE;

        // PHASE 2: sample what the DUT produced for each slot we just drove
        for (int ii = 0; ii < dispatched_IDs.size(); ii++) begin

            // Enrich scoreboard entry with info returned from DUT on entry
            current_entry                      = scoreboard[dispatched_IDs[ii]];
            current_entry.dispatched_timestamp = cycle_counter;
            current_entry.preg                 = inserted_renamed_preg[ii];
            current_entry.rob_index            = inserted_rob_index[ii];

            // Confirm that allocated preg from DUT is currently available
            // according to preg_avail. Issue a bad_preg_alloc ticket otherwise
            if (valid_preg(inserted_renamed_preg[ii], "RRU renamed preg")) begin
                if (preg_avail[current_entry.preg] != 1'b1) begin
                    bad_preg_alloc = new[bad_preg_alloc.size() + 1] (bad_preg_alloc);
                    bad_preg_alloc[bad_preg_alloc.size() - 1] = cycle_counter;
                end

                // Mark allocated preg as unavailable
                preg_avail[current_entry.preg] = 1'b0;
            end

            // Identify instruction dependencies on entry and update map
            current_entry.src1_ID = producer_map[current_entry.src1_areg];
            current_entry.src2_ID = producer_map[current_entry.src2_areg];
            producer_map[current_entry.areg] = current_entry.inst_ID;

            // If instruction is a branch then save a checkpoint.
            //
            // Only mispredicting branches are snapshotted, which the testbench
            // can get away with because the trace states each branch's outcome
            // up front. Nothing on the design side has that foreknowledge at
            // rename time.
            if (current_entry.inst_type == TYPE_FLUSH) begin
                producer_map_checkpoints[current_entry.inst_ID] = producer_map;
            end

            // current_entry is a *copy* of the scoreboard entry, so every
            // field set above (timestamp, preg, rob_index, src IDs) has to be
            // written back or it is silently discarded.
            scoreboard[dispatched_IDs[ii]] = current_entry;
        end
    endtask

    // Send executed broadcasts to ROB and IQ
    // MUST be automatic: task locals are static by default in SystemVerilog, so
    // completed_entry_IDs would persist across calls and accumulate every ID it
    // has ever seen. Stale entries then occupy the low indices of the loop
    // below, pushing genuinely-completed instructions to an index >= PPL_WIDTH
    // where the executed_mask[i] write falls off the end of the vector and the
    // broadcast is silently dropped.
    task automatic drive_executed();

        int completed_count;
        int completed_entry_IDs [$];
        executed_entry_t executed_entry;
        int flush_range_oldest;
        int flush_range_youngest;
        int id;
        int found[$];

        // Decrement each instruction's cooldown to update their age
        foreach (executed_entry_queue[i]) begin
            if (executed_entry_queue[i].time_left > 0) executed_entry_queue[i].time_left -= 'd1;
        end

        // Pick up to PPL_WIDTH instructions that are ready
        completed_count = 0;
        foreach (executed_entry_queue[i]) begin
            if ((completed_count < PPL_WIDTH) && (executed_entry_queue[i].time_left == 'd0)) begin
                completed_count += 'd1;
                completed_entry_IDs.push_front(executed_entry_queue[i].inst_ID);
            end
        end

        // Drive ROB/IQ inputs. flush_en is a single-cycle pulse, so it has to be
        // cleared here rather than only in drive_0(): otherwise the first
        // mispredict leaves it asserted for the rest of the simulation and the
        // DUT squashes on every subsequent cycle.
        executed_mask       = '0;
        executed_rob_index  = '0;
        executed_preg       = '0;
        flush_en            = 1'b0;
        flush_rob_index     = '0;
        for (int i = 0; i < completed_entry_IDs.size(); i++) begin

            foreach (executed_entry_queue[j]) begin
                if (executed_entry_queue[j].inst_ID == completed_entry_IDs[i]) begin

                    executed_entry          = executed_entry_queue[j];
                    executed_mask[i]        = 1'b1;
                    executed_rob_index[i]   = executed_entry.rob_index;
                    executed_preg[i]        = executed_entry.executed_reg;

                    // Process a flush event
                    if (scoreboard[executed_entry.inst_ID].inst_type == TYPE_FLUSH) begin
                        
                        // Drive DUT appropriately
                        flush_en        = 1'b1;
                        flush_rob_index = executed_entry.rob_index;

                        // Revert internal producer map table
                        producer_map = producer_map_checkpoints[executed_entry.inst_ID];
                        producer_map_checkpoints.delete(executed_entry.inst_ID);

                        // Identify ID (exclusive) range of instructions to be
                        // flushed: everything dispatched after the branch, up to
                        // but not including whatever is still waiting to be
                        // dispatched. If the queue has drained, every remaining
                        // instruction in the trace was already dispatched and so
                        // the range runs to the end of the scoreboard.
                        flush_range_oldest      = executed_entry.inst_ID;
                        flush_range_youngest    = (dispatch_entry_queue.size() > 0)
                                                    ? dispatch_entry_queue[0].inst_ID
                                                    : scoreboard.size();

                        // Mark flushed instructions with timestamp on scoreboard
                        for (int k = flush_range_oldest+1; k < flush_range_youngest; k++) begin
                            scoreboard[k].flushed_timestamp = cycle_counter;
                        end
                        
                        // Remove any flushed instructions from the executed queue
                        // (except for the ones currently being executed)
                        for (int k = executed_entry_queue.size()-1; k >= 0; k--) begin
                            id = executed_entry_queue[k].inst_ID;
                            found  = completed_entry_IDs.find() with (item == id);
                            if (scoreboard[id].flushed_timestamp != -1 && found.size() == 0) begin
                                executed_entry_queue.delete(k);
                            end
                        end
                    end

                    // Register executed entry
                    scoreboard[executed_entry.inst_ID].executed_timestamp = cycle_counter;

                    // Re-locate this entry by ID before deleting it. The flush
                    // handling above removes squashed instructions from
                    // executed_entry_queue, so j -- found before that ran -- may
                    // now point past the end of a shorter queue, or at somebody
                    // else's entry. This entry itself is never one of the ones
                    // removed above: the found.size() == 0 guard exempts
                    // anything in completed_entry_IDs, which includes this one.
                    for (int k = executed_entry_queue.size()-1; k >= 0; k--) begin
                        if (executed_entry_queue[k].inst_ID == executed_entry.inst_ID) begin
                            executed_entry_queue.delete(k);
                            break;
                        end
                    end
                    break;
                end
            end
        end
    endtask


    // Sink issued instructions and appemnd them to executed_entry_queue
    task automatic sink_issued();
        executed_entry_t executed_entry;

        for (int ii = 0; ii < PPL_WIDTH; ii++) begin
            if (issued_mask[ii] && valid_inst_ID(issued_iq_entries[ii].inst_ID, "IQ issued entry")) begin

                // Update scoreboard entry. issue_count backs issue_once_check:
                // a correct IQ issues each entry exactly once, so anything
                // above 1 here means the entry was left issue-ready after
                // firing, or was allocated to two slots at once.
                scoreboard[issued_iq_entries[ii].inst_ID].issued_timestamp  = cycle_counter;
                scoreboard[issued_iq_entries[ii].inst_ID].src1_preg         = issued_iq_entries[ii].src1;
                scoreboard[issued_iq_entries[ii].inst_ID].src2_preg         = issued_iq_entries[ii].src2;
                scoreboard[issued_iq_entries[ii].inst_ID].issue_count       += 1;

                // Set up entry for execution queue
                executed_entry.inst_ID      = issued_iq_entries[ii].inst_ID;
                executed_entry.rob_index    = issued_iq_entries[ii].rob_index;
                executed_entry.executed_reg = scoreboard[issued_iq_entries[ii].inst_ID].preg;
                executed_entry.special      = 'd0;
                executed_entry.time_left    = scoreboard[issued_iq_entries[ii].inst_ID].exec_duration;
                executed_entry_queue.push_back(executed_entry);
            end
        end
    endtask


    // Sink committed instructions
    task automatic sink_committed();

        int overwritten_areg;
        int free_preg;

        // Working copy of the archRAT for this batch. archRAT is the DUT's
        // registered output, so it does not yet reflect any commit happening
        // this cycle. If two instructions in one batch write the same areg,
        // reading archRAT directly for both would free the same preg twice and
        // leak the one the older instruction had made architectural.
        logic [ARCH_REG-1:0][PHYS_BIT-1:0] arch_working;
        arch_working = archRAT;

        for (int ii = 0; ii < PPL_WIDTH; ii++) begin
            // Both branches index the scoreboard with an ID from the ROB, so
            // validate once up front and skip the entry entirely if it is bad.
            if ((committed_mask[ii] || removed_mask[ii])
                && !valid_inst_ID(removed_inst_id[ii], "ROB removed entry")) continue;

            if (committed_mask[ii]) begin
                // commit_count backs commit_once_check: the ROB must retire
                // each entry exactly once, so a count above 1 means the slot
                // was never invalidated after being committed.
                scoreboard[removed_inst_id[ii]].committed_timestamp = cycle_counter;
                scoreboard[removed_inst_id[ii]].commit_count        += 1;

                // Identify preg to be overwritten in the archRAT and mark
                // that preg as free in preg_avail. The freed register is the
                // one this areg pointed at *before* this instruction commits,
                // so publish the new mapping for the rest of the batch.
                overwritten_areg        = scoreboard[removed_inst_id[ii]].areg;
                free_preg               = arch_working[overwritten_areg];
                if (valid_preg(free_preg, "archRAT entry")) preg_avail[free_preg] = 1'b1;
                arch_working[overwritten_areg] = scoreboard[removed_inst_id[ii]].preg;
            end
            else if (removed_mask[ii]) begin
                scoreboard[removed_inst_id[ii]].removed_timestamp = cycle_counter;

                // Removed without committing means flushed. The instruction
                // never became architectural, so what goes back is the register
                // it was allocated at rename, not a displaced one. Without this
                // the model leaks a preg per squashed instruction and drifts
                // away from the RRU, which frees on removed_mask.
                free_preg = scoreboard[removed_inst_id[ii]].preg;
                if (valid_preg(free_preg, "flushed entry preg")) preg_avail[free_preg] = 1'b1;
            end
        end
    endtask


    // Run a single cycle of renaming
    task automatic process_cycle();
        cycle_counter = cycle_counter + 1;

        // Step off the clock edge before touching the DUT. process_cycle() is
        // called at the same simulation time as a posedge, so driving inputs
        // immediately would race the DUT's own always_ff blocks sampling them.
        #DRIVE_DELAY;

        // A stall holds back *dispatch only*. Execution, issue and commit have
        // to keep running: the usual reason to stall is that some speculative
        // resource has run out, and the only thing that frees one is an older
        // instruction retiring. Gating the whole cycle on ~stall would mean the
        // condition that caused the stall could never clear.
        if (~stall) drive_dispatch_entry();
        else        inserted_mask = '0;

        drive_executed();
        sink_issued();
        sink_committed();

        /* Timeout if stuck on full */
        if (iq_full === 1'b0 && rob_full === 1'b0 && rru_full == 1'b0) begin
            full_counter = 0;
        end
        if (stall === 1'b0) stall_counter = 0;

        if (iq_full || rob_full || rru_full || 
            iq_full === 1'bx || rob_full === 1'bx || rru_full === 1'bx ||
            iq_full === 1'bz || rob_full === 1'bz || rru_full === 1'bx) begin
            full_counter = full_counter + 1;
        end

        if (stall || stall === 1'bx || stall === 1'bz) stall_counter = stall_counter + 1;

        if (full_counter >= FULL_THRESHOLD) begin
            $display("[ERROR] Simulation ended early...detected full for longer than %d cycles", FULL_THRESHOLD);
            display_scoreboard();
            $display("[ERROR] Simulation ended early...detected full for longer than %d cycles", FULL_THRESHOLD);
            $finish;
        end

        if (stall_counter >= STALL_THRESHOLD) begin
            $display("[ERROR] Simulation ended early...detected stall for longer than %d cycles", STALL_THRESHOLD);
            display_scoreboard();
            $display("[ERROR] Simulation ended early...detected stall for longer than %d cycles", STALL_THRESHOLD);
            $finish;
        end
    endtask


    // Generate clock
    always begin
        #5 clk = ~clk;
    end


    // Main testbench routine
    initial begin
        $dumpfile("tb_top.vcd");
        $dumpvars(0, TB_TOP);

        // INITIAL RESET
        clk = 0;
        drive_0();

        reset <= 1;
        @(posedge clk);
        @(posedge clk);
        @(posedge clk);
        @(posedge clk);
        reset <= 0;
        @(posedge clk);

        // SET UP
        // Parse trace, initialize counters
        $display("\n\n[INFO] Parsing trace file\n");
        parse_trace();
        $display("[INFO] Scoreboard, prepped...starting to dispatch instructions\n");
        cycle_counter           = 0;
        full_counter            = 0;
        stall_counter           = 0;
        dut_index_errors        = 0;

        // Internal map table
        producer_map        = '{default:-1};
        for (int i = 0; i < ARCH_REG; i++) preg_avail[i] = 1'b0;
        for (int i = ARCH_REG; i < PHYS_REG; i++) preg_avail[i] = 1'b1;

        // MAIN TESTBENCH LOOP
        // Keep processing until no more dispatch instructions
        while (dispatch_entry_queue.size() != 0) begin
            process_cycle();
            @(posedge clk);
        end
        $display("[INFO] Finished dispatching all instructions\n");

        // DRAIN CYCLES
        for (int i = 0; i < DRAIN_CYCLES; i++) begin
            process_cycle();
            @(posedge clk);
        end
        $display("[INFO] Finished waiting for pipes to drain\n");

        // Printed with %0d so it parses cleanly. Only meaningful if the checks
        // below pass -- a design that drops instructions finishes early.
        $display("[PERF] completion_cycle: %0d", completion_cycle());

        // SCOREBOARD CHECKS
        scoreboard_check();
        $finish;
    end

endmodule: TB_TOP
