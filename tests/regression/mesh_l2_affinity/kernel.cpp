#include <vx_spawn2.h>
#include <vx_launch.h>
#include <vx_intrinsics.h>
#include <stdint.h>
#include "common.h"
#include "../../../mesh_l2_data.h"

static float mesh_l2_load_f(const void* base, uint32_t elem) {
  const auto* bytes = reinterpret_cast<const uint8_t*>(base);
  const auto* p =
      reinterpret_cast<const float*>(bytes + mesh_l2_elem_byte_offset(elem));
  return *p;
}

static void mesh_l2_store_f(void* base, uint32_t elem, float value) {
  auto* bytes = reinterpret_cast<uint8_t*>(base);
  auto* p = reinterpret_cast<float*>(bytes + mesh_l2_elem_byte_offset(elem));
  *p = value;
}

static void mesh_child_kernel(kernel_arg_t* arg) {
  const uint32_t lid  = threadIdx.x;
  const uint32_t bdim = blockDim.x;

  const void* src    = reinterpret_cast<const void*>(arg->src_addr);
  void* dst          = reinterpret_cast<void*>(arg->dst_addr);
  const void* remote = reinterpret_cast<const void*>(arg->remote_src_addr);
  const uint32_t n = arg->num_floats;

  for (uint32_t i = lid; i < n; i += bdim) {
    if ((i % MESH_L2_REMOTE_ACCESS_STRIDE) == 0u) {
      volatile float touch = mesh_l2_load_f(remote, i % 64u);
      (void)touch;
    }
    mesh_l2_store_f(dst, i, mesh_l2_load_f(src, i));
  }

  if (lid == 0) {
    auto* tag = reinterpret_cast<uint32_t*>(
        reinterpret_cast<uint8_t*>(dst) + mesh_l2_elem_byte_offset(1u));
    *tag = static_cast<uint32_t>(vx_core_id());
  }
}

static void master_dispatch_kernel(master_arg_t* m) {
  const uint32_t lid = threadIdx.x;
  if (lid != 0)
    return;

  auto* src_addrs    = reinterpret_cast<const uint64_t*>(m->partition_src_addr_addr);
  auto* dst_addrs    = reinterpret_cast<const uint64_t*>(m->partition_dst_addr_addr);
  auto* remote_addrs = reinterpret_cast<const uint64_t*>(m->partition_remote_src_addr_addr);
  auto* pool         = reinterpret_cast<kernel_arg_t*>(m->child_arg_pool_addr);

  const uint32_t ncores = m->num_cores ? m->num_cores : 1u;
  uint32_t grid[3]  = {1u, 1u, 1u};
  uint32_t block[3] = {m->child_block_x, 1u, 1u};

  for (uint32_t rep = 0; rep < 4u; ++rep) {
    for (uint32_t i = 0; i < m->num_partitions; ++i) {
      pool[i].entry_kind      = MESH_ENTRY_CHILD;
      pool[i].partition_id    = i;
      pool[i].src_addr        = src_addrs[i];
      pool[i].dst_addr        = dst_addrs[i];
      pool[i].remote_src_addr = remote_addrs[i];
      pool[i].num_floats      = MESH_L2_FLOATS_PER_PARTITION;
      vx_fence();

      const uint32_t home = mesh_l2_partition_home_core[i] % ncores;
      uint32_t aff;
      switch (m->affinity_mode) {
      case MESH_AFFINITY_ANY:
        aff = VORTEX_AFFINITY_ANY;
        break;
      case MESH_AFFINITY_REMOTE:
        aff = (home + 2u) % ncores;
        break;
      default:
        aff = home;
        break;
      }

      vx_kmu_launch_desc_t desc;
      vx_launch_desc_init(&desc,
                                  m->kernel_pc,
                                  reinterpret_cast<uint64_t>(&pool[i]),
                                  grid,
                                  block,
                                  0,
                                  aff);
      vx_kernel_launch(&desc);
    }
  }

  vx_fence();
}

__kernel void kernel_main(void* __UNIFORM__ raw) {
  const uint32_t kind = *reinterpret_cast<const uint32_t*>(raw);
  if (kind == MESH_ENTRY_MASTER) {
    master_dispatch_kernel(reinterpret_cast<master_arg_t*>(raw));
  } else if (kind == MESH_ENTRY_CHILD) {
    mesh_child_kernel(reinterpret_cast<kernel_arg_t*>(raw));
  }
}
