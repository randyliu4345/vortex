#include <vx_spawn2.h>
#include <vx_launch.h>
#include <vx_print.h>
#include "common.h"

// Global copy so child/tail grids can load args via a plain global pointer
// (uniform arg addresses are not always visible to dynamically launched grids).
static kernel_arg_t g_launch_arg;

static inline uint64_t kernel_entry_pc(void (*entry)(kernel_arg_t*)) {
  return reinterpret_cast<uint64_t>(reinterpret_cast<uintptr_t>(entry));
}

static void launch_tail_grid(uint64_t pc, uint64_t arg) {
  uint32_t grid[3]  = {1u, 1u, 1u};
  uint32_t block[3] = {1u, 1u, 1u};
  vx_kmu_launch_desc_t desc;
  vx_launch_desc_init_tail(&desc, pc, arg, grid, block, /*lmem_size=*/0);
  vx_kernel_launch_tail(&desc);
}

__kernel void kernel_child(kernel_arg_t* arg) {
  (void)arg;
  vx_printf("Hello\n");
}

__kernel void kernel_tail(kernel_arg_t* arg) {
  (void)arg;
  vx_printf("World!\n");
}

__kernel void kernel_parent(kernel_arg_t* __UNIFORM__ arg) {
  if (!arg->print_words)
    return;
  g_launch_arg = *arg;
  uint64_t child_arg = reinterpret_cast<uint64_t>(&g_launch_arg);
  launch_tail_grid(kernel_entry_pc(kernel_child), child_arg);
  launch_tail_grid(kernel_entry_pc(kernel_tail), child_arg);
}

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  kernel_parent(arg);
}
