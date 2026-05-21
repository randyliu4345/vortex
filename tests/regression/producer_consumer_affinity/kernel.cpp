#include <vx_spawn2.h>
#include <vx_launch.h>
#include <vx_intrinsics.h>
#include <stdint.h>
#include "common.h"

static void consumer_child_kernel(child_arg_t* a) {
  const uint32_t tid = threadIdx.x;
  const uint32_t bdim = blockDim.x;
  auto* data = reinterpret_cast<float*>(static_cast<uintptr_t>(a->data_addr));
  auto* out = reinterpret_cast<float*>(static_cast<uintptr_t>(a->verify_addr));

  for (uint32_t i = a->elem_begin + tid; i < a->elem_end; i += bdim) {
    out[i] = data[i] * a->scale;
  }
}

static uint32_t page_elems(const master_arg_t* m) {
  return (1u << m->coarse_page_log2) / static_cast<uint32_t>(sizeof(float));
}

static uint32_t active_chunks(const master_arg_t* m, uint32_t page_elems_v) {
  const uint32_t num_cores = m->num_cores ? m->num_cores : 1u;
  uint32_t active = 0;
  for (uint32_t c = 0; c < num_cores; ++c) {
    if (c * page_elems_v >= m->total_elems)
      break;
    ++active;
  }
  return active;
}

static void producer_kernel(master_arg_t* m) {
  if (threadIdx.x != 0)
    return;

  const uint32_t num_cores = m->num_cores ? m->num_cores : 1u;
  const uint32_t page_elems_v = page_elems(m);
  auto* data = reinterpret_cast<float*>(static_cast<uintptr_t>(m->data_addr));
  uint32_t active = 0;
  // Single producer fills all per-core coarse pages first.
  for (uint32_t c = 0; c < num_cores; ++c) {
    const uint32_t begin = c * page_elems_v;
    uint32_t end = begin + page_elems_v;
    if (begin >= m->total_elems)
      break;
    if (end > m->total_elems)
      end = m->total_elems;
    for (uint32_t i = begin; i < end; ++i) {
      data[i] = static_cast<float>(i - begin);
    }
    ++active;
  }
  m->active_chunks = active;
  m->page_elems = page_elems_v;
  vx_fence();
}

static void dispatch_kernel(master_arg_t* m) {
  if (threadIdx.x != 0)
    return;

  const uint32_t num_cores = m->num_cores ? m->num_cores : 1u;
  const uint32_t page_elems_v = page_elems(m);
  const uint32_t active = active_chunks(m, page_elems_v);
  auto* pool = reinterpret_cast<child_arg_t*>(
      static_cast<uintptr_t>(m->child_pool_addr));

  // Launch per-core consumers (baseline:any vs affinity:pinned).
  uint32_t grid[3] = {1u, 1u, 1u};
  uint32_t cons_block[3] = {m->consumer_block_x, 1u, 1u};
  for (uint32_t c = 0; c < active; ++c) {
    const uint32_t begin = c * page_elems_v;
    uint32_t end = begin + page_elems_v;
    if (end > m->total_elems)
      end = m->total_elems;
    pool[c].entry_kind = PC_ENTRY_CONSUMER_CHILD;
    pool[c].core_id = c;
    pool[c].elem_begin = begin;
    pool[c].elem_end = end;
    pool[c].data_addr = m->data_addr;
    pool[c].verify_addr = m->verify_addr;
    pool[c].scale = 2.0f;
    vx_fence();

    vx_kmu_launch_desc_t desc;
    vx_launch_desc_init_affine(&desc,
                                m->kernel_pc,
                                reinterpret_cast<uint64_t>(&pool[c]),
                                grid,
                                cons_block,
                                0,
                                m->consumer_affinity ? (c % num_cores)
                                                     : VORTEX_AFFINITY_ANY);
    vx_kernel_launch(&desc);
  }

  m->active_chunks = active;
  m->page_elems = page_elems_v;
  vx_fence();
}

__kernel void kernel_main(void* __UNIFORM__ raw) {
  const uint32_t kind = *reinterpret_cast<const uint32_t*>(raw);
  if (kind == PC_ENTRY_PRODUCER) {
    producer_kernel(reinterpret_cast<master_arg_t*>(raw));
  } else if (kind == PC_ENTRY_DISPATCH) {
    dispatch_kernel(reinterpret_cast<master_arg_t*>(raw));
  } else if (kind == PC_ENTRY_CONSUMER_CHILD) {
    consumer_child_kernel(reinterpret_cast<child_arg_t*>(raw));
  }
}
