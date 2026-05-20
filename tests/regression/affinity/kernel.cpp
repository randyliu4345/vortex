#include <vx_spawn2.h>
#include <vx_intrinsics.h>
#include "common.h"

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  auto* out = reinterpret_cast<uint32_t*>(arg->out_addr);
  if (threadIdx.x == 0 && threadIdx.y == 0 && threadIdx.z == 0) {
    out[0] = static_cast<uint32_t>(vx_core_id());
  }
}
