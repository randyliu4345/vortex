`include "VX_define.vh"

module VX_cta_dispatch_top import VX_gpu_pkg::*;
(
    input wire                      clk,
    input wire                      reset,

    // CTA task input
    input  wire                     task_in_req_valid,
    output wire                     task_in_req_ready,
    // input  wire [VX_KMU_REQ_DATA_WIDTH-1:0] task_in_req_data, // flatten struct for C++ driving

    input wire[31:0]                num_warps,
    input wire[`XLEN-1:0]           start_pc,
    input wire[`XLEN-1:0]           input_param,
    input wire [31:0] input_cta_x,
    input wire [31:0] input_cta_y,
    input wire [31:0] input_cta_z,
    input wire [31:0] input_cta_id,
    input wire [`NUM_THREADS-1:0] input_remain_mask,

    input  wire [`NUM_WARPS-1:0]    active_warps,

    output wire [`NUM_WARPS-1:0]    sched_warps,
    output wire [`NUM_WARPS-1:0][31:0] cta_x,
    output wire [`NUM_WARPS-1:0][31:0] cta_y,
    output wire [`NUM_WARPS-1:0][31:0] cta_z,
    output wire [`NUM_WARPS-1:0][31:0] cta_id,
    output wire [`NUM_WARPS-1:0][`XLEN-1:0] pc,
    output wire [`NUM_WARPS-1:0][`XLEN-1:0] param,
    output wire [`NUM_WARPS-1:0][`NUM_THREADS-1:0] remain_masks
);

    // Unpack the flat task_in_req_data to struct
    VX_kmu_bus_if kmu_bus();

    assign kmu_bus.req_valid = task_in_req_valid;
    assign task_in_req_ready = kmu_bus.req_ready;

    assign kmu_bus.req_data.num_warps = num_warps;
    assign kmu_bus.req_data.start_pc = start_pc;
    assign kmu_bus.req_data.param = input_param;
    assign kmu_bus.req_data.cta_x = input_cta_x;
    assign kmu_bus.req_data.cta_y = input_cta_y;
    assign kmu_bus.req_data.cta_z = input_cta_z;
    assign kmu_bus.req_data.cta_id = input_cta_id;
    assign kmu_bus.req_data.remain_mask = input_remain_mask;

    VX_cta_dispatch cta_dispatch (
        .clk        (clk),
        .reset      (reset),
        .task_in    (kmu_bus),
        .active_warps (active_warps),
        .sched_warps  (sched_warps),
        .cta_x        (cta_x),
        .cta_y        (cta_y),
        .cta_z        (cta_z),
        .cta_id       (cta_id),
        .pc           (pc),
        .param        (param),
        .remain_masks (remain_masks)
    );

endmodule
