`include "VX_define.vh"

module VX_kmu_top import VX_gpu_pkg::*;(
    `SCOPE_IO_DECL

    input wire reset,
    input wire clk,

    // DCR write request
    input  wire                            dcr_wr_valid,
    input  wire [VX_DCR_ADDR_WIDTH-1:0]    dcr_wr_addr,
    input  wire [VX_DCR_DATA_WIDTH-1:0]    dcr_wr_data,

    // output to test dummy command processor
    output wire[`XLEN-1:0]                  pc,
    output wire[31:0]                       grid_dim[2:0],
    output wire[31:0]                       block_dim[2:0],
    output wire [`XLEN-1:0]                 param,

    // kmu outputs
    // input wire [`NUM_CLUSTERS-1:0][`NUM_CORES-1:0] [`NUM_WARPS-1:0] per_warp_busy,
    output wire [`NUM_CLUSTERS-1:0][`NUM_CORES-1:0][`NUM_WARPS-1:0][`XLEN-1:0] per_warp_pc,
    output wire [`NUM_CLUSTERS-1:0][`NUM_CORES-1:0][`NUM_WARPS-1:0] per_warp_valid,
    output wire [`NUM_CLUSTERS-1:0][`NUM_CORES-1:0][`NUM_WARPS-1:0][31:0] per_warp_task,

    // hardwire signal for dummy command processor data valid
    // input wire cp_valid,

    output wire busy
);

    wire start;
    `UNUSED_VAR(start);

    assign busy = '1;

    VX_kmu_task_if task_interface[`NUM_CLUSTERS * `NUM_CORES]();

    VX_kmu kmu (
        .clk (clk),
        .reset (reset),
        .dcr_wr_valid   (dcr_wr_valid),
        .dcr_wr_addr    (dcr_wr_addr),
        .dcr_wr_data    (dcr_wr_data),
        .task_interface (task_interface),
        .start          (start)
    );

    assign pc = '0;
    assign param = '0;
    assign grid_dim[0] = '0;
    assign grid_dim[1] = '0;
    assign grid_dim[2] = '0;
    assign block_dim[0] = '0;
    assign block_dim[1] = '0;
    assign block_dim[2] = '0;

    // assign per_warp_busy = '0;
    assign per_warp_valid = '0;
    assign per_warp_task = '0;
    assign per_warp_pc = '0;

    `UNUSED_VAR(per_warp_valid);
    `UNUSED_VAR(per_warp_task);
    `UNUSED_VAR(per_warp_pc);



    // always @ (posedge clk) begin
    //     if(reset) begin
    //         `TRACE(0, ("reset is on\n"));
    //     end
    // end

endmodule
