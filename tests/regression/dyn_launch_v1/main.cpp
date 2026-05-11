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

vx_device_h device = nullptr;
vx_buffer_h krnl_buffer = nullptr;
vx_buffer_h args_buffer = nullptr;

static void show_usage() {
  std::cout << "Vortex dyn_launch_v1 (non-KMU, hello-world)." << std::endl;
  std::cout << "Usage: [-k: kernel] [-h: help]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "k:h")) != -1) {
    switch (c) {
    case 'k': kernel_file = optarg; break;
    case 'h': show_usage(); exit(0);
    default:  show_usage(); exit(-1);
    }
  }
}

void cleanup() {
  if (device) {
    if (krnl_buffer) vx_mem_free(krnl_buffer);
    if (args_buffer) vx_mem_free(args_buffer);
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

  uint8_t dummy = 0;
  std::cout << "upload dummy args (kernel uses nullptr callbacks)" << std::endl;
  RT_CHECK(vx_upload_bytes(device, &dummy, sizeof(dummy), &args_buffer));

  std::cout << "start device (expect device prints: Hello World!)" << std::endl;
  RT_CHECK(vx_start(device, krnl_buffer, args_buffer));

  std::cout << "wait for completion" << std::endl;
  RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));

  // End-to-end device-side execution cycles across the full launch tree.
  // max(core MCYCLE) captures wall-clock simulated cycles for the run.
  auto e2e_cycles = query_e2e_sim_cycles(device);
  std::cout << "e2e_sim_cycles=" << e2e_cycles << std::endl;

  cleanup();
  std::cout << "PASSED!" << std::endl;
  return 0;
}
