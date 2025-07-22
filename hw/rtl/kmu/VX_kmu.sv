`include "VX_define.vh"

module VX_kmu import VX_gpu_pkg::*; (
    input wire clk,
    input wire reset,

    // DCR write request
    input  wire                             dcr_wr_valid,
    input  wire [VX_DCR_ADDR_WIDTH-1:0]     dcr_wr_addr,
    input  wire [VX_DCR_DATA_WIDTH-1:0]     dcr_wr_data,

    output reg start,

    VX_raster_bus_if.master                 raster_bus_out[1]
);

    // Configuration data
    kmu_data_t kmu_data;
    logic[31:0] smem_size = 0;
    `UNUSED_VAR(smem_size);

    // Internal counters for CTA distribution
    logic[31:0] counter_x, counter_y, counter_z, counter_id;

    // Thread and warp calculation
    logic[31:0] total_threads;
    logic[31:0] total_warps;

    // State for raster bus handshake
    logic all_cta_sent;

    // Calculate total threads and warps
    always_comb begin
        total_threads = `MAX(kmu_data.block_dim[0], 1)
                      * `MAX(kmu_data.block_dim[1], 1)
                      * `MAX(kmu_data.block_dim[2], 1);

        total_warps = total_threads / `NUM_THREADS;
        if (total_warps * `NUM_THREADS < total_threads)
            total_warps++;
    end

    // DCR write logic
    always_ff @(posedge clk) begin
        if (reset) begin
            kmu_data.pc        <= '0;
            kmu_data.param     <= '0;
            kmu_data.grid_dim  <= '{default:0};
            kmu_data.block_dim <= '{default:0};
            smem_size          <= '0;
        end else if (dcr_wr_valid) begin
            case(dcr_wr_addr)
                // PC
                `VX_DCR_BASE_STARTUP_ADDR0: kmu_data.pc <= dcr_wr_data;
                // PARAM
                `VX_DCR_BASE_STARTUP_ARG0:  kmu_data.param <= dcr_wr_data;
                // Grid_dim
                `VX_DCR_BASE_GRID_DIM0:     kmu_data.grid_dim[0] <= dcr_wr_data;
                `VX_DCR_BASE_GRID_DIM1:     kmu_data.grid_dim[1] <= dcr_wr_data;
                `VX_DCR_BASE_GRID_DIM2:     kmu_data.grid_dim[2] <= dcr_wr_data;
                // Block_dim
                `VX_DCR_BASE_BLOCK_DIM0:    kmu_data.block_dim[0] <= dcr_wr_data;
                `VX_DCR_BASE_BLOCK_DIM1:    kmu_data.block_dim[1] <= dcr_wr_data;
                `VX_DCR_BASE_BLOCK_DIM2:    kmu_data.block_dim[2] <= dcr_wr_data;
                // Shared memory size
                `VX_DCR_BASE_SMEM_SIZE:     smem_size <= dcr_wr_data;
                default: ; // ignore
            endcase
        end
    end

    // CTA distribution state machine
    always_ff @(posedge clk) begin
        if (reset) begin
            counter_x    <= 0;
            counter_y    <= 0;
            counter_z    <= 0;
            counter_id   <= 0;
            all_cta_sent <= 0;
            raster_bus_out[0].req_valid <= 0;
        end else begin
            if (!all_cta_sent && raster_bus_out[0].req_ready) begin
                // Prepare and send one CTA block
                raster_bus_out[0].req_data.stamps[0].num_warps <= total_warps;
                raster_bus_out[0].req_data.stamps[0].start_pc  <= kmu_data.pc;
                raster_bus_out[0].req_data.stamps[0].param     <= kmu_data.param;
                raster_bus_out[0].req_data.stamps[0].cta_x     <= counter_x;
                raster_bus_out[0].req_data.stamps[0].cta_y     <= counter_y;
                raster_bus_out[0].req_data.stamps[0].cta_z     <= counter_z;
                raster_bus_out[0].req_data.stamps[0].cta_id    <= counter_id;
                raster_bus_out[0].req_valid <= 1;

                // Advance to next CTA block
                counter_z  <= counter_z + 1;
                counter_id <= counter_id + 1;
                if (counter_z + 1 >= kmu_data.grid_dim[2]) begin
                    counter_z <= 0;
                    counter_y <= counter_y + 1;
                    if (counter_y + 1 >= kmu_data.grid_dim[1]) begin
                        counter_y <= 0;
                        counter_x <= counter_x + 1;
                        if (counter_x + 1 >= kmu_data.grid_dim[0]) begin
                            all_cta_sent <= 1;
                        end
                    end
                end
            end else begin
                raster_bus_out[0].req_valid <= 0;
            end
        end
    end

    // Start signal logic (optional, can be customized)
    always_ff @(posedge clk) begin
        if (reset)
            start <= 0;
        else
            start <= 1;
    end

endmodule
