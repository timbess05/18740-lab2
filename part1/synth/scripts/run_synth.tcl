# LOAD DESIGN INFORMATION
# =======================
source $DESIGN_SETUP
source /afs/ece.cmu.edu/class/ece740/tools/genus/synth_scripts.tcl

# GENERAL SETUP
# =============
set_db fail_on_error_mesg true

set SDC scripts/design.sdc
set SYN_OPT_EFFORT medium

# SETUP OUTPUT DIRECTORIES
# =========================
if { $FOR_SWEEP } {
    set REPS_DIR Reps/CLK_PERIOD_$CLK_PERIOD
    set RESULTS_DIR Results/CLK_PERIOD_$CLK_PERIOD
} else {
    set REPS_DIR Reps
    set RESULTS_DIR Results
}

if { ! [file isdir $REPS_DIR] } { exec mkdir -p $REPS_DIR }
if { ! [file isdir $RESULTS_DIR] } { exec mkdir -p $RESULTS_DIR }

# SYNTHESIS STEPS
# ===============
setupTech
readDesign
set_db auto_ungroup none
synthDesign
writeReports
