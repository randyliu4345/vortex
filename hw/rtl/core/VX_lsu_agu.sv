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

// VX_lsu_agu: per-lane address generator for vx.ldm / vx.stm uops
// (matrix load/store LSU extension). Computes the blocked address using
// the same formula as load_matrix_sync / store_matrix_sync:
//
//   addr[L] = base + (row(L, r) * ldm + col(L, r)) * elem_bytes
//
// The (row, col) formula is role-specific (matrix_a / matrix_b / accumulator)
// and uses the tile constants from VX_tcu_pkg. The extension is always
// enabled (independent of EXT_TCU_ENABLE) — it's an LSU addressing mode.

`include "VX_define.vh"

module VX_lsu_agu import
    VX_gpu_pkg::*; #(
    parameter LANE_IDX = 0          // per-lane instantiation
) (
    input  logic [1:0]      role,      // 0=A, 1=B, 2=C
    input  logic [1:0]      es,        // element size log2
    input  logic            t,         // transpose
    input  logic [2:0]      r,         // uop counter
    input  logic [`XLEN-1:0] base,     // byte base address
    input  logic [`XLEN-1:0] ldm,      // leading dimension (in elements)
    output logic [`XLEN-1:0] addr      // byte address for this lane/uop
);
    // -------- Fragment-tile constants derived from NUM_THREADS --------
    // Mirrors the C++ wmma_config_t in sim/common/tensor_cfg.h. Duplicated here
    // so the matrix LSU extension is available independently of EXT_TCU_ENABLE
    // (VX_tcu_pkg may not be compiled in no-TCU builds).
    localparam NT = `NUM_THREADS;
    localparam NR = 8;
    localparam TILE_CAP = NT * NR;
    localparam LG_TILE_CAP = $clog2(TILE_CAP);
    localparam TILE_EN = LG_TILE_CAP / 2;
    localparam TILE_EM = LG_TILE_CAP - TILE_EN;
    localparam TILE_M = 1 << TILE_EM;
    localparam TILE_N = 1 << TILE_EN;
    localparam TILE_K = TILE_CAP / ((TILE_M > TILE_N) ? TILE_M : TILE_N);
    localparam BLOCK_CAP = NT;
    localparam LG_BLOCK_CAP = $clog2(BLOCK_CAP);
    localparam BLOCK_EN = LG_BLOCK_CAP / 2;
    localparam BLOCK_EM = LG_BLOCK_CAP - BLOCK_EN;
    localparam TC_M = 1 << BLOCK_EM;
    localparam TC_N = 1 << BLOCK_EN;
    localparam TC_K = BLOCK_CAP / ((TC_M > TC_N) ? TC_M : TC_N);
    localparam K_STEPS = TILE_K / TC_K;
    localparam N_STEPS = TILE_N / TC_N;
    localparam A_BLOCK_SIZE = TC_M * TC_K;
    localparam A_SUB_BLOCKS = BLOCK_CAP / A_BLOCK_SIZE;
    localparam B_BLOCK_SIZE = TC_K * TC_N;
    localparam B_SUB_BLOCKS = BLOCK_CAP / B_BLOCK_SIZE;
    localparam B_SUB_STEPS  = N_STEPS / B_SUB_BLOCKS;

    // i_ratio = sizeof(vreg_t) / sizeof(elem_t) = 4 / (1 << es), clamped to >=1
    logic [2:0] i_ratio;
    always_comb begin
        case (es)
            2'd0: i_ratio = 3'd4;   // 8-bit
            2'd1: i_ratio = 3'd2;   // 16-bit
            default: i_ratio = 3'd1; // 32/64-bit
        endcase
    end

    localparam L = LANE_IDX;
    localparam LW = `XLEN;

    // Role A (matrix_a)
    localparam L_IN_BLK_A  = (A_BLOCK_SIZE == NT) ? L : (L % A_BLOCK_SIZE);
    localparam BLOCK_IDX_A = (A_BLOCK_SIZE == NT) ? 0 : (L / A_BLOCK_SIZE);
    localparam A_BR_BASE   = (L_IN_BLK_A / TC_K) + (BLOCK_IDX_A * TC_M);
    localparam A_BC_BASE   = (L_IN_BLK_A % TC_K);
    localparam A_M_STRIDE  = A_SUB_BLOCKS * TC_M;

    // Role C (accumulator)
    localparam C_BR_BASE   = L / TC_N;
    localparam C_BC_BASE   = L % TC_N;

    // Role B (matrix_b)
    localparam L_IN_BLK_B  = (B_BLOCK_SIZE == NT) ? L : (L % B_BLOCK_SIZE);
    localparam BLOCK_IDX_B = (B_BLOCK_SIZE == NT) ? 0 : (L / B_BLOCK_SIZE);
    localparam B_BC_BASE   = (L_IN_BLK_B / TC_K) + (BLOCK_IDX_B * TC_N);
    localparam B_BR_BASE   = (L_IN_BLK_B % TC_K);
    localparam B_N_STRIDE  = B_SUB_BLOCKS * TC_N;

    // Per-register decomposition
    wire [2:0] block_m_A = r / 3'(K_STEPS);
    wire [2:0] block_k_A = r % 3'(K_STEPS);
    wire [2:0] block_k_B = r / 3'(B_SUB_STEPS);
    wire [2:0] block_n_B = r % 3'(B_SUB_STEPS);
    wire [2:0] block_m_C = r / 3'(N_STEPS);
    wire [2:0] block_n_C = r % 3'(N_STEPS);

    logic [LW-1:0] block_row, block_col;
    logic [LW-1:0] elem_row, elem_col;

    always_comb begin
        case (role)
            2'd0: begin // matrix_a
                block_row = LW'(A_BR_BASE);
                block_col = LW'(A_BC_BASE * i_ratio);
                elem_row  = LW'(block_m_A) * LW'(A_M_STRIDE);
                elem_col  = LW'(block_k_A) * (LW'(TC_K) * LW'(i_ratio));
            end
            2'd1: begin // matrix_b
                block_row = LW'(B_BR_BASE * i_ratio);
                block_col = LW'(B_BC_BASE);
                elem_row  = LW'(block_k_B) * (LW'(TC_K) * LW'(i_ratio));
                elem_col  = LW'(block_n_B) * LW'(B_N_STRIDE);
            end
            default: begin // accumulator (C)
                block_row = LW'(C_BR_BASE);
                block_col = LW'(C_BC_BASE);
                elem_row  = LW'(block_m_C) * LW'(TC_M);
                elem_col  = LW'(block_n_C) * LW'(TC_N);
            end
        endcase
    end

    // Transpose swaps row/col at both lane-origin and register-step
    logic [LW-1:0] row_eff, col_eff;
    always_comb begin
        if (t) begin
            row_eff = block_col + elem_col;
            col_eff = block_row + elem_row;
        end else begin
            row_eff = block_row + elem_row;
            col_eff = block_col + elem_col;
        end
    end

    // Final byte address: base + (row * ldm + col) << es
    wire [LW-1:0] offset_elems = row_eff * ldm + col_eff;
    assign addr = base + (offset_elems << es);

endmodule
