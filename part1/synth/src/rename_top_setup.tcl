# 45nm GPDK GENUS SYNTHESIS FLOW - CONFIGURATION FILE
#----------------------------------------------------

# Top module name - must match the top-level in you SV exactly
set TOP RENAME_TOP
# Directory where HDL source is found
set SOURCE_PATH "./src"
# List of HDL source files to include in synthesis
set SOURCES {global_defines.svh iq.sv rob.sv prf.sv rru.sv rename_top.sv}
