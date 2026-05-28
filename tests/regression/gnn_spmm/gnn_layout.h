#ifndef _GNN_SPMM_LAYOUT_H_
#define _GNN_SPMM_LAYOUT_H_

#include <stdint.h>
#include "../../../gnn_data.h"

/* Socket-homed address regions for gnn_spmm layout (compile-time override). */
#ifndef GNN_SOCKET_REGION_LOG2
#define GNN_SOCKET_REGION_LOG2 15u
#endif

#define GNN_NODES_PER_SOCKET (GNN_NUM_NODES / GNN_NUM_PHYSICAL_CORES)

static inline uint64_t gnn_feature_byte_offset(uint32_t node) {
  const uint32_t sock  = node / GNN_NODES_PER_SOCKET;
  const uint32_t local = node % GNN_NODES_PER_SOCKET;
  return ((uint64_t)sock << GNN_SOCKET_REGION_LOG2)
       + (uint64_t)local * GNN_FEATURE_DIM * sizeof(float);
}

#endif /* _GNN_SPMM_LAYOUT_H_ */
