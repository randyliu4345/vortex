#include <iostream>
#include <unistd.h>
#include <string.h>
#include <vortex.h>
#include <VX_types.h>
#include "common.h"

#define RT_CHECK(_expr)                                         \
  do {                                                          \
    int _ret = _expr;                                           \
    if (0 == _ret) break;                                       \
    printf("Error: '%s' returned %d!\n", #_expr, (int)_ret);    \
    cleanup();                                                  \
    exit(-1);                                                   \
  } while (false)

const char* kernel_file = "kernel.vxbin";
bool print_words = true;

vx_device_h device = nullptr;
vx_buffer_h krnl_buffer = nullptr;
vx_buffer_h parent_args_buffer = nullptr;

static void show_usage() {
  std::cout << "Vortex dyn_launch (parent/child/tail via KMU tail stream)." << std::endl;
  std::cout << "Usage: [-k: kernel] [-q quiet] [-h: help]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "k:qh")) != -1) {
    switch (c) {
    case 'k': kernel_file = optarg; break;
    case 'q': print_words = false; break;
    case 'h': show_usage(); exit(0);
    default:  show_usage(); exit(-1);
    }
  }
}

void cleanup() {
  if (device) {
    if (krnl_buffer) vx_mem_free(krnl_buffer);
    if (parent_args_buffer) vx_mem_free(parent_args_buffer);
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

int main(int argc, char *argv[]) {
  parse_args(argc, argv);

  std::cout << "open device" << std::endl;
  RT_CHECK(vx_dev_open(&device));

  std::cout << "upload kernel" << std::endl;
  RT_CHECK(vx_upload_kernel_file(device, kernel_file, &krnl_buffer));

  kernel_arg_t parent_arg = {};
  parent_arg.print_words = print_words ? 1u : 0u;

  std::cout << "upload args" << std::endl;
  RT_CHECK(vx_upload_bytes(device, &parent_arg, sizeof(parent_arg), &parent_args_buffer));

  std::cout << "start parent kernel (expect device prints: Hello World!)" << std::endl;
  uint32_t grid_dim[1]  = { 1 };
  uint32_t block_dim[1] = { 1 };
  RT_CHECK(vx_start_g(device, krnl_buffer, parent_args_buffer, 1, grid_dim, block_dim, 0));

  std::cout << "wait for completion" << std::endl;
  RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));

  auto e2e_cycles = query_e2e_sim_cycles(device);
  std::cout << "e2e_sim_cycles=" << e2e_cycles << std::endl;

  cleanup();
  std::cout << "PASSED!" << std::endl;
  return 0;
}
