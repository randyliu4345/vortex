#include <algorithm>
#include <iostream>
#include <unistd.h>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>
#include <random>
#include <cmath>
#include <vortex.h>
#include <VX_config.h>
#include <VX_types.h>
#include "common.h"

#define FLOAT_ULP 6

#define RT_CHECK(_expr)                                         \
  do {                                                          \
    int _ret = _expr;                                           \
    if (0 == _ret)                                              \
      break;                                                    \
    printf("Error: '%s' returned %d!\n", #_expr, (int)_ret);    \
    cleanup();                                                  \
    exit(-1);                                                   \
  } while (false)

const char* kernel_file = "kernel.vxbin";

uint32_t T = 256;
uint32_t H = 64;
uint32_t E = 8;

vx_device_h device = nullptr;
vx_buffer_h X_buffer = nullptr;
vx_buffer_h X_perm_buffer = nullptr;
vx_buffer_h Y_buffer = nullptr;
vx_buffer_h topk_buffer = nullptr;
vx_buffer_h counts_buffer = nullptr;
vx_buffer_h offsets_buffer = nullptr;
vx_buffer_h cursor_buffer = nullptr;
vx_buffer_h sorted_buffer = nullptr;
vx_buffer_h W_buffer = nullptr;
vx_buffer_h krnl_buffer = nullptr;
vx_buffer_h args_buffer = nullptr;
vx_buffer_h child_pool_buffer = nullptr;
vx_buffer_h master_arg_buffer = nullptr;
vx_buffer_h aff_permute_buffer = nullptr;
vx_buffer_h aff_expert_ffn_buffer = nullptr;
vx_buffer_h aff_unpermute_buffer = nullptr;
kernel_arg_t kernel_arg = {};

std::vector<uint32_t> g_xperm_off;
std::vector<uint32_t> g_w_off;

static std::vector<int32_t> exclusive_scan(const std::vector<int32_t>& counts) {
  std::vector<int32_t> offs(counts.size(), 0);
  int32_t running = 0;
  for (size_t i = 0; i < counts.size(); ++i) {
    offs[i] = running;
    running += counts[i];
  }
  return offs;
}

static void mesh_stats_tag(const char* tag) {
  if (std::getenv("VORTEX_MESH_STATS") == nullptr)
    return;
  if (tag != nullptr && tag[0] != '\0')
    setenv("VORTEX_MESH_TAG", tag, 1);
  else
    unsetenv("VORTEX_MESH_TAG");
}

static uint64_t query_l2_bank_stalls(bool l2_en) {
  if (!l2_en)
    return 0;
  uint64_t stalls = 0;
  if (vx_mpm_query(device, VX_DCR_MPM_CLASS_MEM,
                   VX_CSR_MPM_L2CACHE_BANK_ST, 0, &stalls) != 0)
    return 0;
  return stalls;
}

static void log_l2_bank_stalls(const char* tag, bool l2_en) {
  if (std::getenv("VORTEX_MESH_STATS") == nullptr)
    return;
  std::cerr << "L2_BANK_STALLS tag=" << tag
            << " stalls=" << query_l2_bank_stalls(l2_en) << std::endl;
}

static void show_usage() {
  std::cout << "Vortex MoE routing microbenchmark (permute/unpermute).\n";
  std::cout << "Usage: [-k kernel] [-t tokens] [-d hidden] [-e experts] [-h help]\n";
}

static void parse_args(int argc, char** argv) {
  int c;
  while ((c = getopt(argc, argv, "k:t:d:e:h")) != -1) {
    switch (c) {
    case 'k':
      kernel_file = optarg;
      break;
    case 't':
      T = (uint32_t)atoi(optarg);
      break;
    case 'd':
      H = (uint32_t)atoi(optarg);
      break;
    case 'e':
      E = (uint32_t)atoi(optarg);
      break;
    case 'h':
      show_usage();
      exit(0);
    default:
      show_usage();
      exit(-1);
    }
  }
  if (T == 0 || H == 0 || E == 0) {
    std::cerr << "T, H, and E must be positive\n";
    exit(-1);
  }
}

void cleanup() {
  if (!device)
    return;
  vx_mem_free(X_buffer);
  vx_mem_free(X_perm_buffer);
  vx_mem_free(Y_buffer);
  vx_mem_free(topk_buffer);
  vx_mem_free(counts_buffer);
  vx_mem_free(offsets_buffer);
  vx_mem_free(cursor_buffer);
  vx_mem_free(sorted_buffer);
  vx_mem_free(W_buffer);
  vx_mem_free(krnl_buffer);
  vx_mem_free(args_buffer);
  if (child_pool_buffer) vx_mem_free(child_pool_buffer);
  if (master_arg_buffer) vx_mem_free(master_arg_buffer);
  if (aff_permute_buffer) vx_mem_free(aff_permute_buffer);
  if (aff_expert_ffn_buffer) vx_mem_free(aff_expert_ffn_buffer);
  if (aff_unpermute_buffer) vx_mem_free(aff_unpermute_buffer);
  vx_dev_close(device);
  device = nullptr;
}

static uint32_t get_l2_bank_for_byte_addr(uint64_t byte_addr,
                                          uint32_t page_log2,
                                          uint32_t num_banks) {
  const uint64_t normalized = (byte_addr >= USER_BASE_ADDR)
                                  ? (byte_addr - USER_BASE_ADDR)
                                  : byte_addr;
  return (uint32_t)((normalized >> page_log2) % (uint64_t)num_banks);
}

static uint32_t get_l2_bank_for_address_coarse(uint64_t base_addr,
                                               uint32_t token_idx,
                                               uint32_t H,
                                               uint32_t page_log2,
                                               uint32_t num_banks) {
  const uint64_t row_byte_addr =
      base_addr + (uint64_t)token_idx * (uint64_t)H * sizeof(float);
  return get_l2_bank_for_byte_addr(row_byte_addr, page_log2, num_banks);
}

static uint32_t encode_l2_bank_policy(uint8_t page_log2) {
  return 1u | ((static_cast<uint32_t>(page_log2) & 0x7fu) << 1);
}

static uint32_t ceil_log2_u64(uint64_t v) {
  if (v <= 1)
    return 0;
  uint32_t log2 = 0;
  while (((uint64_t)1u << log2) < v)
    ++log2;
  return log2;
}

static uint32_t choose_coarse_page_log2(uint32_t H, uint32_t cache_line_size) {
  const uint64_t row_bytes = (uint64_t)H * sizeof(float);
  uint32_t page_log2 = 0;
  while (((uint64_t)1u << page_log2) < row_bytes)
    ++page_log2;
  uint32_t line_log2 = 0;
  while ((1u << line_log2) < cache_line_size)
    ++line_log2;
  if (page_log2 < line_log2)
    page_log2 = line_log2;
  return page_log2;
}

// expert_aligned: coarse page must cover a full [H,H] W tile so dense row-major
// loads stay on one L2 bank; row-sized pages alone would stripe rows across banks.
static uint32_t choose_expert_aligned_page_log2(uint32_t H,
                                                uint32_t cache_line_size) {
  const uint32_t row_page_log2 = choose_coarse_page_log2(H, cache_line_size);
  const uint64_t tile_bytes = (uint64_t)H * (uint64_t)H * sizeof(float);
  const uint32_t tile_page_log2 = ceil_log2_u64(tile_bytes);
  return (row_page_log2 > tile_page_log2) ? row_page_log2 : tile_page_log2;
}

static bool addrs_share_l2_bank(uint64_t addr0,
                                uint64_t addr1,
                                uint32_t page_log2,
                                uint32_t num_banks) {
  return get_l2_bank_for_byte_addr(addr0, page_log2, num_banks) ==
         get_l2_bank_for_byte_addr(addr1, page_log2, num_banks);
}

// Page-align W and rotate so expert-0's tile starts on coarse bank 0.
static uint64_t expert_w_data_byte_offset(uint64_t w_buf_addr,
                                          uint32_t page_log2,
                                          uint32_t num_banks) {
  const uint64_t page_bytes = 1ull << page_log2;
  const uint64_t page_pad = (-w_buf_addr) & (page_bytes - 1ull);
  const uint64_t page_aligned = w_buf_addr + page_pad;
  const uint32_t bank =
      get_l2_bank_for_byte_addr(page_aligned, page_log2, num_banks);
  const uint32_t bank_pad = (num_banks - bank) % num_banks;
  return page_pad + (uint64_t)bank_pad * page_bytes;
}

static uint64_t align_byte_addr_to_l2_bank(uint64_t byte_addr,
                                           uint32_t target_bank,
                                           uint32_t page_log2,
                                           uint32_t num_banks) {
  const uint64_t page_bytes = 1ull << page_log2;
  uint64_t aligned = (byte_addr + page_bytes - 1) & ~(page_bytes - 1);
  const uint32_t bank =
      get_l2_bank_for_byte_addr(aligned, page_log2, num_banks);
  const uint32_t bank_pad = (target_bank + num_banks - bank) % num_banks;
  return aligned + (uint64_t)bank_pad * page_bytes;
}

static uint64_t layout_page_aligned_bins(uint64_t data_addr,
                                         uint32_t E,
                                         uint32_t page_log2,
                                         uint32_t num_banks,
                                         const uint32_t* target_banks,
                                         const uint32_t* bin_floats,
                                         std::vector<uint32_t>& base_off_floats) {
  base_off_floats.assign(E, 0);
  uint64_t cur_byte = data_addr;
  for (uint32_t e = 0; e < E; ++e) {
    cur_byte = align_byte_addr_to_l2_bank(
        cur_byte, target_banks[e], page_log2, num_banks);
    base_off_floats[e] =
        (uint32_t)((cur_byte - data_addr) / sizeof(float));
    cur_byte += (uint64_t)bin_floats[e] * sizeof(float);
  }
  return cur_byte;
}

static uint64_t child_pool_slot_byte_offset(uint32_t p,
                                            uint32_t target_bank,
                                            uint32_t pool_page_bytes,
                                            uint32_t pool_base_bank,
                                            uint32_t num_banks) {
  const uint32_t compact = (uint32_t)sizeof(kernel_arg_t);
  if (!pool_page_bytes || pool_page_bytes <= compact)
    return (uint64_t)p * compact;
  const uint32_t delta =
      (target_bank + num_banks - pool_base_bank) % num_banks;
  return (uint64_t)(p * num_banks + delta) * (uint64_t)pool_page_bytes;
}

static int upload_args() {
  return vx_copy_to_dev(args_buffer, &kernel_arg, 0, sizeof(kernel_arg_t));
}

static int launch_1d(moe_op_t op, uint32_t n, const char* mesh_tag) {
  kernel_arg.op = (uint32_t)op;
  kernel_arg.row_offset = 0;
  kernel_arg.xperm_off_p = 0;
  RT_CHECK(upload_args());
  uint64_t num_threads;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_THREADS, &num_threads));
  uint32_t NT = (uint32_t)num_threads;
  uint32_t grid_dim[1] = {(n + NT - 1) / NT};
  uint32_t block_dim[1] = {NT};
  mesh_stats_tag(mesh_tag);
  RT_CHECK(vx_start_g(device, krnl_buffer, args_buffer, 1, grid_dim, block_dim, 0));
  RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));
  return 0;
}

