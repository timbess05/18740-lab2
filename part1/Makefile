# Out-of-order rename and issue.
#
#   make            build ./simv from verilog_src/rename_modules/
#   make debug      the same, but with full visibility for the GUI
#   make clean
#
# To run the regressions, build first and then use the scripts:
#
#   make && ./run_traces
#   make && ./run_branch_traces
#
# To debug in the GUI, build with 'make debug' and then either open the
# simulation interactively or replay the waveform it dumped:
#
#   ./simv -gui                  step through, set breakpoints
#   dve -openvcd tb_top.vcd &    look at signals from a finished run

.PHONY: all dev debug clean

VCS = vcs

# Full debug visibility. Without this the GUI can still open, but most signals
# are optimised away and will not be browsable.
DEBUG_FLAGS = -debug_access+all

COMMON = verilog_src/global_defines.svh \
	     verilog_src/rename_modules/rename_top.sv \
	     verilog_src/tb_top.sv

SRC = verilog_src/rename_modules/iq.sv \
	  verilog_src/rename_modules/prf.sv \
	  verilog_src/rename_modules/rob.sv \
	  verilog_src/rename_modules/rru.sv

# Found by wildcard rather than listed by name, so the target is simply
# unavailable when the directory is not present.
DEV = $(wildcard verilog_src/rename_test_modules/*.sv)

all:
	$(VCS) -sverilog $(COMMON) $(SRC)

dev:
ifeq ($(strip $(DEV)),)
	@echo "[INFO] The 'dev' target is not available in this checkout."
	@echo "[INFO] Use 'make' to build the modules in verilog_src/rename_modules/."
	@false
else
	$(VCS) -sverilog $(COMMON) $(DEV)
endif

# Slower to compile and to run than 'make', so it is a separate target rather
# than the default.
debug:
	$(VCS) -sverilog $(DEBUG_FLAGS) $(COMMON) $(SRC)

clean:
	rm -rf csrc simv simv.daidir ucli.key DVEfiles tb_top.vcd \
	       inter.vpd vcdplus.vpd *.daidir
