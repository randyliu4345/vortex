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
 

#ifndef __VX_LAUNCH_H__
#define __VX_LAUNCH_H__

#include <stdint.h>
#include <VX_config.h>
#include <vx_intrinsics.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VORTEX_AFFINITY_ANY 0xFFFFFFFFu
#define VX_LAUNCH_FLAG_TAIL  (1u << 0)

typedef struct __attribute__((packed)) {
  uint64_t pc;
  uint64_t arg;
  uint32_t grid_dim[3];
  uint32_t block_dim[3];
  uint32_t block_size;
  uint32_t warp_step[3];
  uint32_t lmem_size;
  uint32_t core_affinity;
  uint32_t flags;
} vx_kmu_launch_desc_t;

typedef struct {
  vx_kmu_launch_desc_t desc;
} vx_kmu_launch_entry_t;

typedef struct {
  volatile uint32_t head;
  volatile uint32_t tail;
  vx_kmu_launch_entry_t entries[LAUNCH_QUEUE_SIZE];
} vx_kmu_launch_queue_t;

static inline vx_kmu_launch_queue_t* vx_launch_queue() {
  return (vx_kmu_launch_queue_t*)(uintptr_t)LAUNCH_QUEUE_BASE;
}

static inline void vx_kmu_launch_signal() {
  __asm__ volatile (".insn r %0, 1, 0, x0, x0, x0" :: "i"(RISCV_CUSTOM2) : "memory");
}

static inline void vx_launch_desc_init(vx_kmu_launch_desc_t* desc,
                                       uint64_t pc,
                                       uint64_t arg,
                                       const uint32_t grid_dim[3],
                                       const uint32_t block_dim[3],
                                       uint32_t lmem_size,
                                       uint32_t core_affinity = VORTEX_AFFINITY_ANY) {
  uint32_t threads_per_warp = (uint32_t)vx_num_threads();
  uint32_t block_size = 1;
  for (int i = 0; i < 3; ++i) {
    desc->grid_dim[i]  = grid_dim[i];
    desc->block_dim[i] = block_dim[i];
    block_size *= block_dim[i];
  }
  desc->pc         = pc;
  desc->arg        = arg;
  desc->block_size = block_size;
  desc->warp_step[0] = threads_per_warp % block_dim[0];
  desc->warp_step[1] = (threads_per_warp / block_dim[0]) % block_dim[1];
  desc->warp_step[2] = (threads_per_warp / (block_dim[0] * block_dim[1])) % block_dim[2];
  desc->lmem_size      = lmem_size;
  desc->core_affinity  = core_affinity;
  desc->flags          = 0;
}

static inline void vx_launch_desc_init_tail(vx_kmu_launch_desc_t* desc,
                                            uint64_t pc,
                                            uint64_t arg,
                                            const uint32_t grid_dim[3],
                                            const uint32_t block_dim[3],
                                            uint32_t lmem_size) {
  vx_launch_desc_init(desc, pc, arg, grid_dim, block_dim, lmem_size);
  desc->flags |= VX_LAUNCH_FLAG_TAIL;
}

static inline void vx_kernel_launch(const vx_kmu_launch_desc_t* desc) {
  vx_kmu_launch_queue_t* queue = vx_launch_queue();

  while (1) {
    uint32_t tail = queue->tail;
    uint32_t head = queue->head;
    if ((tail - head) >= LAUNCH_QUEUE_SIZE) {
      __sync_synchronize();
      continue;
    }
    if (__sync_bool_compare_and_swap(&queue->tail, tail, tail + 1)) {
      uint32_t idx = tail % LAUNCH_QUEUE_SIZE;
      queue->entries[idx].desc = *desc;
      __sync_synchronize();
      vx_fence();
      vx_kmu_launch_signal();
      break;
    }
  }
}

static inline void vx_kernel_launch_tail(const vx_kmu_launch_desc_t* desc) {
  vx_kmu_launch_desc_t tail_desc = *desc;
  tail_desc.flags |= VX_LAUNCH_FLAG_TAIL;
  vx_kernel_launch(&tail_desc);
}

static inline void vx_kernel_launch_affine(uint64_t pc,
                                           uint64_t arg,
                                           const uint32_t grid_dim[3],
                                           const uint32_t block_dim[3],
                                           uint32_t lmem_size,
                                           uint32_t core_affinity) {
  vx_kmu_launch_desc_t desc;
  vx_launch_desc_init(&desc, pc, arg, grid_dim, block_dim, lmem_size, core_affinity);
  vx_kernel_launch(&desc);
}

#ifdef __cplusplus
}
#endif

#endif // __VX_LAUNCH_H__
