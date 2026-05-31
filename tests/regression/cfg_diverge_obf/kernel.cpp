// Obfuscated variant of cfg_diverge_lab (opaque predicates preserve semantics).
#include <vx_spawn.h>
#include <stdint.h>
#include "common.h"

static inline int opaque_true(uint32_t x) {
  return ((x * 2u) == (x << 1u)) ? 1 : 0;
}

void kernel_body(kernel_arg_t* __UNIFORM__ arg) {
  int32_t* src_ptr = (int32_t*)arg->src_addr;
  int32_t* dst_ptr = (int32_t*)arg->dst_addr;
  uint32_t task_id = blockIdx.x;
  int32_t value = src_ptr[task_id];

  if (opaque_true(task_id) && task_id > 1) {
    if (opaque_true(task_id) && task_id > 2) {
      value += 6;
    } else {
      value += 5;
    }
  } else if (opaque_true(task_id)) {
    if (task_id > 0) {
      value += 4;
    } else {
      value += 3;
    }
  }

  for (int i = 0, n = (int)task_id; opaque_true((uint32_t)i) && i < n; ++i) {
    value += src_ptr[i];
  }

  if (opaque_true(task_id)) {
    switch (task_id & 3u) {
    case 0:
      value += 1;
      break;
    case 1:
      value -= 1;
      break;
    case 2:
      value *= 2;
      break;
    default:
      value += 7;
      break;
    }
  }

  dst_ptr[task_id] = value;
}

int main() {
  kernel_arg_t* arg = (kernel_arg_t*)csr_read(VX_CSR_MSCRATCH);
  return vx_spawn_threads(1, &arg->num_points, nullptr, (vx_kernel_func_cb)kernel_body, arg);
}
