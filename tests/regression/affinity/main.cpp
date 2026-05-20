#include <cstdint>
#include <iostream>
#include <vector>
#include <vortex.h>

#include "common.h"

#define RT_CHECK(_expr)                                           \
  do {                                                            \
    int _ret = _expr;                                             \
    if (0 == _ret)                                                \
      break;                                                      \
    std::cerr << "Error: '" << #_expr << "' returned " << _ret   \
              << "!\n";                                           \
    cleanup();                                                    \
    return -1;                                                    \
  } while (false)

static const char* kernel_file = "kernel.vxbin";

static vx_device_h device = nullptr;
static vx_buffer_h out_buffer = nullptr;
static vx_buffer_h krnl_buffer = nullptr;
static vx_buffer_h args_buffer = nullptr;
static kernel_arg_t kernel_arg = {};

static void cleanup() {
  if (device) {
    vx_mem_free(out_buffer);
    vx_mem_free(krnl_buffer);
    vx_mem_free(args_buffer);
    vx_dev_close(device);
  }
}

int main() {
  RT_CHECK(vx_dev_open(&device));

  uint64_t num_cores = 0;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_CORES, &num_cores));
  if (num_cores == 0) {
    std::cerr << "Error: NUM_CORES is zero\n";
    cleanup();
    return -1;
  }

  RT_CHECK(vx_mem_alloc(device, sizeof(uint32_t), VX_MEM_READ_WRITE, &out_buffer));
  RT_CHECK(vx_mem_address(out_buffer, &kernel_arg.out_addr));
  RT_CHECK(vx_upload_kernel_file(device, kernel_file, &krnl_buffer));
  RT_CHECK(vx_upload_bytes(device, &kernel_arg, sizeof(kernel_arg_t), &args_buffer));

  uint32_t grid_dim[1] = {1};
  uint32_t block_dim[1] = {1};

  std::vector<uint32_t> pinned_targets;
  pinned_targets.push_back(0);
  if (num_cores > 1) {
    pinned_targets.push_back(static_cast<uint32_t>(num_cores - 1));
  }

  for (uint32_t target_core : pinned_targets) {
    uint32_t zero = 0;
    RT_CHECK(vx_copy_to_dev(out_buffer, &zero, 0, sizeof(uint32_t)));
    RT_CHECK(vx_start_g_affine(device, krnl_buffer, args_buffer, 1, grid_dim, block_dim,
                               0, target_core));
    RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));
    uint32_t observed = 0;
    RT_CHECK(vx_copy_from_dev(&observed, out_buffer, 0, sizeof(uint32_t)));
    if (observed != target_core) {
      std::cerr << "affinity mismatch: pinned_core=" << target_core
                << " vx_core_id()=" << observed << " (num_cores=" << num_cores << ")\n";
      cleanup();
      return 1;
    }
  }

  {
    uint32_t zero = 0;
    RT_CHECK(vx_copy_to_dev(out_buffer, &zero, 0, sizeof(uint32_t)));
    RT_CHECK(vx_start_g_affine(device, krnl_buffer, args_buffer, 1, grid_dim, block_dim, 0,
                               VORTEX_AFFINITY_ANY));
    RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));
    uint32_t observed = 0;
    RT_CHECK(vx_copy_from_dev(&observed, out_buffer, 0, sizeof(uint32_t)));
    if (observed >= static_cast<uint32_t>(num_cores)) {
      std::cerr << "VORTEX_AFFINITY_ANY: invalid vx_core_id()=" << observed
                << " (num_cores=" << num_cores << ")\n";
      cleanup();
      return 1;
    }
  }

  cleanup();
  std::cout << "PASSED!\n";
  return 0;
}
