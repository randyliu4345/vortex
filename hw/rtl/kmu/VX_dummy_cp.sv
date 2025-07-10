`include "VX_define.vh"

module VX_dummy_cp import VX_gpu_pkg::*; (
    // Clock
    input  wire                             clk,
    input  wire                             reset,
    output wire                             busy,

    // DCR bus interface
    VX_dcr_bus_if.slave                     dcr_bus_if,

    // connect to KMU
    output VX_cmd_processor_if.master       cmd_processor_output, 
    output kmu_data_t                       to_kmu
);

assign busy = 1'b0;

always @ (posedge clk) begin
    if (reset) begin
        // cmd_processor_output.pc <= '0;
        // cmd_processor_output.param <= '0;
        // cmd_processor_output.grid_dim  <= {'0, '0, '0};
        // cmd_processor_output.block_dim <= {'0, '0, '0};
    end 
    if (dcr_bus_if.write_valid) begin
        case(dcr_bus_if.write_addr)
            // PC
            `VX_DCR_BASE_STARTUP_ADDR0: begin
                to_kmu.pc <= dcr_bus_if.write_data;
            end

            // PARAM
            `VX_DCR_BASE_STARTUP_ARG0: begin
                to_kmu.param <= dcr_bus_if.write_data;
                cmd_processor_output.valid <= '1;
            end

            // Grid_dim
            `VX_DCR_BASE_GRID_DIM0: begin
                to_kmu.grid_dim[0] <= dcr_bus_if.write_data;
            end
            `VX_DCR_BASE_GRID_DIM1: begin
                to_kmu.grid_dim[1] <= dcr_bus_if.write_data;
            end
            `VX_DCR_BASE_GRID_DIM2: begin
                to_kmu.grid_dim[2] <= dcr_bus_if.write_data;
            end

            // Block_dim
            `VX_DCR_BASE_BLOCK_DIM0: begin
                to_kmu.block_dim[0] <= dcr_bus_if.write_data;
            end
            `VX_DCR_BASE_BLOCK_DIM1: begin
                to_kmu.block_dim[1] <= dcr_bus_if.write_data;
            end
            `VX_DCR_BASE_BLOCK_DIM2: begin
                to_kmu.block_dim[2] <= dcr_bus_if.write_data;
            end

            default: begin
                // `ASSERT(0, ("%t: invalid DCR write address: %0h", $time, dcr_bus_if.write_addr));
            end
        endcase
    end
    if(cmd_processor_output.valid) begin
        cmd_processor_output.valid <= '0;
    end
end

endmodule
