#include <vx_spawn2.h>
#include <vx_launch.h>
#include "common.h"

// Worker CTA size: one CTA filling a full core (all warps × all lanes).
// A single CTA is sufficient because the in-CTA atomics serialize fast,
// and there is no need for cross-CTA coordination on the next-frontier
// counter.
static __attribute__((always_inline)) uint32_t worker_block_x() {
  return (uint32_t)vx_num_threads() * (uint32_t)vx_num_warps();
}

static inline void launch_child_worker(const kernel_arg_t* arg,
                                       kernel_arg_t* child,
                                       uint32_t child_frontier_size,
                                       uint64_t child_in_addr,
                                       uint64_t child_out_addr,
                                       uint32_t block_x) {
  *child = *arg;
  child->role = BFSV2_ROLE_WORKER;
  child->frontier_size = child_frontier_size;
  child->frontier_a_addr = child_in_addr;
  child->frontier_b_addr = child_out_addr;

  uint32_t grid[3]  = {1u, 1u, 1u};
  uint32_t block[3] = {block_x, 1u, 1u};
  vx_kmu_launch_desc_t desc;
  vx_launch_desc_init(&desc, arg->child_pc,
                      reinterpret_cast<uint64_t>(child), grid, block, 0);
  vx_kernel_launch(&desc);
}

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  uint32_t lid  = threadIdx.x;
  uint32_t bdim = blockDim.x;

  // Parent role: only lane 0 (host launches with block=[1,1,1]) bootstraps
  // the first multi-threaded worker over the seed frontier of size 1, and
  // also primes the global next-frontier counter to 0.
  if (arg->role == BFSV2_ROLE_PARENT) {
    if (lid != 0)
      return;
    auto* counter = reinterpret_cast<uint32_t*>(arg->next_size_counter_addr);
    *counter = 0;
    auto* child = reinterpret_cast<kernel_arg_t*>(arg->child_arg_addr);
    launch_child_worker(arg, child, /*frontier_size=*/1u,
                        arg->frontier_a_addr, arg->frontier_b_addr,
                        worker_block_x());
    return;
  }

  auto* nodes = reinterpret_cast<Node*>(arg->nodes_addr);
  auto* edges = reinterpret_cast<int32_t*>(arg->edges_addr);
  // visit[] is a uint32_t array (matches host allocation) so that the
  // per-node claim can use a 32-bit atomic exchange. Word-granularity AMO
  // is the only RISC-V "A" form Vortex implements.
  auto* visit = reinterpret_cast<uint32_t*>(arg->visit_addr);
  auto* cost  = reinterpret_cast<int32_t*>(arg->cost_addr);
  auto* frontier_in  = reinterpret_cast<uint32_t*>(arg->frontier_a_addr);
  auto* frontier_out = reinterpret_cast<uint32_t*>(arg->frontier_b_addr);
  auto* counter = reinterpret_cast<uint32_t*>(arg->next_size_counter_addr);
  uint32_t fsize = arg->frontier_size;

  // Phase 1: parallel edge expansion with atomic claim + atomic append.
  // Threads stride over the input frontier; each thread expands one vertex
  // at a time. Per-neighbor:
  //   1. A relaxed read of visit[nid] short-circuits the common already-
  //      visited case without going through the AMO unit.
  //   2. amoswap (__sync_lock_test_and_set) atomically takes ownership of
  //      the slot; the thread that observes a 0->1 transition is the unique
  //      claimer and is responsible for cost[] and the frontier append.
  //   3. amoadd (__sync_fetch_and_add) reserves a unique slot in
  //      frontier_out, eliminating the serial compaction phase.
  for (uint32_t i = lid; i < fsize; i += bdim) {
    uint32_t v = frontier_in[i];
    int32_t  cv = cost[v] + 1;
    uint32_t start = nodes[v].starting;
    uint32_t end   = start + nodes[v].no_of_edges;
    for (uint32_t e = start; e < end; ++e) {
      uint32_t nid = (uint32_t)edges[e];
      if (visit[nid] != 0u)
        continue;
      uint32_t old = __sync_lock_test_and_set(&visit[nid], 1u);
      if (old != 0u)
        continue;
      cost[nid] = cv;
      uint32_t pos = __sync_fetch_and_add(counter, 1u);
      frontier_out[pos] = nid;
    }
  }

  __syncthreads();

  // Lane 0 latches the next-frontier size, resets the counter for the
  // child kernel, and chains the next level.
  if (lid != 0)
    return;

  uint32_t total = *counter;
  if (total == 0u)
    return;
  *counter = 0u;

  auto* child = reinterpret_cast<kernel_arg_t*>(arg->child_arg_addr);
  launch_child_worker(arg, child, total,
                      arg->frontier_b_addr, arg->frontier_a_addr,
                      bdim);
}
