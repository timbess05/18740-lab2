`default_nettype none


/*******************************************************
* Design Parameters - Modify as per the Handout
*******************************************************/
parameter ROB_SIZE = 128;
parameter IQ_SIZE  = 64;


/*******************************************************
* Design Parameters - Do NOT Modify
*******************************************************/
parameter PPL_WIDTH = 4;
parameter ARCH_REG  = 32;
parameter PHYS_REG  = 2 * ROB_SIZE;
parameter INST_NUM  = 4096;

parameter ROB_BIT   = $clog2(ROB_SIZE);
parameter IQ_BIT    = $clog2(IQ_SIZE);
parameter ARCH_BIT  = $clog2(ARCH_REG);
parameter PHYS_BIT  = $clog2(PHYS_REG);
parameter INST_BIT  = $clog2(INST_NUM);

/*******************************************************
* ROB / IQ / RRU Entry Structs
*******************************************************/
typedef struct packed 
{
    logic [ROB_BIT-1:0]  rob_index;
    logic [INST_BIT-1:0] inst_ID;
    logic [PHYS_BIT-1:0] src1;
    logic [PHYS_BIT-1:0] src2;
    logic                src1_ready;
    logic                src2_ready;
    logic                valid;
} iq_entry_t;

typedef struct packed
{
    logic [ARCH_BIT-1:0] areg;
    logic [PHYS_BIT-1:0] preg;
    logic [INST_BIT-1:0] inst_ID;
    logic                is_completed;
    logic                is_branch;
    logic                valid;
} rob_entry_t;

typedef struct packed
{
    logic [INST_BIT-1:0] inst_ID;
    logic [ARCH_BIT-1:0] src1;
    logic [ARCH_BIT-1:0] src2;
    logic [ARCH_BIT-1:0] dest;
    logic                is_branch;
} instruction_t;

/*******************************************************
* Testbench Parameters - Do NOT Modify
*******************************************************/
parameter DRAIN_CYCLES      = 1000;
parameter FULL_THRESHOLD    = 1000;
parameter STALL_THRESHOLD   = 6767;
parameter MAX_EXEC_DURATION = 2 * ROB_SIZE;
parameter TYPE_REGULAR      = 0;
parameter TYPE_BRANCH       = 1;
parameter TYPE_FLUSH        = 2;

/*******************************************************
* Testbench Buffer Entry Definitions - Do NOT Modify
*******************************************************/ 
typedef struct packed
{
    logic [INST_BIT-1:0]                  inst_ID;
    logic [PHYS_BIT-1:0]                  executed_reg;
    logic [ROB_BIT-1:0]                   rob_index;
    logic [$clog2(MAX_EXEC_DURATION)-1:0] time_left;
    logic                                 special;
} executed_entry_t;

typedef struct packed
{
    logic [INST_BIT-1:0] inst_ID;
    logic [ARCH_BIT-1:0] areg;
    logic [PHYS_BIT-1:0] preg;
    logic [ARCH_BIT-1:0] src1_areg;
    logic [ARCH_BIT-1:0] src2_areg;
    logic [PHYS_BIT-1:0] src1_preg;
    logic [PHYS_BIT-1:0] src2_preg;
    logic [ROB_BIT-1:0] rob_index;

    int src1_ID;
    int src2_ID;
    int exec_duration;
    int inst_type;

    int dispatched_timestamp;
    int issued_timestamp;
    int executed_timestamp;
    int committed_timestamp;
    int removed_timestamp;
    int flushed_timestamp;

    int issue_count;
    int commit_count;
} scoreboard_entry_t;

typedef int areg_map_t [32];
