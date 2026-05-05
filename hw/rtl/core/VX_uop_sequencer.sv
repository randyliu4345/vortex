// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

`include "VX_define.vh"

module VX_uop_sequencer import
    VX_gpu_pkg::*; (
    input clk,
    input reset,

    VX_ibuffer_if.slave  input_if,
    VX_ibuffer_if.master output_if
);
    ibuffer_t uop_data;

    wire is_uop_input;
    wire uop_start = input_if.valid && is_uop_input;
    wire uop_next = output_if.ready;
    wire uop_done;

    // TODO: Detect vx.ldm / vx.stm instructions (matrix LSU extension).
    // Hint: check ex_type == EX_LSU && inst_lsu_is_mat(op_type)
    // Also handle EXT_TCU_ENABLE for WMMA, then set is_uop_input accordingly.
    wire is_ldm_stm = 1'b0; // TODO: implement
    wire is_wmma    = 1'b0; // TODO: implement (requires EXT_TCU_ENABLE guard)
    assign is_uop_input = is_wmma || is_ldm_stm;

    ibuffer_t wmma_uop_data;
    // TODO: Instantiate VX_lsu_uops to expand each vx.ldm/vx.stm into 8 uops.
    // For each uop r=0..7:
    //   - Increment rd (MLD) or rs2 (MST) fragment register by r
    //   - Stamp r into op_args.ldm.r
    // Connect ldm_done and ldm_uop_data to the mux below.
    ibuffer_t ldm_uop_data;
    wire wmma_done;

`ifdef EXT_TCU_ENABLE
    VX_tcu_uops tcu_uops (
        .clk     (clk),
        .reset   (reset),
        .ibuf_in (input_if.data),
        .ibuf_out(wmma_uop_data),
        .start   (is_wmma && uop_start),
        .next    (is_wmma && uop_next),
        .done    (wmma_done)
    );
`else
    assign wmma_uop_data = '0;
    assign wmma_done     = 1'b0;
`endif

    // TODO: Instantiate VX_lsu_uops here and connect ldm_done, ldm_uop_data
    wire ldm_done = 1'b0; // TODO: connect to VX_lsu_uops
    assign uop_data = is_ldm_stm ? ldm_uop_data : wmma_uop_data;
    assign uop_done = is_ldm_stm ? ldm_done : wmma_done;

    reg uop_active;

    always_ff @(posedge clk) begin
        if (reset) begin
            uop_active <= 0;
        end else begin
            if (uop_active) begin
                if (uop_next && uop_done) begin
                    uop_active <= 0;
                end
            end
            else if (uop_start) begin
                uop_active <= 1;
            end
        end
    end

    // output assignments
    wire uop_hold = ~uop_active && is_uop_input; // hold transition cycles to uop_active
    assign output_if.valid = uop_active ? 1'b1 : (input_if.valid && ~uop_hold);
    assign output_if.data  = uop_active ? uop_data : input_if.data;
    assign input_if.ready  = uop_active ? (output_if.ready && uop_done) : (output_if.ready && ~uop_hold);

endmodule
