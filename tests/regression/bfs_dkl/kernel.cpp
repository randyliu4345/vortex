#include <vx_spawn2.h>
#include <vx_launch.h>
#include "common.h"

// Match vx_max_occupancy_grid(..., ndim=1, ...): one-dimensional block width.
static __attribute__((always_inline)) uint32_t worker_block_x() {
  return (uint32_t)vx_num_threads();
}

static inline void launch_child_worker(const kernel_arg_t* arg,
                                       kernel_arg_t* child,
                                       uint32_t child_frontier_size,
                                       uint64_t child_frontier_in,
                                       uint64_t child_frontier_out,
                                       uint32_t block_x) {
  *child = *arg;
  child->role = BFS_DKL_ROLE_WORKER;
  child->frontier_size = child_frontier_size;
  child->frontier_addr = child_frontier_in;
  child->frontier_next_addr = child_frontier_out;

  uint32_t grid[3]  = {1u, 1u, 1u};
  uint32_t block[3] = {block_x, 1u, 1u};
  vx_kmu_launch_desc_t desc;
  vx_launch_desc_init(&desc, arg->child_pc,
                      reinterpret_cast<uint64_t>(child), grid, block, 0);
  vx_kernel_launch(&desc);
}

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  uint32_t tid  = blockIdx.x * blockDim.x + threadIdx.x;
  uint32_t bdim = blockDim.x * gridDim.x;
  uint32_t lid  = threadIdx.x;

  if (arg->role == BFS_DKL_ROLE_PARENT) {
    if (lid != 0u)
      return;
    auto* child = reinterpret_cast<kernel_arg_t*>(arg->child_arg_addr);
    launch_child_worker(arg, child, /*frontier_size=*/1u,
                        arg->frontier_addr, arg->frontier_next_addr,
                        worker_block_x());
    return;
  }

  auto* __restrict nodes    = reinterpret_cast<Node*>(arg->nodes_addr);
  auto* __restrict edges    = reinterpret_cast<int32_t*>(arg->edges_addr);
  auto* __restrict visit    = reinterpret_cast<uint8_t*>(arg->visit_addr);
  auto* __restrict nextmask = reinterpret_cast<uint8_t*>(arg->nextmask_addr);
  auto* __restrict frontier_in =
      reinterpret_cast<uint32_t*>(arg->frontier_addr);
  auto* __restrict frontier_out =
      reinterpret_cast<uint32_t*>(arg->frontier_next_addr);
  auto* __restrict cost = reinterpret_cast<int32_t*>(arg->cost_addr);

  uint32_t fsize = arg->frontier_size;
  uint32_t n     = arg->num_nodes;

  // Same expansion as tests/regression/bfs/kernel.cpp (strided over frontier).
  for (uint32_t i = tid; i < fsize; i += bdim) {
    uint32_t v   = frontier_in[i];
    uint32_t end = (uint32_t)nodes[v].starting + (uint32_t)nodes[v].no_of_edges;
    int32_t  cv  = cost[v] + 1;
    for (uint32_t j = (uint32_t)nodes[v].starting; j < end; ++j) {
      uint32_t nid = (uint32_t)edges[j];
      if (!visit[nid]) {
        nextmask[nid] = 1;
        cost[nid]     = cv;
      }
    }
  }

  __syncthreads();

  if (lid != 0u)
    return;

  // Same compaction as the host loop in tests/regression/bfs/main.cpp.
  uint32_t pos = 0u;
  for (uint32_t v = 0u; v < n; ++v) {
    if (nextmask[v]) {
      if (!visit[v]) {
        visit[v]            = 1;
        frontier_out[pos++] = v;
      }
      nextmask[v] = 0;
    }
  }

  if (pos == 0u)
    return;

  auto* child = reinterpret_cast<kernel_arg_t*>(arg->child_arg_addr);
  launch_child_worker(arg, child, pos, arg->frontier_next_addr, arg->frontier_addr,
                      worker_block_x());
}
