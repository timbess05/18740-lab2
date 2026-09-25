##### Timing constraint -----------------------------------------------------

# :::::::: SEQUENTIAL CIRCUITS
set CLK_PORT_NAME "clk"
set CLK_UNCERTAINTY 0.05

set CLK_TRAN 0.05
set IN_DELAY 0.1
set OUT_DELAY 0.1

set MAX_TRAN 0.14

#Define the clock
create_clock -name $CLK_PORT_NAME -period $CLK_PERIOD [get_ports $CLK_PORT_NAME]
set_clock_uncertainty -setup $CLK_UNCERTAINTY [get_clocks $CLK_PORT_NAME]
set_clock_uncertainty -hold $CLK_UNCERTAINTY [get_clocks $CLK_PORT_NAME]

# Set input delay for the input ports ( Delay from register generating the ip signal to the ip port )
set_input_delay $IN_DELAY -clock $CLK_PORT_NAME [get_db [get_db ports -if {.direction=="in" && .name!="$CLK_PORT_NAME"}] .name]
set_output_delay $OUT_DELAY -clock $CLK_PORT_NAME [get_db [get_db ports -if {.direction=="out"}] .name]

# :::::::: COMBINATIONAL CIRCUITS
#set_max_delay $DELAY -from [all_inputs] -to [all_outputs]

##### Design constraints (output load, input drive, max tran) -------------------------------------------------------------

# Set load at every output pin (eg. 5fF). Express the number in library units.
# report_lib command will provide the units used in the library.
set_load 0.025 [all_outputs]

# Set the driver cells for the inputs (or) set_input_transition depending on requirment
set_driving_cell -lib_cell BUFX2 [get_db [get_db ports -if {.direction=="in" && .name!="$CLK_PORT_NAME"}] .name]

# Define transition times
set_clock_transition $CLK_TRAN [get_clocks $CLK_PORT_NAME]
set_max_transition $MAX_TRAN $TOP

# Set maximum fanout for the design to avoid very hivh fanout nets
set_max_fanout 32 [get_designs $TOP]
