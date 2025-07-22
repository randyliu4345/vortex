`include "VX_define.vh"

typedef struct packed {
    logic[31:0]          num_warps;
    logic[`XLEN-1:0]     start_pc;
    logic[`XLEN-1:0]     param;
    logic[31:0]          cta_x;
    logic[31:0]          cta_y;
    logic[31:0]          cta_z;
    logic[31:0]          cta_id;
} kmu_task_t;

interface VX_kmu_task_if();
    /////////////// kmu to sm /////////////////////
    wire kmu_valid;
    wire core_ready;
    kmu_task_t task_data;

    `UNUSED_VAR(kmu_valid);
    `UNUSED_VAR(core_ready);
    `UNUSED_VAR(task_data);

    assign kmu_valid = '0;
    assign core_ready = 1;
    assign task_data = '0;

    ////////////// sm to kmu ///////////////////////

    wire core_valid;
    wire kmu_ready;
    wire core_done;

    assign core_valid = '0;
    assign kmu_ready = '0;
    assign core_done = '0;

    `UNUSED_VAR(core_valid);
    `UNUSED_VAR(kmu_ready);
    `UNUSED_VAR(core_done);

    modport master (
        output kmu_valid,
        input core_ready,
        output task_data,
        input core_valid,
        output kmu_ready,
        input core_done
    );

    modport slave (
        input kmu_valid,
        output core_ready,
        input task_data,
        output core_valid,
        input kmu_ready,
        output core_done
    );

endinterface
