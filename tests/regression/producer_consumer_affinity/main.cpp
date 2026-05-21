// E2E: producer fills per-core coarse pages, then consumers are pinned to the
// matching cores so most L2 traffic should be 0-hop in coarse mode.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>
#include <vortex.h>
#include <VX_types.h>
#include "common.h"

#define RT_CHECK(_expr)                                          \
  do {                                                           \
    int _ret = (_expr);                                          \
    if (0 == _ret) break;                                        \
    std::printf("Error: '%s' returned %d!\n", #_expr, (int)_ret);\
    cleanup();                                                   \
    std::exit(-1);                                               \
  } while (false)

static const char* kernel_file = "kernel.vxbin";

static vx_device_h device = nullptr;
static vx_buffer_h krnl = nullptr;
static vx_buffer_h data_buf = nullptr;
static vx_buffer_h verify_buf = nullptr;
static vx_buffer_h prod_arg_buf = nullptr;
static vx_buffer_h child_pool_buf = nullptr;

static void cleanup() {
  if (!device)
    return;
  if (child_pool_buf)
    vx_mem_free(child_pool_buf);
  if (prod_arg_buf)
    vx_mem_free(prod_arg_buf);
  if (verify_buf)
    vx_mem_free(verify_buf);
  if (data_buf)
    vx_mem_free(data_buf);
  if (krnl)
    vx_mem_free(krnl);
  vx_dev_close(device);
}

struct PhaseStats {
  uint64_t max_cycle = 0;
  uint64_t sum_load_lt = 0;
  uint64_t l2_reads = 0;
  uint64_t l2_read_miss = 0;
};

static int query_phase_stats(uint32_t num_cores, PhaseStats* out) {
  out->max_cycle = 0;
  out->sum_load_lt = 0;
  for (uint32_t c = 0; c < num_cores; ++c) {
    uint64_t cyc = 0;
    uint64_t lt = 0;
    if (vx_mpm_query(device, VX_DCR_MPM_CLASS_BASE, VX_CSR_MCYCLE, c, &cyc) != 0)
      return -1;
    if (vx_mpm_query(device, VX_DCR_MPM_CLASS_BASE, VX_CSR_MPM_LOAD_LT, c, &lt) != 0)
      return -1;
    out->sum_load_lt += lt;
    if (cyc > out->max_cycle)
      out->max_cycle = cyc;
  }
  if (vx_mpm_query(device, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_L2CACHE_READS,
                   0, &out->l2_reads) != 0)
    return -1;
  if (vx_mpm_query(device, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_L2CACHE_MISS_R,
                   0, &out->l2_read_miss) != 0)
    return -1;
  return 0;
}

static uint32_t encode_l2_bank_policy(bool coarse_mode, uint8_t page_log2) {
  return (coarse_mode ? 1u : 0u) | ((static_cast<uint32_t>(page_log2) & 0x7Fu) << 1);
}

int main() {
  std::cout << "=== producer_consumer_affinity (per-core coarse pages) ===\n";

  RT_CHECK(vx_dev_open(&device));

  uint64_t num_cores = 0;
  uint64_t num_warps = 0;
  uint64_t num_threads = 0;
  uint64_t isa = 0;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_CORES, &num_cores));
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_WARPS, &num_warps));
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_THREADS, &num_threads));
  RT_CHECK(vx_dev_caps(device, VX_CAPS_ISA_FLAGS, &isa));

  if ((isa & VX_ISA_EXT_L2CACHE) == 0) {
    std::cout << "L2 not enabled; rebuild with --l2cache --l2mesh\n";
    cleanup();
    return 1;
  }

  RT_CHECK(vx_upload_kernel_file(device, kernel_file, &krnl));
  uint64_t kernel_pc = 0;
  RT_CHECK(vx_mem_address(krnl, &kernel_pc));

  const uint32_t coarse_page_log2 = 16;   /* 64KB pages */
  const uint32_t tensor_bytes = 1u << 20; /* 1MB tensor */
  const uint32_t total_elems = tensor_bytes / static_cast<uint32_t>(sizeof(float));
  const uint32_t num_banks = 4;           /* match L2_NUM_BANKS=4 test config */
  const uint32_t page_size = 1u << coarse_page_log2;
  const uint32_t consumer_block_x =
      static_cast<uint32_t>(num_warps * num_threads);

  RT_CHECK(vx_mem_alloc(device, tensor_bytes, VX_MEM_READ_WRITE, &data_buf));
  RT_CHECK(vx_mem_alloc(device, tensor_bytes, VX_MEM_READ_WRITE, &verify_buf));
  RT_CHECK(vx_mem_alloc(device, sizeof(master_arg_t), VX_MEM_READ_WRITE,
                        &prod_arg_buf));
  RT_CHECK(vx_mem_alloc(device, sizeof(child_arg_t) * static_cast<uint32_t>(num_cores),
                        VX_MEM_READ_WRITE, &child_pool_buf));

  uint64_t data_addr = 0;
  uint64_t verify_addr = 0;
  uint64_t child_pool_addr = 0;
  RT_CHECK(vx_mem_address(data_buf, &data_addr));
  RT_CHECK(vx_mem_address(verify_buf, &verify_addr));
  RT_CHECK(vx_mem_address(child_pool_buf, &child_pool_addr));

  const uint64_t data_page = data_addr >> coarse_page_log2;
  const uint64_t verify_page = verify_addr >> coarse_page_log2;
  std::cout << "addr_info: data_addr=0x" << std::hex << data_addr
            << " verify_addr=0x" << verify_addr << std::dec
            << " data_page_mod_banks=" << (data_page % num_banks)
            << " verify_page_mod_banks=" << (verify_page % num_banks)
            << " page_size=" << page_size << "\n";

  std::vector<float> zeros(total_elems, 0.0f);
  RT_CHECK(vx_copy_to_dev(data_buf, zeros.data(), 0, tensor_bytes));
  RT_CHECK(vx_copy_to_dev(verify_buf, zeros.data(), 0, tensor_bytes));

  auto run_case = [&](const char* name, uint32_t consumer_affinity, PhaseStats* out_stats, master_arg_t* out_meta) {
    std::vector<float> zeros_local(total_elems, 0.0f);
    RT_CHECK(vx_copy_to_dev(data_buf, zeros_local.data(), 0, tensor_bytes));
    RT_CHECK(vx_copy_to_dev(verify_buf, zeros_local.data(), 0, tensor_bytes));

    const uint32_t dcr = encode_l2_bank_policy(true, coarse_page_log2);
    RT_CHECK(vx_dcr_write(device, VX_DCR_BASE_L2_BANK_POLICY, dcr));

    master_arg_t m{};
    m.entry_kind = PC_ENTRY_PRODUCER;
    m.total_elems = total_elems;
    m.coarse_page_log2 = coarse_page_log2;
    m.num_banks = num_banks;
    m.num_cores = static_cast<uint32_t>(num_cores);
    m.consumer_block_x = consumer_block_x;
    m.consumer_affinity = consumer_affinity;
    m.data_addr = data_addr;
    m.verify_addr = verify_addr;
    m.child_pool_addr = child_pool_addr;
    m.kernel_pc = kernel_pc;

    RT_CHECK(vx_copy_to_dev(prod_arg_buf, &m, 0, sizeof(m)));
    uint32_t grid = 1;
    uint32_t block = 1;
    RT_CHECK(vx_start_g_affine(device, krnl, prod_arg_buf, 1, &grid, &block, 0,
                               VORTEX_AFFINITY_ANY));
    RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));

    // Measure consumer phase only.
    RT_CHECK(vx_perf_reset(device));
    m.entry_kind = PC_ENTRY_DISPATCH;
    RT_CHECK(vx_copy_to_dev(prod_arg_buf, &m, 0, sizeof(m)));
    RT_CHECK(vx_start_g_affine(device, krnl, prod_arg_buf, 1, &grid, &block, 0,
                               VORTEX_AFFINITY_ANY));
    RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));

    RT_CHECK(vx_copy_from_dev(out_meta, prod_arg_buf, 0, sizeof(*out_meta)));
    RT_CHECK(query_phase_stats(static_cast<uint32_t>(num_cores), out_stats));
    std::printf("PC_AFF_CSV,phase=%s,l2_reads=%lu,l2_read_miss=%lu,load_lt_sum=%lu,max_cycle=%lu,active_chunks=%u,page_elems=%u\n",
      name,
      static_cast<unsigned long>(out_stats->l2_reads),
      static_cast<unsigned long>(out_stats->l2_read_miss),
      static_cast<unsigned long>(out_stats->sum_load_lt),
      static_cast<unsigned long>(out_stats->max_cycle),
      out_meta->active_chunks,
      out_meta->page_elems);
  };

  PhaseStats baseline_stats{}, affinity_stats{};
  master_arg_t baseline_meta{}, affinity_meta{};
  std::cout << "=== Run: baseline_any ===\n";
  run_case("baseline_any", 0u, &baseline_stats, &baseline_meta);
  std::cout << "=== Run: affinity_pinned ===\n";
  run_case("affinity_pinned", 1u, &affinity_stats, &affinity_meta);

  std::vector<float> verify(total_elems, 0.0f);
  RT_CHECK(vx_copy_from_dev(verify.data(), verify_buf, 0, tensor_bytes));

  const uint32_t page_elems = affinity_meta.page_elems;
  const uint32_t active = affinity_meta.active_chunks;
  int errors = 0;
  for (uint32_t c = 0; c < active; ++c) {
    const uint32_t begin = c * page_elems;
    uint32_t end = begin + page_elems;
    if (end > total_elems)
      end = total_elems;
    for (uint32_t i = begin; i < end; ++i) {
      const float ref = 2.0f * static_cast<float>(i - begin);
      const float got = verify[i];
      if (std::fabs(got - ref) > 1e-4f) {
        if (errors < 8) {
          std::cout << "verify mismatch core=" << c << " i=" << i << " got="
                    << got << " ref=" << ref << "\n";
        }
        ++errors;
      }
    }
  }

  const double baseline_miss_rate = (baseline_stats.l2_reads == 0)
      ? 1.0
      : static_cast<double>(baseline_stats.l2_read_miss) / static_cast<double>(baseline_stats.l2_reads);
  const double affinity_miss_rate = (affinity_stats.l2_reads == 0)
      ? 1.0
      : static_cast<double>(affinity_stats.l2_read_miss) / static_cast<double>(affinity_stats.l2_reads);

  std::cout << "\n======== PC_AFF_BASELINE_VS_AFFINITY ========\n"
            << std::left << std::setw(18) << "Mode"
            << std::setw(12) << "Chunks"
            << std::setw(14) << "L2_READS"
            << std::setw(14) << "L2_RD_MISS"
            << std::setw(12) << "MISS_RT"
            << std::setw(14) << "LOAD_LT_SUM"
            << std::setw(14) << "MAX_CYCLE"
            << "\n" << std::string(98, '-') << "\n";
  std::cout << std::setw(18) << "baseline_any"
            << std::setw(12) << baseline_meta.active_chunks
            << std::setw(14) << baseline_stats.l2_reads
            << std::setw(14) << baseline_stats.l2_read_miss
            << std::setw(12) << std::fixed << std::setprecision(3) << baseline_miss_rate
            << std::setw(14) << baseline_stats.sum_load_lt
            << std::setw(14) << baseline_stats.max_cycle
            << "\n";
  std::cout << std::setw(18) << "affinity_pinned"
            << std::setw(12) << affinity_meta.active_chunks
            << std::setw(14) << affinity_stats.l2_reads
            << std::setw(14) << affinity_stats.l2_read_miss
            << std::setw(12) << std::fixed << std::setprecision(3) << affinity_miss_rate
            << std::setw(14) << affinity_stats.sum_load_lt
            << std::setw(14) << affinity_stats.max_cycle
            << "\n" << std::string(98, '-') << "\n";
  std::cout << "(Consumer phase only is measured; producer phase is excluded via vx_perf_reset.)\n";

  cleanup();

  if (errors != 0) {
    std::cout << "Found " << errors << " functional errors!\nFAILED!\n";
    return 1;
  }
  if (affinity_stats.max_cycle > baseline_stats.max_cycle) {
    std::cout << "Microarchitectural gate failed (affinity consumer phase slower than baseline).\nFAILED!\n";
    return 2;
  }

  std::cout << "PASSED!\n";
  return 0;
}
