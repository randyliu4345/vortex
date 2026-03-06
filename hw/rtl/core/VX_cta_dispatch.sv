module VX_cta_dispatch import VX_gpu_pkg::*;
(
    input wire                              clk,
    input wire                              reset,

    // from KMU
    VX_kmu_bus_if.slave                     kmu_bus_if,

    // from scheduler
    input wire[`NUM_WARPS-1:0]              active_warps,

    // to scheduler
    output wire                             dispatch_fire,
    output wire[`CLOG2(`NUM_WARPS)-1:0]     cta_dispatch_wid,
    output reg[`XLEN-1:0]                   pc,
    output reg[`XLEN-1:0]                   param,
    output wire[`NUM_THREADS-1:0]           tmask,

    // to CSR unit
    output wire                             cta_csr_valid,
    output wire [NW_WIDTH-1:0]              cta_csr_wid,
    output wire cta_csr_data_t              cta_csr_data
);
    // Define states for the FSM
    typedef enum logic [0:0] { IDLE = 1'b0, DISPATCH = 1'b1 } state_t;
    state_t state;

    // Only handshake with KMU when the machine is in non-reset IDLE state
    assign kmu_bus_if.req_ready = (state == IDLE && ~reset);

    // store some kernel info
    logic [31:0]                num_warps;
    reg [`NUM_THREADS-1:0]      cur_remain_mask;



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
        .N              (`NUM_WARPS),
        .DATAW(`CLOG2   (`NUM_WARPS)),
        .REVERSE(0)
    ) find_first_idle (
        .data_in        (warp_indices),
        .valid_in       (~active_warps), // invert to find first '0'
        .data_out       (cta_dispatch_wid),
        .valid_out      (cta_dispatch_valid)
    );

    int warp_counter;
    // Only dispatch when 1. in DISPATCH state and 2. there are slot in activate_warps and 3. there
    //  are some warp to be disptached
    assign dispatch_fire = (state == DISPATCH) && (cta_dispatch_valid) && (warp_counter < num_warps);

    // combinational logic to handle tmask, in this way, it is available when needed
    logic[`NUM_THREADS-1:0] tmask_n;
    always_comb begin
        if (warp_counter == num_warps - 1)
            tmask_n = cur_remain_mask;
        else
            tmask_n = {`NUM_THREADS{1'b1}};
    end

    assign tmask = tmask_n;

    // FSM and dispatch logic
    always_ff @(posedge clk) begin
        if (reset) begin
            state           <= IDLE;
            warp_counter    <= 0;
            pc              <= '0;
            param           <= '0;
            cur_remain_mask <= '0;
            num_warps       <= 0;
            cta_csr_valid   <= 0;
        end else begin
            case (state)
                IDLE: begin
                    warp_counter <= 0;
                    if (kmu_bus_if.req_valid && kmu_bus_if.req_ready) begin
                        /*  When there is a handshake, store the kernel info from kmu
                        then make the FSM do a transition ot DISPATCH state
                        */
                        pc                   <= kmu_bus_if.req_data.start_pc;
                        param                <= kmu_bus_if.req_data.param;
                        num_warps            <= kmu_bus_if.req_data.num_warps;
                        cur_remain_mask      <= kmu_bus_if.req_data.remain_mask;
                        cta_csr_data.cta_x   <= kmu_bus_if.req_data.cta_x;
                        cta_csr_data.cta_y   <= kmu_bus_if.req_data.cta_y;
                        cta_csr_data.cta_z   <= kmu_bus_if.req_data.cta_z;
                        cta_csr_data.cta_id  <= kmu_bus_if.req_data.cta_id;
                        state                <= DISPATCH;
                    end
                end

                DISPATCH: begin
                    if (dispatch_fire) begin
                        /*  update counter, write the cta csr message.
                        The warp is dispatched in the VX_schedule module
                        at the same time
                        */
                        warp_counter        <= warp_counter + 1;
                        cta_csr_data.local_warp_id <= warp_counter;
                        cta_csr_wid         <= cta_dispatch_wid;
                        cta_csr_valid       <= 1;
                    end else begin
                        /*  If not dispatch_fire, no warp is dispatched in the coming cycle
                            So set the cta csr valid to 0 for the coming cycle
                        */
                        cta_csr_valid       <= 0;
                    end

                    // Exit DISPATCH after all warps are dispatched
                    if (warp_counter >= num_warps) begin
                        state <= IDLE;
                    end
                end

                default: state <= IDLE;
            endcase
        end
    end

endmodule
