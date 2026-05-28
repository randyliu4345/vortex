#ifndef _COMMON_H_
#define _COMMON_H_

#include <stdint.h>

enum moe_op_t {
  MOE_OP_COUNT = 0,
  MOE_OP_SCATTER = 1,
  MOE_OP_PERMUTE = 2,
  MOE_OP_UNPERMUTE = 3,
  MOE_OP_EXPERT_FFN = 5, // fused expert weight load + activation stub (core sweep)
  MOE_OP_MASTER = 6, // device-side dispatcher; bursts T pinned child grids
};

typedef struct {
  uint32_t op;
  uint32_t T;
  uint32_t H;
  uint32_t E;
  uint32_t row_offset;
  uint32_t xperm_off_p;   // float offset into X_perm for this pinned row
  uint32_t w_off_p;       // float offset into W for this row's expert tile
  uint32_t w_row_stride;  // float stride between consecutive k rows in W
  uint64_t X_addr;
  uint64_t X_perm_addr;
  uint64_t W_addr; // [E,H,H] expert weights (float); stride varies with layout
  uint64_t Y_addr;
  uint64_t topk_addr;
  uint64_t expert_counts_addr;
  uint64_t expert_offsets_addr;
  uint64_t expert_cursor_addr;
  uint64_t sorted_token_ids_addr;
} kernel_arg_t;

// Device-side master dispatcher argument: bursts T pinned child grids without
// returning to the host. Parallel master CTAs (num_dispatchers blocks) each
// enqueue rows p = blockIdx.x, blockIdx.x + num_dispatchers, ...; the master
// patches `pool[p].op = child_op` and submits a KMU launch with affinity
// `affinity_addr[p]`. Host runs one multi-block master per phase.
// First field MUST be `op == MOE_OP_MASTER` so the shared kernel_main entry
// can dispatch off the same first uint32 used by kernel_arg_t.
typedef struct {
  uint32_t op;              // == MOE_OP_MASTER
  uint32_t child_op;        // op the children should execute
  uint32_t T;
  uint32_t NT;              // child block.x (= num_threads/block)
  uint32_t grid_x;          // child grid.x (= ceil(H/NT))
  uint32_t num_dispatchers; // master grid.x (= num_cores); shards row launches
  uint32_t pool_page_bytes; // expert_aligned: coarse page; baseline: sizeof(kernel_arg_t)
  uint32_t pool_base_bank;  // coarse bank of child_pool_addr (expert_aligned only)
  uint64_t kernel_pc;       // child kernel entry PC (== krnl_buffer address)
  uint64_t child_pool_addr; // T entries of kernel_arg_t, prefilled by host
  uint64_t affinity_addr;   // T uint32_t core ids (compact)
} master_arg_t;

// One-core expert FFN sweep: fuses weight tile load with in-place activation stub.
typedef struct {
  uint32_t op; // == MOE_OP_EXPERT_FFN
  uint32_t core_id;
  uint32_t T;
  uint32_t H;
  uint32_t E;
  uint32_t by_expert_bin;   // 1: iterate expert bins on core_id; 0: round-robin rows
  uint32_t pool_page_bytes; // sizeof(kernel_arg_t) today; coarse page if pool strided
  uint32_t pool_base_bank;  // coarse bank of child_pool_addr (expert_aligned only)
  uint32_t num_banks;
  int32_t expert_counts[16];
  int32_t expert_offsets[16];
  uint64_t child_pool_addr; // kernel_arg_t[T] or page-strided pool
} expert_ffn_core_arg_t;

#endif
