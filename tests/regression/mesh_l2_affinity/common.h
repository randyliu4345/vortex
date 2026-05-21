#ifndef _MESH_L2_AFFINITY_COMMON_H_
#define _MESH_L2_AFFINITY_COMMON_H_

#include <stdint.h>

#define MESH_ENTRY_MASTER 0x4D4D5354u  /* 'MMST' */
#define MESH_ENTRY_CHILD  0x4D4C3243u  /* 'ML2C' */

#define MESH_AFFINITY_HOME   0u
#define MESH_AFFINITY_ANY    1u
#define MESH_AFFINITY_REMOTE 2u

typedef struct {
  uint32_t entry_kind;
  uint32_t partition_id;
  uint64_t src_addr;
  uint64_t dst_addr;
  uint64_t remote_src_addr;
  uint32_t num_floats;
} kernel_arg_t;

typedef struct {
  uint32_t entry_kind;
  uint32_t affinity_mode;
  uint64_t partition_src_addr_addr;
  uint64_t partition_dst_addr_addr;
  uint64_t partition_remote_src_addr_addr;
  uint64_t child_arg_pool_addr;
  uint64_t kernel_pc;
  uint32_t num_partitions;
  uint32_t child_block_x;
  uint32_t num_cores;
} master_arg_t;

#endif /* _MESH_L2_AFFINITY_COMMON_H_ */