// Device-side parallel master: one host run() per phase; num_cores master
// blocks each enqueue a subset of the T pinned child grids.
static int launch_row_phase(moe_op_t op,
                            vx_buffer_h affinity_buffer,
                            const char* mesh_tag,
                            uint32_t num_dispatchers,
                            uint32_t pool_page_bytes,
                            uint32_t pool_base_bank,
                            bool mesh_enable) {
  uint64_t num_threads = 0;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_THREADS, &num_threads));
  uint32_t NT = (uint32_t)num_threads;

  uint64_t kernel_pc = 0, pool_addr = 0, aff_addr = 0;
  RT_CHECK(vx_mem_address(krnl_buffer, &kernel_pc));
  RT_CHECK(vx_mem_address(child_pool_buffer, &pool_addr));
  RT_CHECK(vx_mem_address(affinity_buffer, &aff_addr));

  master_arg_t ma{};
  ma.op = (uint32_t)MOE_OP_MASTER;
  ma.child_op = (uint32_t)op;
  ma.T = T;
  ma.NT = NT;
  ma.grid_x = (H + NT - 1) / NT;
  ma.num_dispatchers = num_dispatchers ? num_dispatchers : 1u;
  ma.pool_page_bytes = pool_page_bytes;
  ma.pool_base_bank = pool_base_bank;
  ma.kernel_pc = kernel_pc;
  ma.child_pool_addr = pool_addr;
  ma.affinity_addr = aff_addr;
  RT_CHECK(vx_copy_to_dev(master_arg_buffer, &ma, 0, sizeof(ma)));

  RT_CHECK(vx_perf_reset(device));
  if (mesh_enable)
    mesh_stats_tag(mesh_tag);
  uint32_t master_grid[1] = {ma.num_dispatchers};
  uint32_t master_block[1] = {1};
  RT_CHECK(vx_start_g_affine(device, krnl_buffer, master_arg_buffer,
                             1, master_grid, master_block, 0,
                             VORTEX_AFFINITY_ANY));
  RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));
  return 0;
}

