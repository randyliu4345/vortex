// Device-side master dispatch: one host processor.run() launches a tiny master
// CTA that issues 64 KMU child grids (SpMM mini-partitions) via VX_CSR_KMU_LAUNCH.
// Descriptor layout and core_affinity follow kernel/include/vx_launch.h.

#include <vx_spawn2.h>
#include <vx_launch.h>
#include <vx_intrinsics.h>
#include "common.h"
#include "gnn_layout.h"

static void spmm_child_kernel(kernel_arg_t* arg) {
  const uint32_t lid  = threadIdx.x;
  const uint32_t bdim = blockDim.x;

  auto row_ptr      = reinterpret_cast<const uint32_t*>(arg->row_ptr_addr);
  auto col_ind      = reinterpret_cast<const uint32_t*>(arg->col_ind_addr);
  auto features_in  = reinterpret_cast<const float*>(arg->features_in_addr);
  auto features_out = reinterpret_cast<float*>(arg->features_out_addr);

  const uint32_t start = arg->start_node;
  const uint32_t end   = arg->end_node;
  constexpr uint32_t F = GNN_SPMM_FEATURE_DIM;

  for (uint32_t v = start + lid; v < end; v += bdim) {
    float acc[F];
    #pragma GCC unroll 16
    for (uint32_t f = 0; f < F; ++f) acc[f] = 0.0f;

    const uint32_t s = row_ptr[v];
    const uint32_t e = row_ptr[v + 1];
    for (uint32_t k = s; k < e; ++k) {
      const uint32_t n = col_ind[k];
      const auto* fin = reinterpret_cast<const uint8_t*>(features_in);
      const float* nf = reinterpret_cast<const float*>(
          fin + gnn_feature_byte_offset(n));
      #pragma GCC unroll 16
      for (uint32_t f = 0; f < F; ++f) {
        acc[f] += nf[f];
      }
    }

    auto* fout = reinterpret_cast<uint8_t*>(features_out);
    float* out = reinterpret_cast<float*>(fout + gnn_feature_byte_offset(v));
    #pragma GCC unroll 16
    for (uint32_t f = 0; f < F; ++f) {
      out[f] = acc[f];
    }
  }
}

static void master_dispatch_kernel(master_arg_t* m) {
  const uint32_t lid = threadIdx.x;
  if (lid != 0)
    return;

  auto* starts = reinterpret_cast<const uint32_t*>(m->mini_partition_start_addr);
  auto* ends   = reinterpret_cast<const uint32_t*>(m->mini_partition_end_addr);
  auto* affs   = reinterpret_cast<const uint32_t*>(m->mini_partition_affinity_addr);
  auto* pool   = reinterpret_cast<kernel_arg_t*>(m->child_arg_pool_addr);

  const uint32_t ncores = m->num_cores ? m->num_cores : 1u;
  uint32_t grid[3]  = {1u, 1u, 1u};
  uint32_t block[3] = {m->child_block_x, 1u, 1u};

  for (uint32_t i = 0; i < m->num_minis; ++i) {
    pool[i].start_node = starts[i];
    pool[i].end_node   = ends[i];
    vx_fence();

    const uint32_t home = affs[i] % ncores;
    uint32_t aff;
    switch (m->affinity_mode) {
    case GNN_AFFINITY_ANY:
      aff = VORTEX_AFFINITY_ANY;
      break;
    case GNN_AFFINITY_REMOTE:
      aff = (home + 2u) % ncores;
      break;
    default:
      aff = home;
      break;
    }

    vx_kmu_launch_desc_t desc;
    vx_launch_desc_init_affine(&desc,
                                m->kernel_pc,
                                reinterpret_cast<uint64_t>(&pool[i]),
                                grid,
                                block,
                                /*lmem_size=*/0,
                                aff);
    vx_kernel_launch(&desc);
  }

  // Global fence: all prior device-side launches + memory are ordered before the
  // master CTA completes. SimX still drains all in-flight child warps before the
  // host vx_ready_wait() returns from this processor.run().
  vx_fence();
}

__kernel void kernel_main(void* __UNIFORM__ raw) {
  const uint32_t kind = *reinterpret_cast<const uint32_t*>(raw);
  if (kind == GNN_ENTRY_MASTER) {
    master_dispatch_kernel(reinterpret_cast<master_arg_t*>(raw));
    return;
  }
  if (kind == GNN_ENTRY_SPMM) {
    spmm_child_kernel(reinterpret_cast<kernel_arg_t*>(raw));
    return;
  }
}
