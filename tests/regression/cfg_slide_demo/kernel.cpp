// Tiny kernel for slide-quality CFG plots (nested if only, no loops).
#include <vx_spawn.h>
#include <stdint.h>
#include "common.h"

static __attribute__((noinline)) void path_high(int32_t* v) { *v += 6; }
static __attribute__((noinline)) void path_mid(int32_t* v) { *v += 4; }
static __attribute__((noinline)) void path_low(int32_t* v) { *v += 3; }

void kernel_body(kernel_arg_t* __UNIFORM__ arg) {
  int32_t* src_ptr = (int32_t*)arg->src_addr;
  int32_t* dst_ptr = (int32_t*)arg->dst_addr;
  uint32_t task_id = blockIdx.x;
  int32_t value = src_ptr[task_id];

  if (task_id > 1) {
    path_high(&value);
  } else if (task_id > 0) {
    path_mid(&value);
  } else {
    path_low(&value);
  }

  dst_ptr[task_id] = value;
}

int main() {
  kernel_arg_t* arg = (kernel_arg_t*)csr_read(VX_CSR_MSCRATCH);
  return vx_spawn_threads(1, &arg->num_points, nullptr, (vx_kernel_func_cb)kernel_body, arg);
}