static int launch_expert_ffn_core_sweep(const char* mesh_tag,
                                        uint32_t num_cores,
                                        uint32_t pool_page_bytes,
                                        uint32_t pool_base_bank,
                                        bool by_expert_bin,
                                        const int32_t* expert_counts,
                                        const int32_t* expert_offsets,
                                        uint32_t num_experts,
                                        bool mesh_enable) {
  uint64_t num_threads = 0;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_THREADS, &num_threads));
  const uint32_t NT = (uint32_t)num_threads;
  uint32_t grid[1] = {(H + NT - 1) / NT};
  uint32_t block[1] = {NT};

  uint64_t pool_addr = 0;
  RT_CHECK(vx_mem_address(child_pool_buffer, &pool_addr));

  RT_CHECK(vx_perf_reset(device));
  if (mesh_enable)
    mesh_stats_tag(mesh_tag);

  expert_ffn_core_arg_t ec{};
  ec.T = T;
  ec.H = H;
  ec.E = E;
  ec.by_expert_bin = by_expert_bin ? 1u : 0u;
  ec.pool_page_bytes = pool_page_bytes;
  ec.pool_base_bank = pool_base_bank;
  ec.num_banks = num_cores ? num_cores : 1u;
  ec.child_pool_addr = pool_addr;
  for (uint32_t e = 0; e < num_experts && e < 16u; ++e) {
    ec.expert_counts[e] = expert_counts[e];
    ec.expert_offsets[e] = expert_offsets[e];
  }

  for (uint32_t c = 0; c < num_cores; ++c) {
    ec.op = (uint32_t)MOE_OP_EXPERT_FFN;
    ec.core_id = c;
    RT_CHECK(vx_copy_to_dev(args_buffer, &ec, 0, sizeof(ec)));
    RT_CHECK(vx_start_g_affine(device, krnl_buffer, args_buffer, 1, grid, block, 0, c));
    RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));
  }
  return 0;
}

