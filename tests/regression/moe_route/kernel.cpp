#include <vx_spawn2.h>
#include <vx_launch.h>
#include <vx_intrinsics.h>
#include "common.h"

static void count_experts(const kernel_arg_t* arg) {
  auto topk = reinterpret_cast<const int32_t*>(arg->topk_addr);
  auto counts = reinterpret_cast<int32_t*>(arg->expert_counts_addr);
  uint32_t T = arg->T;
  uint32_t t = blockIdx.x * blockDim.x + threadIdx.x;
  if (t < T) {
    int32_t e = topk[t];
    __sync_fetch_and_add(&counts[e], 1);
  }
}

static void scatter_token_ids(const kernel_arg_t* arg) {
  auto topk = reinterpret_cast<const int32_t*>(arg->topk_addr);
  auto offsets = reinterpret_cast<const int32_t*>(arg->expert_offsets_addr);
  auto cursor = reinterpret_cast<int32_t*>(arg->expert_cursor_addr);
  auto sorted = reinterpret_cast<int32_t*>(arg->sorted_token_ids_addr);
  uint32_t T = arg->T;
  uint32_t t = blockIdx.x * blockDim.x + threadIdx.x;
  if (t < T) {
    int32_t e = topk[t];
    int32_t slot = __sync_fetch_and_add(&cursor[e], 1);
    sorted[offsets[e] + slot] = (int32_t)t;
  }
}

// X[token,h] -> X_perm[xperm_off_p + h]; host sets xperm_off_p and core pin.
static void permute_rows(const kernel_arg_t* arg) {
  auto X = reinterpret_cast<const float*>(arg->X_addr);
  auto sorted = reinterpret_cast<const int32_t*>(arg->sorted_token_ids_addr);
  auto X_perm = reinterpret_cast<float*>(arg->X_perm_addr);
  uint32_t T = arg->T;
  uint32_t H = arg->H;
  uint32_t p = blockIdx.y + arg->row_offset;
  uint32_t dst_base = arg->xperm_off_p;
  uint32_t h = blockIdx.x * blockDim.x + threadIdx.x;
  if (p < T && h < H) {
    int32_t token = sorted[p];
    X_perm[dst_base + h] = X[(uint32_t)token * H + h];
  }
}

// X_perm[xperm_off_p + h] -> Y[token,h]
static void unpermute_rows(const kernel_arg_t* arg) {
  auto Y_perm = reinterpret_cast<const float*>(arg->X_perm_addr);
  auto sorted = reinterpret_cast<const int32_t*>(arg->sorted_token_ids_addr);
  auto Y = reinterpret_cast<float*>(arg->Y_addr);
  uint32_t T = arg->T;
  uint32_t H = arg->H;
  uint32_t p = blockIdx.y + arg->row_offset;
  uint32_t src_base = arg->xperm_off_p;
  uint32_t h = blockIdx.x * blockDim.x + threadIdx.x;
  if (p < T && h < H) {
    int32_t token = sorted[p];
    Y[(uint32_t)token * H + h] = Y_perm[src_base + h];
  }
}

static uint64_t child_pool_slot_addr(uint64_t pool_addr,
                                     uint32_t p,
                                     uint32_t target_bank,
                                     uint32_t pool_page_bytes,
                                     uint32_t pool_base_bank,
                                     uint32_t num_banks) {
  const uint32_t compact = (uint32_t)sizeof(kernel_arg_t);
  if (!pool_page_bytes || pool_page_bytes <= compact) {
    return pool_addr + (uint64_t)p * compact;
  }
  const uint32_t delta =
      (target_bank + num_banks - pool_base_bank) % num_banks;
  return pool_addr +
         (uint64_t)(p * num_banks + delta) * (uint64_t)pool_page_bytes;
}

static uint64_t master_pool_slot_addr(const master_arg_t* m, uint32_t p,
                                      uint32_t target_bank) {
  return child_pool_slot_addr(m->child_pool_addr, p, target_bank,
                              m->pool_page_bytes, m->pool_base_bank,
                              m->num_dispatchers ? m->num_dispatchers : 1u);
}

