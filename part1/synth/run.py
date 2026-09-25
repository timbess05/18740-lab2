#!/usr/bin/python3

import argparse
import subprocess

parser = argparse.ArgumentParser(description="Run Genus synthesis")

parser.add_argument(
    "--batch", action="store_true", help="exit immediately on synth completion"
)
parser.add_argument("--clock-period", action="store", help="target clock period")
parser.add_argument(
    "--for-sweep",
    action="store_true",
    help="create unique output dirs for each set of input parameters"
    " (useful when programatically sweeping parameters)",
)
parser.add_argument(
    "design_setup", action="store", help="TCL script with design-specific variables"
)

args = parser.parse_args()

clock_period = 20.0  # 50 MHz
if args.clock_period != None:
    clock_period = float(args.clock_period)

for_sweep = int(args.for_sweep)
if for_sweep:
    logs_dir = "Logs/CLK_PERIOD_{}".format(clock_period)
else:
    logs_dir = "Logs"

subprocess.run(["mkdir", "-p", logs_dir], check=True)

log_file = "{}/run.log".format(logs_dir)

genus = [
    "genus",
    "-log",
    "{}/run.log".format(logs_dir),
    "-overwrite",
    "-execute",
    "set DESIGN_SETUP {};set FOR_SWEEP {};set CLK_PERIOD {}".format(
        args.design_setup, for_sweep, clock_period
    ),
    "-files",
    "scripts/run_synth.tcl",
]
if args.batch:
    genus.extend(["-no_gui", "-batch"])

subprocess.run(genus)
