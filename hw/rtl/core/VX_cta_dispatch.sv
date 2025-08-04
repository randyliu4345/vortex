module VX_cta_dispatch import VX_gpu_pkg::*; 
(
    input wire      clk,
    input wire      reset,

    // CTA task input
    VX_kmu_bus_if.slave         task_in,

    input wire[`NUM_WARPS-1:0]          active_warps,

    // TODO: fix the interface
    output reg[`XLEN-1:0]   pc,
    output reg[`XLEN-1:0]   param,  
    output reg[`NUM_THREADS-1:0] remain_masks,

    VX_cta_csr_if.master                cta_csr_if,

    output wire[`CLOG2(`NUM_WARPS)-1:0]     cta_dispatch_wid,
    output reg                             dispatch_valid
);

    typedef enum logic [0:0] { IDLE = 1'b0, DISPATCH = 1'b1 } state_t;
    state_t state;

    // kernel message that is currently dispatched
    reg [`NUM_THREADS-1:0]      cur_remain_mask;

    int warp_counter;

    assign task_in.req_ready = (state == IDLE && ~reset);

    // Generate indices for data_in
    logic [`NUM_WARPS-1:0][`CLOG2(`NUM_WARPS)-1:0] warp_indices;
    generate
        for (genvar i = 0; i < `NUM_WARPS; ++i) begin : gen_warp_index
            assign warp_indices[i] = i[`CLOG2(`NUM_WARPS)-1:0];
        end
    endgenerate

    logic cta_dispatch_valid;

    // Find the first idle warp (active_warps[i] == 0)
    VX_find_first #(
        .N(`NUM_WARPS),
        .DATAW(`CLOG2(`NUM_WARPS)),
        .REVERSE(0)
    ) find_first_idle (
        .data_in    (warp_indices),
        .valid_in   (~active_warps), // invert to find first '0'
        .data_out   (cta_dispatch_wid),
        .valid_out  (cta_dispatch_valid)
    );

    // assign dispatch_valid = (state == DISPATCH) && (cta_dispatch_valid);
    assign cta_csr_if.wid = cta_dispatch_wid;
    assign cta_csr_if.valid = dispatch_valid;

    // FSM and dispatch logic
    always_ff @(posedge clk) begin
        if (reset) begin
            state         <= IDLE;
            warp_counter  <= 0;
            pc            <= '0;
            param         <= '0;
            remain_masks  <= '0;
            cur_remain_mask <= '0;
            dispatch_valid <= '0;
        end else begin
            case (state)
                IDLE: begin
                    warp_counter <= 0;
                    if (task_in.req_valid && task_in.req_ready) begin
                        pc              <= task_in.req_data.start_pc;
                        param           <= task_in.req_data.param;
                        cur_remain_mask <= task_in.req_data.remain_mask;
                        cta_csr_if.data.cta_x <= task_in.req_data.cta_x;
                        cta_csr_if.data.cta_y <= task_in.req_data.cta_y;
                        cta_csr_if.data.cta_z <= task_in.req_data.cta_z;
                        cta_csr_if.data.cta_id <= task_in.req_data.cta_id;
                        state          <= DISPATCH;
                    end
                end

                DISPATCH: begin
                    if (cta_dispatch_valid && warp_counter < task_in.req_data.num_warps) begin
                        // Set remain_masks for the last warp
                        if (warp_counter == task_in.req_data.num_warps - 1)
                            remain_masks <= cur_remain_mask;
                        else
                            remain_masks <= {`NUM_THREADS{1'b1}};

                        if (dispatch_valid == 0) begin
                            dispatch_valid <= 1;
                        end else begin
                            dispatch_valid <= 0;
                            warp_counter <= warp_counter + 1;
                        end
                    end
                    // Exit DISPATCH after all warps are dispatched
                    if (warp_counter == task_in.req_data.num_warps) begin
                        dispatch_valid <= 0;
                        state <= IDLE;
                    end
                end

                default: state <= IDLE;
            endcase
        end
    end

endmodule
