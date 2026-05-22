// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __VX_SPAWN_H__
#define __VX_SPAWN_H__

#include <vx_intrinsics.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union {
  struct {
    uint32_t x;
    uint32_t y;
    uint32_t z;
  };
  uint32_t m[3];
} dim3_t;

extern __thread dim3_t blockIdx;
extern __thread dim3_t threadIdx;
extern dim3_t gridDim;
extern dim3_t blockDim;

extern __thread uint32_t __local_group_id;
extern uint32_t __warps_per_group;

typedef void (*vx_kernel_func_cb)(void *arg);

typedef void (*vx_serial_cb)(void *arg);

#define __local_mem(size) \
  (void*)((int8_t*)csr_read(VX_CSR_LOCAL_MEM_BASE) + __local_group_id * size)

#define __syncthreads() \
  vx_barrier(__local_group_id, __warps_per_group)

// launch a kernel function with a grid of blocks and block of threads
int vx_spawn_threads(uint32_t dimension,
                     const uint32_t* grid_dim,
                     const uint32_t* block_dim,
                     vx_kernel_func_cb kernel_func,
                     const void* arg);

// function call serialization
void vx_serial(vx_serial_cb callback, const void * arg);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <vx_launch.h>

// Device-side dynamic launch helper: build a vx_kmu_launch_desc_t on the
// stack pinned to a single core and submit it to the KMU. Equivalent to
// the unaffine path when `core_id` == VORTEX_AFFINITY_ANY.
//
//   pc        kernel entry PC (typically obtained from the parent kernel arg)
//   arg       kernel argument pointer (passed via MSCRATCH / __UNIFORM__ arg)
//   grid_dim  3-element grid dimensions array
//   block_dim 3-element block dimensions array
//   lmem_size local-memory bytes per block
//   core_id   global core_id to pin to, or VORTEX_AFFINITY_ANY for default
//
// Omit core_id (or pass VORTEX_AFFINITY_ANY) for load-balanced launch.
static inline void vx_spawn_affine(uint64_t pc,
                                   uint64_t arg,
                                   const uint32_t grid_dim[3],
                                   const uint32_t block_dim[3],
                                   uint32_t lmem_size,
                                   uint32_t core_id) {
  vx_kernel_launch_affine(pc, arg, grid_dim, block_dim, lmem_size, core_id);
}
#endif

#endif // __VX_SPAWN_H__