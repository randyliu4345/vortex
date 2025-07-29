module VX_cta_dispatch import VX_gpu_pkg::*; 
(
    input wire      clk,
    input wire      reset,

    // some input for tmp test use, subject to change later
    VX_kmu_bus_if.slave         task_in,

    input wire[`NUM_WARPS-1:0]  active_warps,
    input wire[31:0]            num_warps,
    output reg[`NUM_WARPS-1:0] sched_warps,
    output reg[`NUM_WARPS-1:0][31:0] cta_x,
    output reg[`NUM_WARPS-1:0][31:0] cta_y,
    output reg[`NUM_WARPS-1:0][31:0] cta_z,
    output reg[`NUM_WARPS-1:0][31:0] cta_id,
    output reg[`NUM_WARPS-1:0][`XLEN-1:0]   pc,
    output reg[`NUM_WARPS-1:0][`XLEN-1:0]   param,
    output reg[`NUM_WARPS-1:0][`NUM_THREADS-1:0] remain_masks
);

    typedef enum logic [0:0] { IDLE = 1'b0, DISPATCH = 1'b1 } state_t;
    state_t state, next_state;

    reg [`XLEN-1:0] cur_pc;
    reg [`XLEN-1:0] cur_param;
    reg [31:0]      cur_cta_x;
    reg [31:0]      cur_cta_y;
    reg [31:0]      cur_cta_z;
    reg [31:0]      cur_cta_id;
    reg [`NUM_THREADS-1:0]      cur_remain_mask;

    int warp_counter;

    assign task_in.req_ready = ~state;

    // Next state logic
    always_comb begin
        next_state = state;
        if (state == IDLE && task_in.req_valid && task_in.req_ready) begin
            next_state = DISPATCH;
        end

        if (state == DISPATCH && warp_counter >= num_warps) begin
            next_state = IDLE;
        end
    end

    // State register
    always_ff @(posedge clk) begin
        if (reset) begin
            state <= IDLE;
            warp_counter <= 0;
        end else begin
            state <= next_state;
        end

        if (next_state == IDLE) begin
            warp_counter <= 0;
        end

        if (state == IDLE && next_state == DISPATCH) begin
            cur_pc <= task_in.req_data.start_pc;
            cur_param <= task_in.req_data.param;
            cur_cta_x <= task_in.req_data.cta_x;
            cur_cta_y <= task_in.req_data.cta_y;
            cur_cta_z <= task_in.req_data.cta_z;
            cur_cta_id <= task_in.req_data.cta_id;
            cur_remain_mask <= task_in.req_data.remain_mask;
        end
    end

    int idle_warp;
    logic [`NUM_WARPS-1:0] active_warps_n;
    // pick a warp to dispatch
    always_comb begin
        for(idle_warp = 0; idle_warp < `NUM_WARPS; idle_warp++) begin
            if (~active_warps[idle_warp])
                break;
        end

        active_warps_n = active_warps;
        active_warps_n[idle_warp] = 1'b1;
    end

    // dispatch logic
    always_ff @(posedge clk) begin
        if (state == DISPATCH && ~(&active_warps) && warp_counter < num_warps) begin
            warp_counter <= warp_counter + 1;
            sched_warps <= active_warps_n;
            pc[idle_warp] <= cur_pc;
            param[idle_warp] <= cur_param;
            cta_x[idle_warp] <= cur_cta_x;
            cta_y[idle_warp] <= cur_cta_y;
            cta_z[idle_warp] <= cur_cta_z;
            cta_id[idle_warp] <= cur_cta_id;
            remain_masks[idle_warp] <= cur_remain_mask;
        end
    end

    // cta dimension and id logic
    always_ff @(posedge clk) begin
        if (reset) begin
            cta_x <= '0;
            cta_y <= '0;
            cta_z <= '0;
            cta_id <= '0;
            pc <= '0;
            param <= '0;
            remain_masks <= '0;
        end
    end

endmodule
