#include <vx_spawn2.h>
#include <vx_launch.h>
#include <vx_print.h>
#include "common.h"

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  // Optional proof-of-life prints for default 2-stage chain.
  if (arg->print_words) {
    if (arg->launches_remaining == 1) {
      vx_printf("Hello\n");
    } else if (arg->launches_remaining == 0) {
      vx_printf("World!\n");
    }
  }

  if (arg->launches_remaining == 0)
    return;

  auto* child_arg = reinterpret_cast<kernel_arg_t*>(arg->child_arg_addr);
  child_arg->role = DL_ROLE_CHILD;
  child_arg->launches_remaining = arg->launches_remaining - 1;
  child_arg->print_words = arg->print_words;
  child_arg->child_pc = arg->child_pc;
  child_arg->child_arg_addr = arg->child_arg_addr;

  uint32_t grid1[3]  = {1u, 1u, 1u};
  uint32_t block1[3] = {1u, 1u, 1u};
  vx_kmu_launch_desc_t desc;
  vx_launch_desc_init(&desc,
                      arg->child_pc,
                      reinterpret_cast<uint64_t>(child_arg),
                      grid1,
                      block1,
                      /*lmem_size=*/0);
  vx_kernel_launch(&desc);
}
