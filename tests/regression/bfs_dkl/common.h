#ifndef _COMMON_H_
#define _COMMON_H_

#define MAX_NODES 100000

struct Node {
  int starting;
  int no_of_edges;
};

// Same logical fields as tests/regression/bfs/common.h, plus device launch.
typedef struct {
  uint32_t num_nodes;
  uint32_t num_edges;
  uint32_t frontier_size;

  uint64_t nodes_addr;
  uint64_t edges_addr;
  uint64_t nextmask_addr;
  uint64_t visit_addr;
  uint64_t frontier_addr;
  uint64_t cost_addr;

  uint32_t role;
  uint64_t child_pc;
  uint64_t child_arg_addr;
  uint64_t frontier_next_addr;
} kernel_arg_t;

#define BFS_DKL_ROLE_PARENT 0u
#define BFS_DKL_ROLE_WORKER 1u

#endif
