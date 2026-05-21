// L2 mesh + core affinity: one processor.run() per phase; KMU dispatches all partitions.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <unistd.h>
#include <vector>
#include <vortex.h>
#include <VX_types.h>
#include "common.h"
#include "../../../mesh_l2_data.h"

#define RT_CHECK(_expr)                                          \
  do {                                                           \
    int _ret = (_expr);                                          \
    if (0 == _ret) break;                                        \
    std::printf("Error: '%s' returned %d!\n", #_expr, (int)_ret);\
    cleanup();                                                   \
    std::exit(-1);                                              \
  } while (false)

const char* kernel_file = "kernel.vxbin";
bool verify_result = true;

vx_device_h device = nullptr;
vx_buffer_h krnl_buffer = nullptr;
vx_buffer_h mem_buffer = nullptr;
vx_buffer_h src_addr_table_buffer = nullptr;
vx_buffer_h dst_addr_table_buffer = nullptr;
vx_buffer_h remote_addr_table_buffer = nullptr;
vx_buffer_h child_pool_buffer = nullptr;
vx_buffer_h master_arg_buffer = nullptr;

static void show_usage() {
  std::cout << "Vortex mesh L2 affinity demo\n";
  std::cout << "Usage: mesh_l2_affinity [-k kernel.vxbin] [-V] [-h]\n";
}

static void parse_args(int argc, char** argv) {
  int c;
  while ((c = getopt(argc, argv, "k:Vh")) != -1) {
    switch (c) {
      case 'k': kernel_file = optarg; break;
      case 'V': verify_result = false; break;
      case 'h': show_usage(); std::exit(0);
      default:  show_usage(); std::exit(-1);
    }
  }
}

void cleanup() {
  if (!device) return;
  if (master_arg_buffer)        vx_mem_free(master_arg_buffer);
  if (child_pool_buffer)        vx_mem_free(child_pool_buffer);
  if (remote_addr_table_buffer) vx_mem_free(remote_addr_table_buffer);
  if (dst_addr_table_buffer)    vx_mem_free(dst_addr_table_buffer);
  if (src_addr_table_buffer)    vx_mem_free(src_addr_table_buffer);
  if (mem_buffer)               vx_mem_free(mem_buffer);
  if (krnl_buffer)              vx_mem_free(krnl_buffer);
  vx_dev_close(device);
}

struct PhaseResult {
  uint64_t sum_load_latency = 0;
  uint64_t max_cycle = 0;
  uint64_t l2_reads = 0;
  uint64_t l2_read_miss = 0;
};

static int query_phase_metrics(vx_device_h dev,
                               uint32_t num_cores,
                               bool l2_en,
                               PhaseResult* out) {
  out->sum_load_latency = 0;
  out->max_cycle = 0;
  for (uint32_t c = 0; c < num_cores; ++c) {
    uint64_t cy = 0, lt = 0;
    if (vx_mpm_query(dev, VX_DCR_MPM_CLASS_BASE, VX_CSR_MCYCLE, c, &cy) != 0)
      return -1;
    if (vx_mpm_query(dev, VX_DCR_MPM_CLASS_BASE, VX_CSR_MPM_LOAD_LT, c, &lt) != 0)
      return -1;
    out->sum_load_latency += lt;
    if (cy > out->max_cycle)
      out->max_cycle = cy;
  }
  out->l2_reads = 0;
  out->l2_read_miss = 0;
  if (!l2_en)
    return 0;
  if (vx_mpm_query(dev, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_L2CACHE_READS,
                   0, &out->l2_reads) != 0)
    return -1;
  if (vx_mpm_query(dev, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_L2CACHE_MISS_R,
                   0, &out->l2_read_miss) != 0)
    return -1;
  return 0;
}

static void emit_csv(const char* phase, const PhaseResult& r) {
  std::printf(
      "MESH_L2_CSV,phase=%s,LOAD_LT_SUM=%lu,MCYCLE_MAX=%lu,L2_READS=%lu,"
      "L2_READ_MISS=%lu\n",
      phase,
      (unsigned long)r.sum_load_latency,
      (unsigned long)r.max_cycle,
      (unsigned long)r.l2_reads,
      (unsigned long)r.l2_read_miss);
}

int main(int argc, char* argv[]) {
  parse_args(argc, argv);

  std::cout << "=== mesh_l2_affinity (L2 mesh + core affinity) ===\n";

  RT_CHECK(vx_dev_open(&device));

  uint64_t num_cores = 0;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_CORES, &num_cores));
  if (num_cores < MESH_L2_NUM_PARTITIONS) {
    std::cout << "Need at least " << MESH_L2_NUM_PARTITIONS
              << " cores (have " << num_cores << ")\n";
    cleanup();
    return 1;
  }

  uint64_t isa_flags = 0;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_ISA_FLAGS, &isa_flags));
  if ((isa_flags & VX_ISA_EXT_L2CACHE) == 0) {
    std::cout << "L2 not enabled; rebuild with --l2cache --l2mesh\n";
    cleanup();
    return 1;
  }

  const uint64_t region0 = mesh_l2_partition_base[0];
  const uint64_t region_last = mesh_l2_partition_base[MESH_L2_NUM_PARTITIONS - 1];
  const uint64_t alloc_size =
      (region_last - region0) + 2ull * MESH_L2_PARTITION_SPAN_BYTES;

  std::vector<float> host_src(MESH_L2_NUM_PARTITIONS * MESH_L2_FLOATS_PER_PARTITION);
  for (size_t i = 0; i < host_src.size(); ++i) {
    host_src[i] = static_cast<float>(i & 1023u) * 0.001f;
  }

  RT_CHECK(vx_mem_alloc(device, alloc_size, VX_MEM_READ_WRITE, &mem_buffer));
  uint64_t dev_mem_base = 0;
  RT_CHECK(vx_mem_address(mem_buffer, &dev_mem_base));

  std::vector<uint64_t> src_dev(MESH_L2_NUM_PARTITIONS);
  std::vector<uint64_t> dst_dev(MESH_L2_NUM_PARTITIONS);
  std::vector<uint64_t> remote_dev(MESH_L2_NUM_PARTITIONS);

  for (uint32_t p = 0; p < MESH_L2_NUM_PARTITIONS; ++p) {
    const uint64_t off = mesh_l2_partition_base[p] - region0;
    src_dev[p] = dev_mem_base + off;
    dst_dev[p] = src_dev[p] + MESH_L2_PARTITION_SPAN_BYTES;
    remote_dev[p] = dev_mem_base +
        (mesh_l2_partition_base[mesh_l2_partition_remote_core[p]] - region0);
  }

  for (uint32_t p = 0; p < MESH_L2_NUM_PARTITIONS; ++p) {
    const uint64_t part_off = mesh_l2_partition_base[p] - region0;
    const float* host_part = host_src.data() + p * MESH_L2_FLOATS_PER_PARTITION;
    for (uint32_t g = 0; g < MESH_L2_NUM_GRANULES; ++g) {
      const uint32_t elem0 = g * MESH_L2_FLOATS_PER_GRANULE;
      const uint64_t byte_off =
          part_off + mesh_l2_elem_byte_offset(elem0);
      RT_CHECK(vx_copy_to_dev(mem_buffer,
                              host_part + elem0,
                              byte_off,
                              MESH_L2_FLOATS_PER_GRANULE * sizeof(float)));
    }
  }

  RT_CHECK(vx_mem_alloc(device, sizeof(uint64_t) * MESH_L2_NUM_PARTITIONS,
                        VX_MEM_READ, &src_addr_table_buffer));
  RT_CHECK(vx_mem_alloc(device, sizeof(uint64_t) * MESH_L2_NUM_PARTITIONS,
                        VX_MEM_READ, &dst_addr_table_buffer));
  RT_CHECK(vx_mem_alloc(device, sizeof(uint64_t) * MESH_L2_NUM_PARTITIONS,
                        VX_MEM_READ, &remote_addr_table_buffer));
  RT_CHECK(vx_copy_to_dev(src_addr_table_buffer, src_dev.data(), 0,
                          src_dev.size() * sizeof(uint64_t)));
  RT_CHECK(vx_copy_to_dev(dst_addr_table_buffer, dst_dev.data(), 0,
                          dst_dev.size() * sizeof(uint64_t)));
  RT_CHECK(vx_copy_to_dev(remote_addr_table_buffer, remote_dev.data(), 0,
                          remote_dev.size() * sizeof(uint64_t)));

  uint64_t src_table_addr = 0, dst_table_addr = 0, remote_table_addr = 0;
  RT_CHECK(vx_mem_address(src_addr_table_buffer, &src_table_addr));
  RT_CHECK(vx_mem_address(dst_addr_table_buffer, &dst_table_addr));
  RT_CHECK(vx_mem_address(remote_addr_table_buffer, &remote_table_addr));

  const uint32_t child_pool_bytes =
      sizeof(kernel_arg_t) * MESH_L2_NUM_PARTITIONS;
  RT_CHECK(vx_mem_alloc(device, child_pool_bytes, VX_MEM_READ_WRITE,
                        &child_pool_buffer));
  uint64_t child_pool_addr = 0;
  RT_CHECK(vx_mem_address(child_pool_buffer, &child_pool_addr));
  RT_CHECK(vx_mem_alloc(device, sizeof(master_arg_t), VX_MEM_READ,
                        &master_arg_buffer));

  std::cout << "upload kernel\n";
  RT_CHECK(vx_upload_kernel_file(device, kernel_file, &krnl_buffer));
  uint64_t kernel_pc = 0;
  RT_CHECK(vx_mem_address(krnl_buffer, &kernel_pc));

  uint64_t num_warps = 0, num_threads = 0;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_WARPS, &num_warps));
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_THREADS, &num_threads));
  const uint32_t child_block_x = (uint32_t)(num_warps * num_threads);

  struct ModeSpec {
    const char* name;
    uint32_t mode;
  };
  const ModeSpec modes[] = {
      {"home", MESH_AFFINITY_HOME},
      {"any", MESH_AFFINITY_ANY},
      {"remote", MESH_AFFINITY_REMOTE},
  };

  PhaseResult results[3]{};

  for (int m = 0; m < 3; ++m) {
    std::cout << "=== Run: " << modes[m].name << " ===\n";
    RT_CHECK(vx_perf_reset(device));

    master_arg_t ma{};
    ma.entry_kind                     = MESH_ENTRY_MASTER;
    ma.affinity_mode                  = modes[m].mode;
    ma.partition_src_addr_addr        = src_table_addr;
    ma.partition_dst_addr_addr        = dst_table_addr;
    ma.partition_remote_src_addr_addr = remote_table_addr;
    ma.child_arg_pool_addr            = child_pool_addr;
    ma.kernel_pc                      = kernel_pc;
    ma.num_partitions                 = MESH_L2_NUM_PARTITIONS;
    ma.child_block_x                  = child_block_x;
    ma.num_cores                      = (uint32_t)num_cores;

    RT_CHECK(vx_copy_to_dev(master_arg_buffer, &ma, 0, sizeof(ma)));
    const uint32_t grid = 1u, block = 1u;
    RT_CHECK(vx_start_g_affine(device, krnl_buffer, master_arg_buffer,
                               1, &grid, &block, 0, VORTEX_AFFINITY_ANY));
    RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));

    RT_CHECK(query_phase_metrics(device, (uint32_t)num_cores, true, &results[m]));
    emit_csv(modes[m].name, results[m]);

    std::cout << "  cores (dst[1] tag): ";
    for (uint32_t p = 0; p < MESH_L2_NUM_PARTITIONS; ++p) {
      uint32_t core_tag = 0;
      const uint64_t tag_off =
          (dst_dev[p] - dev_mem_base) + mesh_l2_elem_byte_offset(1u);
      RT_CHECK(vx_copy_from_dev(&core_tag, mem_buffer, tag_off, sizeof(core_tag)));
      std::cout << "p" << p << "=" << core_tag << " ";
    }
    std::cout << "\n";
  }

  std::cout << "\n======== MESH_L2_COMPARISON ========\n"
            << std::left << std::setw(20) << "Mode"
            << std::setw(18) << "LOAD_LT (sum)"
            << std::setw(16) << "MCYCLE (max)"
            << std::setw(14) << "L2_READS"
            << "\n" << std::string(68, '-') << "\n";
  for (int m = 0; m < 3; ++m) {
    std::cout << std::setw(20) << modes[m].name
              << std::setw(18) << results[m].sum_load_latency
              << std::setw(16) << results[m].max_cycle
              << std::setw(14) << results[m].l2_reads << "\n";
  }
  std::cout << std::string(70, '-') << "\n";

  if (results[2].max_cycle > results[0].max_cycle) {
    const double gap = 100.0 *
        static_cast<double>(results[2].max_cycle - results[0].max_cycle) /
        static_cast<double>(results[0].max_cycle);
    std::cout << "HOME vs REMOTE max-cycle gap: " << std::fixed
              << std::setprecision(1) << gap << "%\n";
  }

  int errors = 0;
  if (verify_result) {
    for (uint32_t p = 0; p < MESH_L2_NUM_PARTITIONS; ++p) {
      std::vector<float> dev_dst(MESH_L2_FLOATS_PER_PARTITION);
      const uint64_t part_off = dst_dev[p] - dev_mem_base;
      for (uint32_t g = 0; g < MESH_L2_NUM_GRANULES; ++g) {
        const uint32_t elem0 = g * MESH_L2_FLOATS_PER_GRANULE;
        const uint64_t byte_off =
            part_off + mesh_l2_elem_byte_offset(elem0);
        RT_CHECK(vx_copy_from_dev(dev_dst.data() + elem0,
                                 mem_buffer,
                                 byte_off,
                                 MESH_L2_FLOATS_PER_GRANULE * sizeof(float)));
      }
      for (uint32_t i = 0; i < MESH_L2_FLOATS_PER_PARTITION; ++i) {
        if (i == 1)
          continue; /* elem 1 holds core_id tag from kernel */
        const float ref = host_src[p * MESH_L2_FLOATS_PER_PARTITION + i];
        const float got = dev_dst[i];
        const float tol = std::fmax(1e-4f, std::fabs(ref) * 1e-3f);
        if (std::fabs(got - ref) > tol) {
          if (errors < 8) {
            std::cout << "verify mismatch p=" << p << " i=" << i
                      << " got=" << got << " ref=" << ref << "\n";
          }
          ++errors;
        }
      }
    }
  }

  cleanup();
  if (errors != 0) {
    std::cout << "Found " << errors << " errors!\nFAILED!\n";
    return 1;
  }
  std::cout << "PASSED!\n";
  return 0;
}
