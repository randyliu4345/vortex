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

// VX_lsu_uops: sequences vx.ldm / vx.stm (matrix load/store LSU extension)
// into 8 LSU uops, one per fragment register. Each uop targets FP register
// (reg0 + r) and carries the counter r in op_args.ldm.r so VX_lsu_agu inside
// VX_lsu_slice knows which blocked address to compute.
//
//  ldm uop: op_type = INST_LSU_MLD
//           rd  = reg0 + r   (FP writeback)
//           rs1 = base        (INT, uniform)
//           rs2 = ldm         (INT, uniform)
//  stm uop: op_type = INST_LSU_MST
//           rs2 = reg0 + r    (FP source)
//           rs1 = base        (INT, uniform)
//           rs3 = ldm         (INT, uniform)

`include "VX_define.vh"

module VX_lsu_uops import
    VX_gpu_pkg::*; (
    input clk,
    input reset,

    input  ibuffer_t ibuf_in,
    output ibuffer_t ibuf_out,
    input  wire      start,
    input  wire      next,
    output reg       done
);
    localparam NUM_MEM_UOPS = 8;
    localparam CTR_W        = 3;  // 0..7

    reg [CTR_W-1:0] counter;

    wire is_store = (ibuf_in.op_type == INST_OP_BITS'(INST_LSU_MST));

    // Extract raw register index (lower 5 bits) from the typed reg number.
    // `make_reg_num(type, idx)` packs type in the upper bits; we only add to idx.
    localparam REG_IDX_W = 5;

    // For MLD, base register is rd; for MST, base register is rs2 (FP source).
    wire [REG_IDX_W-1:0] reg0_idx = is_store ? ibuf_in.rs2[REG_IDX_W-1:0]
                                             : ibuf_in.rd [REG_IDX_W-1:0];
    wire [REG_IDX_W-1:0] reg_r_idx = reg0_idx + REG_IDX_W'(counter);

    // Rebuild typed reg number with the incremented index (type bits preserved).
    localparam TYPE_HI = NUM_REGS_BITS - 1;
    localparam TYPE_LO = REG_IDX_W;
    wire [NUM_REGS_BITS-REG_IDX_W-1:0] rd_type_bits  = ibuf_in.rd [TYPE_HI:TYPE_LO];
    wire [NUM_REGS_BITS-REG_IDX_W-1:0] rs2_type_bits = ibuf_in.rs2[TYPE_HI:TYPE_LO];

    wire [NUM_REGS_BITS-1:0] new_rd  = {rd_type_bits,  reg_r_idx};
    wire [NUM_REGS_BITS-1:0] new_rs2 = {rs2_type_bits, reg_r_idx};

`ifdef UUID_ENABLE
    wire [31:0] uuid_lo = {counter, ibuf_in.uuid[0 +: (32-CTR_W)]};
    wire [UUID_WIDTH-1:0] uuid = {ibuf_in.uuid[UUID_WIDTH-1:32], uuid_lo};
`else
    wire [UUID_WIDTH-1:0] uuid = ibuf_in.uuid;
`endif

    // Output uop
    assign ibuf_out.uuid    = uuid;
    assign ibuf_out.tmask   = ibuf_in.tmask;
    assign ibuf_out.PC      = ibuf_in.PC;
    assign ibuf_out.ex_type = ibuf_in.ex_type;
    assign ibuf_out.op_type = ibuf_in.op_type;
    assign ibuf_out.wb      = ibuf_in.wb;
    assign ibuf_out.used_rs = ibuf_in.used_rs;
    assign ibuf_out.rs1     = ibuf_in.rs1;                 // base, unchanged
    assign ibuf_out.rs3     = ibuf_in.rs3;                 // ldm (for MST), unchanged
    assign ibuf_out.rs2     = is_store ? new_rs2 : ibuf_in.rs2;  // MST increments rs2
    assign ibuf_out.rd      = is_store ? ibuf_in.rd : new_rd;    // MLD increments rd

    // Stamp the per-uop counter into op_args.ldm.r. The other ldm fields
    // (es, t) pass through unchanged from the macro instruction.
    op_args_t out_args;
    always_comb begin
        out_args = ibuf_in.op_args;
        out_args.ldm.r = counter;
    end
    assign ibuf_out.op_args = out_args;

    reg busy;
    always_ff @(posedge clk) begin
        if (reset) begin
            counter <= '0;
            busy    <= 0;
            done    <= 0;
        end else begin
            if (~busy && start) begin
                busy <= 1;
                counter <= '0;
                done <= (NUM_MEM_UOPS == 1);
            end else if (busy && next) begin
                counter <= counter + CTR_W'(1);
                done <= (counter == CTR_W'(NUM_MEM_UOPS - 2));
                busy <= ~done;
            end
        end
    end

endmodule