// Device-side master: each master CTA (blockIdx.x in [0, num_dispatchers))
// enqueues child grids for rows p = blockIdx.x, blockIdx.x + stride, ...
static void master_dispatch_burst(const master_arg_t* m) {
  if (threadIdx.x != 0)
    return;
  auto* aff_tab = reinterpret_cast<const uint32_t*>(m->affinity_addr);
  const uint32_t T = m->T;
  const uint32_t child_op = m->child_op;
  const uint64_t kernel_pc = m->kernel_pc;
  const uint32_t stride = m->num_dispatchers ? m->num_dispatchers : 1u;
  const uint32_t begin = blockIdx.x;
  uint32_t grid[3]  = {m->grid_x, 1u, 1u};
  uint32_t block[3] = {m->NT, 1u, 1u};
  for (uint32_t p = begin; p < T; p += stride) {
    const uint32_t aff = aff_tab[p];
    const uint64_t slot_addr = master_pool_slot_addr(m, p, aff);
    auto* entry = reinterpret_cast<kernel_arg_t*>(slot_addr);
    entry->op = child_op;
    vx_kmu_launch_desc_t desc;
    vx_launch_desc_init(&desc, kernel_pc, slot_addr, grid, block, 0, aff);
    vx_kernel_launch(&desc);
  }
  vx_fence();
}

// Expert FFN: one grid per core; fuses W tile load with in-place activation stub.
static void expert_ffn_core_rows(const expert_ffn_core_arg_t* wc) {
  const uint32_t H = wc->H;
  const uint32_t h = blockIdx.x * blockDim.x + threadIdx.x;
  if (h >= H)
    return;
  const uint32_t compact = (uint32_t)sizeof(kernel_arg_t);
  const uint32_t num_banks = wc->num_banks ? wc->num_banks : 1u;
  const uint32_t page_bytes =
      wc->pool_page_bytes ? wc->pool_page_bytes : compact;

  if (wc->by_expert_bin) {
    for (uint32_t e = 0; e < wc->E; ++e) {
      if (e != wc->core_id)
        continue;
      const int32_t nrows = wc->expert_counts[e];
      const int32_t base_p = wc->expert_offsets[e];
      for (int32_t slot = 0; slot < nrows; ++slot) {
        const uint32_t p = (uint32_t)(base_p + slot);
        const uint64_t slot_addr = child_pool_slot_addr(
            wc->child_pool_addr, p, e, page_bytes, wc->pool_base_bank,
            num_banks);
        const kernel_arg_t* arg =
            reinterpret_cast<const kernel_arg_t*>(slot_addr);
        auto W = reinterpret_cast<const float*>(arg->W_addr);
        auto X_perm = reinterpret_cast<float*>(arg->X_perm_addr);
        const uint32_t base = arg->xperm_off_p;
        const uint32_t w_base = arg->w_off_p;
        const uint32_t w_row_stride = arg->w_row_stride;
        float wsum = 0.f;
        for (uint32_t k = 0; k < H; ++k)
          wsum += W[w_base + k * w_row_stride + h];
        const float v = X_perm[base + h];
        X_perm[base + h] = v + wsum * 0.f;
      }
    }
    return;
  }

  for (uint32_t p = wc->core_id; p < wc->T; p += num_banks) {
    const uint64_t slot_addr = child_pool_slot_addr(
        wc->child_pool_addr, p, wc->core_id, compact, 0u, num_banks);
    const kernel_arg_t* arg = reinterpret_cast<const kernel_arg_t*>(slot_addr);
    auto W = reinterpret_cast<const float*>(arg->W_addr);
    auto X_perm = reinterpret_cast<float*>(arg->X_perm_addr);
    const uint32_t base = arg->xperm_off_p;
    const uint32_t w_base = arg->w_off_p;
    const uint32_t w_row_stride = arg->w_row_stride;
    float wsum = 0.f;
    for (uint32_t k = 0; k < H; ++k)
      wsum += W[w_base + k * w_row_stride + h];
    const float v = X_perm[base + h];
    X_perm[base + h] = v + wsum * 0.f;
  }
}

__kernel void kernel_main(void* __UNIFORM__ raw) {
  const uint32_t op = *reinterpret_cast<const uint32_t*>(raw);
  switch (op) {
  case MOE_OP_COUNT:
    count_experts(reinterpret_cast<const kernel_arg_t*>(raw));
    break;
  case MOE_OP_SCATTER:
    scatter_token_ids(reinterpret_cast<const kernel_arg_t*>(raw));
    break;
  case MOE_OP_PERMUTE:
    permute_rows(reinterpret_cast<const kernel_arg_t*>(raw));
    break;
  case MOE_OP_UNPERMUTE:
    unpermute_rows(reinterpret_cast<const kernel_arg_t*>(raw));
    break;
  case MOE_OP_MASTER:
    master_dispatch_burst(reinterpret_cast<const master_arg_t*>(raw));
    break;
  case MOE_OP_EXPERT_FFN:
    expert_ffn_core_rows(reinterpret_cast<const expert_ffn_core_arg_t*>(raw));
    break;
  default:
    break;
  }
}
