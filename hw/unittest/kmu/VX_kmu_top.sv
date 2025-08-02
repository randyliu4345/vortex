`include "VX_define.vh"

module VX_kmu_top import VX_gpu_pkg::*;
(
    input wire              clk,
    input wire              reset,

    // DCR write request
    input  wire                             dcr_wr_valid,
    input  wire [VX_DCR_ADDR_WIDTH-1:0]     dcr_wr_addr,
    input  wire [VX_DCR_DATA_WIDTH-1:0]     dcr_wr_data,

    output wire start
);

    VX_kmu_bus_if kmu_bus_out[1]();

    assign kmu_bus_out[0].req_ready = '1;

    `UNUSED_VAR(kmu_bus_out[0].req_data);
    `UNUSED_VAR(kmu_bus_out[0].req_valid);

    VX_kmu kmu(
        .clk        (clk),
        .reset      (reset),
        .dcr_wr_valid   (dcr_wr_valid),
        .dcr_wr_addr    (dcr_wr_addr),
        .dcr_wr_data    (dcr_wr_data),
        .start          (start),
        .kmu_bus_out    (kmu_bus_out)
    );

endmodule;