static uint64_t query_max_mcycle() {
  uint64_t num_cores = 0;
  if (vx_dev_caps(device, VX_CAPS_NUM_CORES, &num_cores) != 0)
    return 0;
  uint64_t max_cy = 0;
  for (uint32_t c = 0; c < (uint32_t)num_cores; ++c) {
    uint64_t cy = 0;
    if (vx_mpm_query(device, VX_DCR_MPM_CLASS_BASE, VX_CSR_MCYCLE, c, &cy) != 0)
      continue;
    if (cy > max_cy)
      max_cy = cy;
  }
  return max_cy;
}

int main(int argc, char** argv) {
  parse_args(argc, argv);

  const char* env_xperm = std::getenv("MOE_XPERM_LAYOUT");
  const bool expert_aligned =
      (env_xperm != nullptr) && (std::strcmp(env_xperm, "expert_aligned") == 0);
  const char* layout_name = expert_aligned ? "expert_aligned" : "baseline";

  std::cout << "T=" << T << " H=" << H << " E=" << E
            << " layout=" << layout_name;
  if (expert_aligned)
    std::cout << " (coarse L2 pages + core affinity + parallel master)";
  else
    std::cout << " (fine L2 banks + affinity any)";
  std::cout << "\n";

  RT_CHECK(vx_dev_open(&device));

  uint64_t isa_flags = 0;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_ISA_FLAGS, &isa_flags));
  const bool l2_en = (isa_flags & VX_ISA_EXT_L2CACHE) != 0;

  uint64_t num_cores = 0;
  uint64_t cache_line_size = 0;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_CORES, &num_cores));
  RT_CHECK(vx_dev_caps(device, VX_CAPS_CACHE_LINE_SIZE, &cache_line_size));
  if (cache_line_size == 0)
    cache_line_size = 64;
  const uint32_t num_banks = (num_cores > 0) ? (uint32_t)num_cores : 1u;

  // expert_aligned: coarse row-sized L2 pages + per-row core affinity.
  // baseline (default): Vortex default fine (cache-line interleaved) L2 banks
  // and VORTEX_AFFINITY_ANY load balancing — no L2_BANK_POLICY DCR write.
  uint32_t coarse_page_log2 = 0;
  const uint64_t w_tile_floats = (uint64_t)H * (uint64_t)H;
  const uint32_t w_row_stride_floats = H;
  if (expert_aligned) {
    const uint32_t row_bytes = H * (uint32_t)sizeof(float);
    if ((row_bytes == 0) || ((row_bytes & (row_bytes - 1)) != 0)) {
      std::cerr << "expert_aligned requires H with power-of-two row bytes (got "
                << row_bytes << ")\n";
      cleanup();
      return 1;
    }
    coarse_page_log2 = choose_expert_aligned_page_log2(H, (uint32_t)cache_line_size);
    if (l2_en) {
      RT_CHECK(vx_dcr_write(device, VX_DCR_BASE_L2_BANK_POLICY,
                            encode_l2_bank_policy((uint8_t)coarse_page_log2)));
    }
    const uint64_t w_tile_bytes = w_tile_floats * sizeof(float);
    if (((1ull << coarse_page_log2) != w_tile_bytes) ||
        ((w_tile_bytes & (w_tile_bytes - 1)) != 0)) {
      std::cerr << "expert_aligned requires W tile_bytes to equal a power-of-two "
                << "coarse page (got tile=" << w_tile_bytes
                << " page=" << (1ull << coarse_page_log2) << ")\n";
      cleanup();
      return 1;
    }
  }

  size_t x_bytes = size_t(T) * H * sizeof(float);
  const uint32_t coarse_page_bytes =
      expert_aligned ? (1u << coarse_page_log2) : 0u;
  const uint64_t x_payload_bytes = (uint64_t)T * H * sizeof(float);
  size_t x_perm_bytes = (size_t)x_payload_bytes;
  if (expert_aligned) {
    // Worst case: every token row uses num_banks row spacing within its expert bin.
    const uint64_t xperm_payload_worst =
        (uint64_t)T * (uint64_t)num_banks * H * sizeof(float);
    const uint64_t xperm_max_pad =
        (uint64_t)E * (uint64_t)(num_banks - 1u) * (uint64_t)coarse_page_bytes;
    x_perm_bytes = (size_t)(xperm_payload_worst + xperm_max_pad +
                            coarse_page_bytes - 1);
  }
  size_t ids_bytes = size_t(T) * sizeof(int32_t);
  size_t expert_bytes = size_t(E) * sizeof(int32_t);
  const uint64_t w_payload_bytes = (uint64_t)E * w_tile_floats * sizeof(float);
  size_t w_alloc_bytes = (size_t)w_payload_bytes;
  if (expert_aligned) {
    const uint64_t w_max_pad =
        (uint64_t)E * (uint64_t)(num_banks - 1u) * (uint64_t)coarse_page_bytes;
    w_alloc_bytes = (size_t)(w_payload_bytes + w_max_pad + coarse_page_bytes - 1);
    w_alloc_bytes = (w_alloc_bytes + 3) & ~size_t(3);
  }

  std::vector<float> hX(T * H);
  std::vector<float> hW(size_t(E) * size_t(w_tile_floats), 1.f);
  std::vector<float> hY(T * H);
  std::vector<int32_t> hTopk(T);
  std::vector<int32_t> hCounts(E, 0);
  std::vector<int32_t> hOffsets(E);
  std::vector<int32_t> hCursor(E, 0);

  std::mt19937 rng(123);
  std::uniform_real_distribution<float> uf(0.f, 1.f);
  std::uniform_int_distribution<int32_t> ue(0, (int32_t)E - 1);

  for (auto& v : hX)
    v = uf(rng);
  for (int32_t t = 0; t < (int32_t)T; ++t)
    hTopk[t] = ue(rng);

  RT_CHECK(vx_mem_alloc(device, x_bytes, VX_MEM_READ, &X_buffer));
  RT_CHECK(vx_mem_address(X_buffer, &kernel_arg.X_addr));
  RT_CHECK(vx_mem_alloc(device, x_perm_bytes, VX_MEM_READ_WRITE, &X_perm_buffer));
  RT_CHECK(vx_mem_address(X_perm_buffer, &kernel_arg.X_perm_addr));
  RT_CHECK(vx_mem_alloc(device, x_bytes, VX_MEM_WRITE, &Y_buffer));
  RT_CHECK(vx_mem_address(Y_buffer, &kernel_arg.Y_addr));
  RT_CHECK(vx_mem_alloc(device, ids_bytes, VX_MEM_READ, &topk_buffer));
  RT_CHECK(vx_mem_address(topk_buffer, &kernel_arg.topk_addr));
  RT_CHECK(vx_mem_alloc(device, expert_bytes, VX_MEM_READ_WRITE, &counts_buffer));
  RT_CHECK(vx_mem_address(counts_buffer, &kernel_arg.expert_counts_addr));
  RT_CHECK(vx_mem_alloc(device, expert_bytes, VX_MEM_READ, &offsets_buffer));
  RT_CHECK(vx_mem_address(offsets_buffer, &kernel_arg.expert_offsets_addr));
  RT_CHECK(vx_mem_alloc(device, expert_bytes, VX_MEM_READ_WRITE, &cursor_buffer));
  RT_CHECK(vx_mem_address(cursor_buffer, &kernel_arg.expert_cursor_addr));
  RT_CHECK(vx_mem_alloc(device, ids_bytes, VX_MEM_READ_WRITE, &sorted_buffer));
  RT_CHECK(vx_mem_address(sorted_buffer, &kernel_arg.sorted_token_ids_addr));
  RT_CHECK(vx_mem_alloc(device, w_alloc_bytes, VX_MEM_READ, &W_buffer));
  RT_CHECK(vx_mem_address(W_buffer, &kernel_arg.W_addr));
  uint64_t w_data_byte_offset = 0;
  uint64_t w_used_bytes = w_payload_bytes;
  std::vector<uint32_t> w_expert_offset(E, 0);
  std::vector<uint32_t> expert_core(E, 0);
  if (expert_aligned) {
    w_data_byte_offset = expert_w_data_byte_offset(
        kernel_arg.W_addr, coarse_page_log2, num_banks);
    kernel_arg.W_addr += w_data_byte_offset;

    std::vector<uint32_t> w_bin_floats(E, (uint32_t)w_tile_floats);
    std::vector<uint32_t> w_target_banks(E);
    for (uint32_t e = 0; e < E; ++e)
      w_target_banks[e] = e;
    const uint64_t w_end_byte = layout_page_aligned_bins(
        kernel_arg.W_addr, E, coarse_page_log2, num_banks,
        w_target_banks.data(), w_bin_floats.data(), w_expert_offset);
    const uint64_t w_buf_base = kernel_arg.W_addr - w_data_byte_offset;
    w_used_bytes = w_end_byte - w_buf_base;
    if (w_end_byte > w_buf_base + w_alloc_bytes) {
      std::cerr << "expert_aligned W layout exceeds allocation (used "
                << (w_end_byte - w_buf_base) << " of " << w_alloc_bytes
                << " bytes)\n";
      cleanup();
      return 1;
    }
    for (uint32_t e = 0; e < E; ++e) {
      const uint64_t w_tile_addr =
          kernel_arg.W_addr + (uint64_t)w_expert_offset[e] * sizeof(float);
      expert_core[e] =
          get_l2_bank_for_byte_addr(w_tile_addr, coarse_page_log2, num_banks);
      if (expert_core[e] != e) {
        std::cerr << "expert_aligned W tile bank mismatch for expert " << e
                  << " (got bank " << expert_core[e] << ", expected " << e
                  << ", tile=0x" << std::hex << w_tile_addr << std::dec
                  << ")\n";
        cleanup();
        return 1;
      }
      const uint64_t w_tile_end =
          w_tile_addr + (uint64_t)w_tile_floats * sizeof(float) - 1;
      if (!addrs_share_l2_bank(w_tile_addr, w_tile_end, coarse_page_log2,
                               num_banks)) {
        std::cerr << "expert_aligned W tile spans multiple banks for expert "
                  << e << "\n";
        cleanup();
        return 1;
      }
    }
  } else {
    for (uint32_t e = 0; e < E; ++e)
      w_expert_offset[e] = (uint32_t)((uint64_t)e * w_tile_floats);
  }
  const size_t args_alloc_bytes =
      std::max({sizeof(kernel_arg_t), sizeof(master_arg_t),
                sizeof(expert_ffn_core_arg_t)});
  RT_CHECK(vx_mem_alloc(device, args_alloc_bytes, VX_MEM_READ, &args_buffer));

  kernel_arg.T = T;
  kernel_arg.H = H;
  kernel_arg.E = E;
  kernel_arg.row_offset = 0;
  kernel_arg.w_row_stride = w_row_stride_floats;
  kernel_arg.w_off_p = 0;

  RT_CHECK(vx_copy_to_dev(X_buffer, hX.data(), 0, x_bytes));
  RT_CHECK(vx_copy_to_dev(topk_buffer, hTopk.data(), 0, ids_bytes));
  if (expert_aligned) {
    const size_t w_tile_bytes = (size_t)w_tile_floats * sizeof(float);
    const size_t w_dev_floats =
        (size_t)((w_used_bytes + sizeof(float) - 1) / sizeof(float));
    std::vector<float> hW_dev(w_dev_floats, 0.f);
    for (uint32_t e = 0; e < E; ++e) {
      std::memcpy(hW_dev.data() + w_expert_offset[e],
                  hW.data() + (size_t)e * (size_t)w_tile_floats, w_tile_bytes);
    }
    RT_CHECK(vx_copy_to_dev(W_buffer, hW_dev.data(), w_data_byte_offset,
                            w_used_bytes));
  } else {
    RT_CHECK(vx_copy_to_dev(W_buffer, hW.data(), 0, (size_t)w_payload_bytes));
  }
  RT_CHECK(vx_copy_to_dev(counts_buffer, hCounts.data(), 0, expert_bytes));

  RT_CHECK(vx_upload_kernel_file(device, kernel_file, &krnl_buffer));

  RT_CHECK(launch_1d(MOE_OP_COUNT, T, "count"));
  log_l2_bank_stalls("count", l2_en);

  RT_CHECK(vx_copy_from_dev(hCounts.data(), counts_buffer, 0, expert_bytes));
  hOffsets = exclusive_scan(hCounts);
  RT_CHECK(vx_copy_to_dev(offsets_buffer, hOffsets.data(), 0, expert_bytes));

  RT_CHECK(vx_copy_to_dev(cursor_buffer, hCursor.data(), 0, expert_bytes));
  RT_CHECK(launch_1d(MOE_OP_SCATTER, T, "scatter"));
  log_l2_bank_stalls("scatter", l2_en);

  std::vector<int32_t> hSorted(T, 0);
  RT_CHECK(vx_copy_from_dev(hSorted.data(), sorted_buffer, 0, ids_bytes));

  g_xperm_off.assign(T, 0);
  g_w_off.assign(T, 0);

  std::vector<uint32_t> xperm_expert_base(E, 0);
  std::vector<uint32_t> xperm_slot_stride(E, H);
  if (expert_aligned) {
    const uint64_t xperm_data_byte_offset = expert_w_data_byte_offset(
        kernel_arg.X_perm_addr, coarse_page_log2, num_banks);
    kernel_arg.X_perm_addr += xperm_data_byte_offset;

    const uint32_t page_rows =
        coarse_page_bytes / (H * (uint32_t)sizeof(float));
    std::vector<uint32_t> xperm_bin_floats(E);
    for (uint32_t e = 0; e < E; ++e) {
      const bool dense_bin =
          (hCounts[e] > 0) && ((uint32_t)hCounts[e] <= page_rows);
      xperm_slot_stride[e] = dense_bin ? H : (num_banks * H);
      xperm_bin_floats[e] = (uint32_t)hCounts[e] * xperm_slot_stride[e];
    }
    const uint64_t xperm_end_byte = layout_page_aligned_bins(
        kernel_arg.X_perm_addr, E, coarse_page_log2, num_banks,
        expert_core.data(), xperm_bin_floats.data(), xperm_expert_base);
    const uint64_t xperm_buf_base =
        kernel_arg.X_perm_addr - xperm_data_byte_offset;
    if (xperm_end_byte > xperm_buf_base + x_perm_bytes) {
      std::cerr << "expert_aligned X_perm layout exceeds allocation (used "
                << (xperm_end_byte - xperm_buf_base) << " of " << x_perm_bytes
                << " bytes)\n";
      cleanup();
      return 1;
    }
    for (uint32_t e = 0; e < E; ++e) {
      if (hCounts[e] == 0)
        continue;
      const uint64_t bin_base =
          kernel_arg.X_perm_addr +
          (uint64_t)xperm_expert_base[e] * sizeof(float);
      const uint32_t bin_bank = get_l2_bank_for_byte_addr(
          bin_base, coarse_page_log2, num_banks);
      if (bin_bank != expert_core[e]) {
        std::cerr << "expert_aligned X_perm bin bank mismatch for expert " << e
                  << " (got bank " << bin_bank << ", expected "
                  << expert_core[e] << ")\n";
        cleanup();
        return 1;
      }
      for (int32_t slot = 0; slot < hCounts[e]; ++slot) {
        const uint64_t row_addr =
            bin_base + (uint64_t)slot * (uint64_t)xperm_slot_stride[e] *
                       sizeof(float);
        const uint64_t row_end = row_addr + (uint64_t)H * sizeof(float) - 1;
        if (!addrs_share_l2_bank(row_addr, row_end, coarse_page_log2,
                                 num_banks) ||
            get_l2_bank_for_byte_addr(row_addr, coarse_page_log2,
                                      num_banks) != expert_core[e]) {
          std::cerr << "expert_aligned X_perm row bank mismatch expert " << e
                    << " slot " << slot << "\n";
          cleanup();
          return 1;
        }
      }
    }

    std::vector<int32_t> xperm_cursor(E, 0);
    for (uint32_t p = 0; p < T; ++p) {
      const uint32_t token = (uint32_t)hSorted[p];
      const uint32_t expert = (uint32_t)hTopk[token];
      const uint32_t slot = (uint32_t)xperm_cursor[expert]++;
      g_xperm_off[p] = xperm_expert_base[expert] +
                       slot * xperm_slot_stride[expert];
    }
  } else {
    for (uint32_t p = 0; p < T; ++p)
      g_xperm_off[p] = p * H;
  }

  std::vector<uint32_t> permute_affinity_map(T, VORTEX_AFFINITY_ANY);
  std::vector<uint32_t> expert_ffn_affinity_map(T, VORTEX_AFFINITY_ANY);
  std::vector<uint32_t> unpermute_affinity_map(T, VORTEX_AFFINITY_ANY);
  for (uint32_t p = 0; p < T; ++p) {
    const uint32_t token = (uint32_t)hSorted[p];
    const int32_t expert = hTopk[token];
    g_w_off[p] = w_expert_offset[(uint32_t)expert];

    if (expert_aligned) {
      const uint32_t ec = expert_core[(uint32_t)expert];
      permute_affinity_map[p] = ec;
      expert_ffn_affinity_map[p] = ec;
      unpermute_affinity_map[p] =
          get_l2_bank_for_address_coarse(kernel_arg.Y_addr, token, H,
                                         coarse_page_log2, num_banks);
    } else {
      // Core-sweep expert FFN: round-robin rows across cores (permute stays ANY).
      expert_ffn_affinity_map[p] = p % num_banks;
    }
  }

  std::vector<kernel_arg_t> hPool(T, kernel_arg);
  for (uint32_t p = 0; p < T; ++p) {
    hPool[p].row_offset = p;
    hPool[p].xperm_off_p = g_xperm_off[p];
    hPool[p].w_off_p = g_w_off[p];
  }
  const size_t pool_bytes = size_t(T) * sizeof(kernel_arg_t);
  RT_CHECK(vx_mem_alloc(device, pool_bytes, VX_MEM_READ_WRITE, &child_pool_buffer));
  RT_CHECK(vx_copy_to_dev(child_pool_buffer, hPool.data(), 0, pool_bytes));
  const uint32_t pool_page_bytes = (uint32_t)sizeof(kernel_arg_t);
  const uint32_t pool_base_bank = 0u;

  const size_t aff_bytes = size_t(T) * sizeof(uint32_t);
  RT_CHECK(vx_mem_alloc(device, aff_bytes, VX_MEM_READ, &aff_permute_buffer));
  RT_CHECK(vx_mem_alloc(device, aff_bytes, VX_MEM_READ, &aff_expert_ffn_buffer));
  RT_CHECK(vx_mem_alloc(device, aff_bytes, VX_MEM_READ, &aff_unpermute_buffer));
  RT_CHECK(vx_copy_to_dev(aff_permute_buffer, permute_affinity_map.data(),
                          0, aff_bytes));
  RT_CHECK(vx_copy_to_dev(aff_expert_ffn_buffer, expert_ffn_affinity_map.data(),
                          0, aff_bytes));
  RT_CHECK(vx_copy_to_dev(aff_unpermute_buffer, unpermute_affinity_map.data(),
                          0, aff_bytes));
  RT_CHECK(vx_mem_alloc(device, sizeof(master_arg_t), VX_MEM_READ,
                        &master_arg_buffer));

  // Parallel master (num_cores dispatch blocks) spreads KMU enqueue for expert_aligned.
  const uint32_t num_dispatchers = expert_aligned ? num_banks : 1u;
  RT_CHECK(launch_row_phase(MOE_OP_PERMUTE, aff_permute_buffer, "permute",
                            num_dispatchers, pool_page_bytes, pool_base_bank,
                            true));
  log_l2_bank_stalls("permute", l2_en);
  RT_CHECK(launch_expert_ffn_core_sweep("expert_ffn", num_banks, pool_page_bytes,
                                        pool_base_bank, expert_aligned,
                                        hCounts.data(), hOffsets.data(), E,
                                        true));
  log_l2_bank_stalls("expert_ffn", l2_en);
  RT_CHECK(launch_row_phase(MOE_OP_UNPERMUTE, aff_unpermute_buffer, "unpermute",
                            num_dispatchers, pool_page_bytes, pool_base_bank,
                            true));
  log_l2_bank_stalls("unpermute", l2_en);

  const uint64_t max_mcycle = query_max_mcycle();
  const uint64_t l2_bank_stalls = query_l2_bank_stalls(l2_en);
  std::cerr << "MOE_METRICS layout=" << layout_name
            << " max_mcycle=" << max_mcycle
            << " l2_bank_stalls=" << l2_bank_stalls << std::endl;

  RT_CHECK(vx_copy_from_dev(hY.data(), Y_buffer, 0, x_bytes));

  int errors = 0;
  for (uint32_t i = 0; i < T * H; ++i) {
    union fi_t {
      float f;
      int32_t i;
    };
    fi_t actual, expected;
    actual.f = hY[i];
    expected.f = hX[i];
    if (std::abs(actual.i - expected.i) > FLOAT_ULP) {
      if (errors < 20) {
        printf("*** error: [%u] expected=%f actual=%f\n", i, expected.f, actual.f);
      }
      ++errors;
    }
  }

  cleanup();

  if (errors != 0) {
    std::cout << "Found " << errors << " errors!\n";
    std::cout << "FAILED!\n";
    return 1;
  }

  std::cout << "PASSED!\n";
  return 0;
}
