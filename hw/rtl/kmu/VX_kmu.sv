`include "VX_define.vh"

task automatic updateCounter(
    input           core_ready,
    input [31:0]    grid_dim_x, 
    input [31:0]    grid_dim_y, 
    input [31:0]    grid_dim_z,
    inout [31:0]    counter_x,
    inout [31:0]    counter_y, 
    inout [31:0]    counter_z,
    inout [31:0]    counter_id,
    output [31:0]   cta_x, 
    output [31:0]   cta_y, 
    output [31:0]   cta_z,
    output [31:0]   cta_id
);
    if (core_ready) begin
        cta_x = counter_x;
        cta_y = counter_y;
        cta_z = counter_z;
        cta_id = counter_id;

        counter_z++;
        counter_id++;

        if (counter_z >= grid_dim_z) begin
            counter_z = 0;
            counter_y++;
        end

        if (counter_y >= grid_dim_y) begin
            counter_y = 0;
            counter_x++;
        end

        if (counter_x >= grid_dim_x) begin
            counter_x = -1;
            // When in this conditional branch,
            // it means that kmu has distributed all
            // the task.
        end
    end

endtask

module VX_kmu import VX_gpu_pkg::*; (
    input wire                              clk,
    input wire                              reset,
    input VX_cmd_processor_if.slave         cmd_processor_input,
    input kmu_data_t                        from_cp,
    VX_kmu_task_if.master                   task_interface[`NUM_CLUSTERS * `NUM_CORES]
);

// All the internal signals are for the testing purpose
// wire [`CLOG2(32+1)-1:0] internal_size/* verilator public */;
wire internal_empty/* verilator public */;

wire internal_ready;

kmu_data_t kmu_data;
`UNUSED_VAR(kmu_data.grid_dim);

reg pop = '0;

VX_fifo_queue #(
    .DATAW (2 * `XLEN + 32*6)
) fifo (
    .clk                    (clk),
    .reset                  (reset),
    .push                   (cmd_processor_input.valid), // assume new_task is off when full
    .pop                    (pop),
    .data_in                (from_cp),
    .data_out               (kmu_data),
    .empty                  (internal_empty),
    .alm_empty              ('x),
    .full                   (internal_ready),
    .alm_full               ('x),
    .size                   ('x)
);

assign cmd_processor_input.ready = ~internal_ready;

logic[31:0] cta_x[`NUM_CLUSTERS * `NUM_CORES];
logic[31:0] cta_y[`NUM_CLUSTERS * `NUM_CORES];
logic[31:0] cta_z[`NUM_CLUSTERS * `NUM_CORES];
logic[31:0] cta_id[`NUM_CLUSTERS * `NUM_CORES];
wire core_ready[`NUM_CLUSTERS * `NUM_CORES];

logic[31:0] counter_x;
logic[31:0] counter_y;
logic[31:0] counter_z;
logic[31:0] counter_id;


for(genvar i  = 0; i < `NUM_CLUSTERS * `NUM_CORES; ++i) begin : g_task_distr
    assign task_interface[i].task_data.num_warps = `MAX(kmu_data.block_dim[0], 1) 
                                            * `MAX(kmu_data.block_dim[1], 1) 
                                            * `MAX(kmu_data.block_dim[2], 1);

    assign task_interface[i].task_data.start_pc = kmu_data.pc;
    assign task_interface[i].task_data.param = kmu_data.param;
    assign task_interface[i].task_data.cta_x = cta_x[i];
    assign task_interface[i].task_data.cta_y = cta_y[i];
    assign task_interface[i].task_data.cta_z = cta_z[i];
    assign task_interface[i].task_data.cta_id = cta_id[i];
    assign core_ready[i] = task_interface[i].core_ready;
end

logic[31:0] cur_x;
logic[31:0] cur_y;
logic[31:0] cur_z;
logic[31:0] cur_id;

always_comb begin
    cur_x = counter_x;
    cur_y = counter_y;
    cur_z = counter_z;
    cur_id = counter_id;

    for (int i = 0; i < `NUM_CLUSTERS * `NUM_CORES; ++i) begin
        updateCounter(
            core_ready[i],
            kmu_data.grid_dim[0],
            kmu_data.grid_dim[1],
            kmu_data.grid_dim[2],
            cur_x,
            cur_y,
            cur_z,
            cur_id,
            cta_x[i],
            cta_y[i],
            cta_z[i],
            cta_id[i]
        );
    end
end

always_ff @(posedge clk) begin
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
end

endmodule
