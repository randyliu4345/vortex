`include "VX_define.vh"

module VX_kmu import VX_gpu_pkg::*; (
    input wire clk,
    input wire reset,

    // DCR write request
    input  wire                             dcr_wr_valid,
    input  wire [VX_DCR_ADDR_WIDTH-1:0]     dcr_wr_addr,
    input  wire [VX_DCR_DATA_WIDTH-1:0]     dcr_wr_data,

    VX_kmu_task_if.master                   task_interface[`NUM_CLUSTERS * `NUM_CORES],

    // signal to start execution
    output reg start
);

    kmu_data_t kmu_data;
    logic[31:0] smem_size = 0;

    `UNUSED_VAR(smem_size);

    logic[31:0] cta_x[`NUM_CLUSTERS * `NUM_CORES];
    logic[31:0] cta_y[`NUM_CLUSTERS * `NUM_CORES];
    logic[31:0] cta_z[`NUM_CLUSTERS * `NUM_CORES];
    logic[31:0] cta_id[`NUM_CLUSTERS * `NUM_CORES];
    logic core_ready[`NUM_CLUSTERS * `NUM_CORES];

    logic[31:0] total_threads;

    logic[31:0] total_warps;

    for(genvar i  = 0; i < `NUM_CLUSTERS * `NUM_CORES; ++i) begin : g_task_distr
        assign task_interface[i].task_data.num_warps = total_warps;

        assign task_interface[i].task_data.start_pc = kmu_data.pc;
        assign task_interface[i].task_data.param = kmu_data.param;
        assign task_interface[i].task_data.cta_x = cta_x[i];
        assign task_interface[i].task_data.cta_y = cta_y[i];
        assign task_interface[i].task_data.cta_z = cta_z[i];
        assign task_interface[i].task_data.cta_id = cta_id[i];
        assign core_ready[i] = task_interface[i].core_ready;
    end

    logic[31:0] counter_x;
    logic[31:0] counter_y;
    logic[31:0] counter_z;
    logic[31:0] counter_id;

    logic[31:0] cur_x;
    logic[31:0] cur_y;
    logic[31:0] cur_z;
    logic[31:0] cur_id;

    logic[`NUM_THREADS-1:0] tmask_v;

    always_comb begin
        total_threads = `MAX(kmu_data.block_dim[0], 1) 
                        * `MAX(kmu_data.block_dim[1], 1) 
                        * `MAX(kmu_data.block_dim[2], 1);

        total_warps = total_threads / `NUM_THREADS;

        if (total_warps * `NUM_THREADS < total_threads) begin
            total_warps++;
        end

        if (total_warps * `NUM_THREADS == total_threads) begin
            tmask_v = {`NUM_THREADS{1'b1}};
        end else begin
            tmask_v = {`NUM_THREADS{1'b1}};
            tmask_v = tmask_v << (total_warps * `NUM_THREADS - total_threads);
        end


        cur_x = counter_x;
        cur_y = counter_y;
        cur_z = counter_z;
        cur_id = counter_id;

        for (int i = 0; i < `NUM_CLUSTERS * `NUM_CORES; ++i) begin
            if (core_ready[i]) begin
                cta_x[i] = cur_x;
                cta_y[i] = cur_y;
                cta_z[i] = cur_z;
                cta_id[i] = cur_id;

                cur_z++;
                cur_id++;

                if (cur_z >= kmu_data.grid_dim[2]) begin
                    cur_z = 0;
                    cur_y++;
                end

                if (cur_y >= kmu_data.grid_dim[1]) begin
                    cur_y = 0;
                    cur_x++;
                end

                if (cur_x >= kmu_data.grid_dim[0]) begin
                    cur_x = -1;
                    // When in this conditional branch,
                    // it means that kmu has distributed all
                    // the task.
                end
            end else begin
                // Default assignment to avoid latch inference
                cta_x[i] = cta_x[i];
                cta_y[i] = cta_y[i];
                cta_z[i] = cta_z[i];
                cta_id[i] = cta_id[i];
            end
        end
    end


    always @ (posedge clk) begin
        if (reset) begin
            counter_x <= '0;
            counter_y <= '0;
            counter_z <= '0;
            counter_id <= '0;
        end else begin
            counter_x <= cur_x;
            counter_y <= cur_y;
            counter_z <= cur_z;
            counter_id <= cur_id;
        end
        if (dcr_wr_valid) begin
            case(dcr_wr_addr)
                // PC
                `VX_DCR_BASE_STARTUP_ADDR0: begin
                    kmu_data.pc <= dcr_wr_data;
                end

                // PARAM
                `VX_DCR_BASE_STARTUP_ARG0: begin
                    kmu_data.param <= dcr_wr_data;
                end

                // Grid_dim
                `VX_DCR_BASE_GRID_DIM0: begin
                    kmu_data.grid_dim[0] <= dcr_wr_data;
                end
                `VX_DCR_BASE_GRID_DIM1: begin
                    kmu_data.grid_dim[1] <= dcr_wr_data;
                end
                `VX_DCR_BASE_GRID_DIM2: begin
                    kmu_data.grid_dim[2] <= dcr_wr_data;
                end

                // Block_dim
                `VX_DCR_BASE_BLOCK_DIM0: begin
                    kmu_data.block_dim[0] <= dcr_wr_data;
                end
                `VX_DCR_BASE_BLOCK_DIM1: begin
                    kmu_data.block_dim[1] <= dcr_wr_data;
                end
                `VX_DCR_BASE_BLOCK_DIM2: begin
                    kmu_data.block_dim[2] <= dcr_wr_data;
                end
                `VX_DCR_BASE_SMEM_SIZE: begin
                    smem_size <= dcr_wr_data;
                end

                default: begin
                    // `ASSERT(0, ("%t: invalid DCR write address: %0h", $time, dcr_bus_if.write_addr));
                end
            endcase
        end

        start <= reset;

        if (smem_size != 0) begin
            if (total_threads <= `NUM_CLUSTERS * `NUM_CORES * `NUM_THREADS) begin
                `ASSERT(0, ("cannot use shared memory since the cta size is smaller than total hardware threads\n"));
            end
        end
    end

endmodule
