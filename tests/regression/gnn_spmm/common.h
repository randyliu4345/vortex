#ifndef _GNN_SPMM_COMMON_H_
#define _GNN_SPMM_COMMON_H_

#include <stdint.h>

#ifndef GNN_SPMM_FEATURE_DIM
#define GNN_SPMM_FEATURE_DIM 16u
#endif

// kernel_main dispatches on entry_kind: master bootstrap vs SpMM child grid.
#define GNN_ENTRY_MASTER 0x474E4D53u  // 'GNMS'
#define GNN_ENTRY_SPMM   0x474E5350u  // 'GNSP'

#define GNN_AFFINITY_HOME   0u
#define GNN_AFFINITY_ANY    1u
#define GNN_AFFINITY_REMOTE 2u

// Per-mini SpMM arguments (device-launched child uses this layout).
typedef struct {
  uint32_t entry_kind;         // must be GNN_ENTRY_SPMM
  uint32_t reserved0;
  uint64_t row_ptr_addr;
  uint64_t col_ind_addr;
  uint64_t features_in_addr;
  uint64_t features_out_addr;
  uint32_t start_node;
  uint32_t end_node;
} kernel_arg_t;

// Host-launched master: one thread loops and issues VX_CSR_KMU_LAUNCH for each
// mini-partition (vx_kmu_launch_desc_t + core_affinity per vx_launch.h).
typedef struct {
  uint32_t entry_kind;         // must be GNN_ENTRY_MASTER
  uint32_t reserved0;
  uint64_t mini_partition_start_addr;
  uint64_t mini_partition_end_addr;
  uint64_t mini_partition_affinity_addr;
  uint64_t child_arg_pool_addr;
  uint64_t kernel_pc;
  uint32_t num_minis;
  uint32_t affinity_mode;  // GNN_AFFINITY_HOME | ANY | REMOTE
  uint32_t child_block_x;
  uint32_t num_cores;
} master_arg_t;

#endif  // _GNN_SPMM_COMMON_H_
