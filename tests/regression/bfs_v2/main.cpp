#include <iostream>
#include <string.h>
#include <unistd.h>
#include <vector>
#include <chrono>
#include <vortex.h>
#include <VX_types.h>
#include "common.h"

#define RT_CHECK(_expr)                                      \
  do {                                                       \
    int _ret = _expr;                                        \
    if (0 == _ret)                                           \
      break;                                                 \
    printf("Error: '%s' returned %d!\n", #_expr, (int)_ret); \
    cleanup();                                               \
    exit(-1);                                                \
  } while (false)

#define BFSV2_ROLE_PARENT 0u

const char* kernel_file = "kernel.vxbin";
uint32_t size = 1024;

vx_device_h device = nullptr;
vx_buffer_h nodes_buffer = nullptr;
vx_buffer_h edges_buffer = nullptr;
vx_buffer_h visit_buffer = nullptr;
vx_buffer_h next_size_counter_buffer = nullptr;
vx_buffer_h blocks_done_counter_buffer = nullptr;
vx_buffer_h frontier_a_buffer = nullptr;
vx_buffer_h frontier_b_buffer = nullptr;
vx_buffer_h cost_buffer = nullptr;
vx_buffer_h krnl_buffer = nullptr;
vx_buffer_h parent_args_buffer = nullptr;
vx_buffer_h child_args_buffer = nullptr;

static void show_usage() {
  std::cout << "Vortex BFS v2 (device-side level launches)." << std::endl;
  std::cout << "Usage: [-k: kernel] [-n words] [-h: help]" << std::endl;
}

static void parse_args(int argc, char** argv) {
  int c;
  while ((c = getopt(argc, argv, "n:k:h")) != -1) {
    switch (c) {
    case 'n': size = atoi(optarg); break;
    case 'k': kernel_file = optarg; break;
    case 'h': show_usage(); exit(0);
    default: show_usage(); exit(-1);
    }
  }
}

void cleanup() {
  if (device) {
    if (nodes_buffer) vx_mem_free(nodes_buffer);
    if (edges_buffer) vx_mem_free(edges_buffer);
    if (visit_buffer) vx_mem_free(visit_buffer);
    if (next_size_counter_buffer) vx_mem_free(next_size_counter_buffer);
    if (blocks_done_counter_buffer) vx_mem_free(blocks_done_counter_buffer);
    if (frontier_a_buffer) vx_mem_free(frontier_a_buffer);
    if (frontier_b_buffer) vx_mem_free(frontier_b_buffer);
    if (cost_buffer) vx_mem_free(cost_buffer);
    if (krnl_buffer) vx_mem_free(krnl_buffer);
    if (parent_args_buffer) vx_mem_free(parent_args_buffer);
    if (child_args_buffer) vx_mem_free(child_args_buffer);
    vx_dev_close(device);
  }
}

static uint64_t query_e2e_sim_cycles(vx_device_h dev) {
  uint64_t num_cores = 0;
  RT_CHECK(vx_dev_caps(dev, VX_CAPS_NUM_CORES, &num_cores));

  uint64_t max_cycles = 0;
  for (uint32_t core_id = 0; core_id < num_cores; ++core_id) {
    uint64_t cycles = 0;
    RT_CHECK(vx_mpm_query(dev, 0, VX_CSR_MCYCLE, core_id, &cycles));
    if (cycles > max_cycles) {
      max_cycles = cycles;
    }
  }
  return max_cycles;
}

static uint64_t host_cycle_counter() {
#if defined(__x86_64__) || defined(__i386__)
  return __builtin_ia32_rdtsc();
#else
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
#endif
}

template <typename Type>
class Comparator {};

template <>
class Comparator<int> {
public:
  static int generate() {
    return rand() % 100;
  }
};

void generate_random_graph(int num_nodes, int max_edges_per_node,
                           std::vector<Node>& nodes, std::vector<int>& edges) {
  nodes.resize(num_nodes);
  edges.clear();

  int edge_count = 0;
  for (int i = 0; i < num_nodes; ++i) {
    nodes[i].starting = edge_count;
    int num_edges = rand() % (max_edges_per_node + 1);
    nodes[i].no_of_edges = num_edges;

    for (int e = 0; e < num_edges; ++e) {
      int dest_node = Comparator<int32_t>::generate() % num_nodes;
      edges.push_back(dest_node);
      edge_count++;
    }
  }
}

static void bfs_cpu(Node graph_nodes[], int graph_edges[], int num_nodes, int source, int cost[]) {
  int queue[MAX_NODES];
  int head = 0, tail = 0;

  for (int i = 0; i < num_nodes; i++) {
    cost[i] = -1;
  }

  cost[source] = 0;
  queue[tail++] = source;

  while (head < tail) {
    int current = queue[head++];
    Node node = graph_nodes[current];
    int start = node.starting;
    int end = start + node.no_of_edges;

    for (int i = start; i < end; i++) {
      int neighbor = graph_edges[i];
      if (cost[neighbor] == -1) {
        cost[neighbor] = cost[current] + 1;
        queue[tail++] = neighbor;
      }
    }
  }
}

int main(int argc, char* argv[]) {
  parse_args(argc, argv);
  std::srand(50);

  std::cout << "open device connection" << std::endl;
  RT_CHECK(vx_dev_open(&device));

  int max_edges_per_node = 5;
  std::vector<Node> node_data;
  std::vector<int> edge_data;
  generate_random_graph(size, max_edges_per_node, node_data, edge_data);

  uint32_t num_nodes = size;
  uint32_t num_edges = edge_data.size();

  uint32_t nodes_buf_size = num_nodes * sizeof(Node);
  uint32_t edges_buf_size = num_edges * sizeof(int32_t);
  uint32_t visit_buf_size = num_nodes * sizeof(uint32_t);
  uint32_t next_size_counter_buf_size = sizeof(uint32_t);
  uint32_t blocks_done_counter_buf_size = sizeof(uint32_t);
  uint32_t frontier_buf_size = num_nodes * sizeof(uint32_t);
  uint32_t cost_buf_size = num_nodes * sizeof(int32_t);

  std::cout << "number of nodes: " << num_nodes << std::endl;
  std::cout << "allocate device memory" << std::endl;
  RT_CHECK(vx_mem_alloc(device, nodes_buf_size, VX_MEM_READ_WRITE, &nodes_buffer));
  RT_CHECK(vx_mem_alloc(device, edges_buf_size, VX_MEM_READ_WRITE, &edges_buffer));
  RT_CHECK(vx_mem_alloc(device, visit_buf_size, VX_MEM_READ_WRITE, &visit_buffer));
  RT_CHECK(vx_mem_alloc(device, next_size_counter_buf_size, VX_MEM_READ_WRITE, &next_size_counter_buffer));
  RT_CHECK(vx_mem_alloc(device, blocks_done_counter_buf_size, VX_MEM_READ_WRITE, &blocks_done_counter_buffer));
  RT_CHECK(vx_mem_alloc(device, frontier_buf_size, VX_MEM_READ_WRITE, &frontier_a_buffer));
  RT_CHECK(vx_mem_alloc(device, frontier_buf_size, VX_MEM_READ_WRITE, &frontier_b_buffer));
  RT_CHECK(vx_mem_alloc(device, cost_buf_size, VX_MEM_READ_WRITE, &cost_buffer));

  uint64_t nodes_addr = 0, edges_addr = 0, visit_addr = 0, next_size_counter_addr = 0, blocks_done_counter_addr = 0;
  uint64_t frontier_a_addr = 0, frontier_b_addr = 0, cost_addr = 0;
  RT_CHECK(vx_mem_address(nodes_buffer, &nodes_addr));
  RT_CHECK(vx_mem_address(edges_buffer, &edges_addr));
  RT_CHECK(vx_mem_address(visit_buffer, &visit_addr));
  RT_CHECK(vx_mem_address(next_size_counter_buffer, &next_size_counter_addr));
  RT_CHECK(vx_mem_address(blocks_done_counter_buffer, &blocks_done_counter_addr));
  RT_CHECK(vx_mem_address(frontier_a_buffer, &frontier_a_addr));
  RT_CHECK(vx_mem_address(frontier_b_buffer, &frontier_b_addr));
  RT_CHECK(vx_mem_address(cost_buffer, &cost_addr));

  std::vector<uint32_t> h_visit(num_nodes, 0);
  std::vector<int32_t> h_cost(num_nodes, -1);
  std::vector<uint32_t> h_frontier0(1, 0);
  uint32_t h_next_size_counter = 0;
  uint32_t h_blocks_done_counter = 0;
  h_visit[0] = 1;
  h_cost[0] = 0;

  RT_CHECK(vx_copy_to_dev(nodes_buffer, node_data.data(), 0, nodes_buf_size));
  RT_CHECK(vx_copy_to_dev(edges_buffer, edge_data.data(), 0, edges_buf_size));
  RT_CHECK(vx_copy_to_dev(visit_buffer, h_visit.data(), 0, visit_buf_size));
  RT_CHECK(vx_copy_to_dev(next_size_counter_buffer, &h_next_size_counter, 0, next_size_counter_buf_size));
  RT_CHECK(vx_copy_to_dev(blocks_done_counter_buffer, &h_blocks_done_counter, 0, blocks_done_counter_buf_size));
  RT_CHECK(vx_copy_to_dev(cost_buffer, h_cost.data(), 0, cost_buf_size));
  RT_CHECK(vx_copy_to_dev(frontier_a_buffer, h_frontier0.data(), 0, sizeof(uint32_t)));

  std::cout << "upload program" << std::endl;
  RT_CHECK(vx_upload_kernel_file(device, kernel_file, &krnl_buffer));
  uint64_t child_pc = 0;
  RT_CHECK(vx_mem_address(krnl_buffer, &child_pc));

  RT_CHECK(vx_mem_alloc(device, sizeof(kernel_arg_t), VX_MEM_READ_WRITE, &child_args_buffer));
  uint64_t child_arg_addr = 0;
  RT_CHECK(vx_mem_address(child_args_buffer, &child_arg_addr));

  kernel_arg_t parent_arg = {};
  parent_arg.role = BFSV2_ROLE_PARENT;
  parent_arg.num_nodes = num_nodes;
  parent_arg.num_edges = num_edges;
  parent_arg.frontier_size = 1;
  parent_arg.child_pc = child_pc;
  parent_arg.child_arg_addr = child_arg_addr;
  parent_arg.nodes_addr = nodes_addr;
  parent_arg.edges_addr = edges_addr;
  parent_arg.visit_addr = visit_addr;
  parent_arg.next_size_counter_addr = next_size_counter_addr;
  parent_arg.blocks_done_counter_addr = blocks_done_counter_addr;
  parent_arg.frontier_a_addr = frontier_a_addr;
  parent_arg.frontier_b_addr = frontier_b_addr;
  parent_arg.cost_addr = cost_addr;

  std::cout << "start device" << std::endl;
  uint64_t host_dev_begin = host_cycle_counter();
  RT_CHECK(vx_upload_bytes(device, &parent_arg, sizeof(parent_arg), &parent_args_buffer));
  uint32_t grid_dim[1] = {1};
  uint32_t block_dim[1] = {1};
  RT_CHECK(vx_start_g(device, krnl_buffer, parent_args_buffer, 1, grid_dim, block_dim, 0));
  RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));
  uint64_t host_dev_end = host_cycle_counter();
  // End-to-end device-side execution cycles across parent+child BFS launches.
  auto e2e_cycles = query_e2e_sim_cycles(device);
  std::cout << "e2e_sim_cycles=" << e2e_cycles << std::endl;
  std::cout << "e2e_host_device_cycles=" << (host_dev_end - host_dev_begin) << std::endl;

  RT_CHECK(vx_copy_from_dev(h_cost.data(), cost_buffer, 0, cost_buf_size));

  std::cout << "verify result" << std::endl;
  std::vector<int> ref_cost(num_nodes);
  bfs_cpu(node_data.data(), edge_data.data(), num_nodes, 0, ref_cost.data());

  int errors = 0;
  for (uint32_t i = 0; i < num_nodes; i++) {
    if (h_cost[i] != ref_cost[i]) {
      if (errors < 16) {
        std::cout << "error at result #" << i << ": actual=" << h_cost[i]
                  << ", expected=" << ref_cost[i] << std::endl;
      }
      ++errors;
    }
  }

  std::cout << "cleanup" << std::endl;
  cleanup();

  if (errors != 0) {
    std::cout << "Found " << errors << " errors!" << std::endl;
    std::cout << "FAILED!" << std::endl;
    return 1;
  }

  std::cout << "PASSED!" << std::endl;
  return 0;
}
