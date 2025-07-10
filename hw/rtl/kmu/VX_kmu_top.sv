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

    assign busy = '1;

    VX_dcr_bus_if dcr_bus_if();
    assign dcr_bus_if.write_valid = dcr_wr_valid;
    assign dcr_bus_if.write_addr  = dcr_wr_addr;
    assign dcr_bus_if.write_data  = dcr_wr_data;

    wire dummy_cp_busy;
    `UNUSED_VAR (dummy_cp_busy);
    VX_cmd_processor_if dummy_cp_if();

    // assign dummy_cp_if.valid = cp_valid;

    kmu_data_t cp_to_kmu;

    VX_dummy_cp dummy_cp (
            .clk (clk),
            .reset (reset),
            .busy (dummy_cp_busy),
            .dcr_bus_if (dcr_bus_if),
            .cmd_processor_output (dummy_cp_if.master),
            .to_kmu (cp_to_kmu)
    );

    // wire [`NUM_CLUSTERS-1:0][NUM_SOCKETS * `SOCKET_SIZE * `NUM_WARPS-1:0] per_cluster_cta_busy;
    // `UNUSED_VAR(per_cluster_cta_busy);

    // wire [`NUM_CLUSTERS-1:0][NUM_SOCKETS-1:0][`SOCKET_SIZE-1:0][`NUM_WARPS-1:0][`XLEN-1:0] per_cta_pc;
    // wire [`NUM_CLUSTERS-1:0][NUM_SOCKETS-1:0][`SOCKET_SIZE-1:0][`NUM_WARPS-1:0] per_cta_valid;
    // wire [`NUM_CLUSTERS-1:0][NUM_SOCKETS-1:0][`SOCKET_SIZE-1:0][`NUM_WARPS-1:0][31:0] per_cta_task;

    VX_kmu_task_if task_interface[`NUM_CLUSTERS * `NUM_CORES]();

    VX_kmu kmu (
        .clk (clk),
        .reset (reset),
        .cmd_processor_input (dummy_cp_if.slave),
        .from_cp (cp_to_kmu),
        .task_interface (task_interface)
    );

    assign pc = cp_to_kmu.pc;
    assign param = cp_to_kmu.param;
    assign grid_dim[0] = cp_to_kmu.grid_dim[0];
    assign grid_dim[1] = cp_to_kmu.grid_dim[1];
    assign grid_dim[2] = cp_to_kmu.grid_dim[2];
    assign block_dim[0] = cp_to_kmu.block_dim[0];
    assign block_dim[1] = cp_to_kmu.block_dim[1];
    assign block_dim[2] = cp_to_kmu.block_dim[2];

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
