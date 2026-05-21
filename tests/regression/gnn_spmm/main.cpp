// SpMM host driver with device-side KMU dispatch: one vx_start_g_affine per phase
// runs a master CTA that issues 64 child SpMM grids inside a single
// processor.run(), so L1/L2 state persists across mini-partitions.
//
// Three phases (HOME / ANY / REMOTE) mirror mesh_l2_affinity: HOME pins each mini
// to mini_partition_core_affinity[i], REMOTE uses (home+2)%num_cores, ANY lets
// KMU schedule freely. Use CONFIGS=-DL2_ENABLE -DL2_MESH_ENABLE and --l2mesh for
// L2 Manhattan hop delay; VORTEX_MESH_STATS=1 prints hop histograms per phase.

#include <iostream>
#include <iomanip>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <vortex.h>
#include <VX_types.h>
#include "common.h"
#include "../../../gnn_data.h"

#define RT_CHECK(_expr)                                          \
  do {                                                           \
    int _ret = (_expr);                                          \
    if (0 == _ret) break;                                        \
    std::printf("Error: '%s' returned %d!\n", #_expr, (int)_ret);\
    cleanup();                                                   \
    std::exit(-1);                                               \
  } while (false)

static_assert(GNN_FEATURE_DIM == GNN_SPMM_FEATURE_DIM,
              "kernel feature dim must match the generated data");

const char* kernel_file = "kernel.vxbin";
bool verify_result = true;

vx_device_h device = nullptr;
vx_buffer_h krnl_buffer = nullptr;
vx_buffer_h row_ptr_buffer = nullptr;
vx_buffer_h col_ind_buffer = nullptr;
vx_buffer_h features_in_buffer = nullptr;
vx_buffer_h features_out_buffer = nullptr;
vx_buffer_h mini_start_buffer = nullptr;
vx_buffer_h mini_end_buffer = nullptr;
vx_buffer_h mini_aff_buffer = nullptr;
vx_buffer_h child_pool_buffer = nullptr;
vx_buffer_h master_arg_buffer = nullptr;

static void show_usage() {
  std::cout << "Vortex GNN SpMM (Core Affinity demo)\n";
  std::cout << "Usage: gnn_spmm [-k kernel.vxbin] [-V (skip verify)] [-h]\n";
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
  if (master_arg_buffer)   vx_mem_free(master_arg_buffer);
  if (child_pool_buffer)   vx_mem_free(child_pool_buffer);
  if (mini_aff_buffer)     vx_mem_free(mini_aff_buffer);
  if (mini_end_buffer)    vx_mem_free(mini_end_buffer);
  if (mini_start_buffer)  vx_mem_free(mini_start_buffer);
  if (features_out_buffer) vx_mem_free(features_out_buffer);
  if (features_in_buffer)  vx_mem_free(features_in_buffer);
  if (col_ind_buffer)      vx_mem_free(col_ind_buffer);
  if (row_ptr_buffer)      vx_mem_free(row_ptr_buffer);
  if (krnl_buffer)         vx_mem_free(krnl_buffer);
  vx_dev_close(device);
}

struct CacheMetrics {
  std::vector<uint64_t> reads;
  std::vector<uint64_t> writes;
  std::vector<uint64_t> read_misses;
  std::vector<uint64_t> write_misses;
  std::vector<uint64_t> total_reqs;
  std::vector<uint64_t> total_misses;
  std::vector<double>   hit_rate;
  std::vector<uint64_t> xbar_bytes;
  std::vector<uint64_t> cycles;
};

static int query_cache_metrics(vx_device_h dev,
                               uint32_t num_cores,
                               uint64_t l1_line_size,
                               CacheMetrics* out) {
  out->reads.assign(num_cores, 0);
  out->writes.assign(num_cores, 0);
  out->read_misses.assign(num_cores, 0);
  out->write_misses.assign(num_cores, 0);
  out->total_reqs.assign(num_cores, 0);
  out->total_misses.assign(num_cores, 0);
  out->hit_rate.assign(num_cores, 0.0);
  out->xbar_bytes.assign(num_cores, 0);
  out->cycles.assign(num_cores, 0);

  for (uint32_t c = 0; c < num_cores; ++c) {
    if (vx_mpm_query(dev, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_DCACHE_READS,  c, &out->reads[c]) != 0) return -1;
    if (vx_mpm_query(dev, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_DCACHE_WRITES, c, &out->writes[c]) != 0) return -1;
    if (vx_mpm_query(dev, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_DCACHE_MISS_R, c, &out->read_misses[c]) != 0) return -1;
    if (vx_mpm_query(dev, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_DCACHE_MISS_W, c, &out->write_misses[c]) != 0) return -1;
    if (vx_mpm_query(dev, VX_DCR_MPM_CLASS_BASE, VX_CSR_MCYCLE, c, &out->cycles[c]) != 0) return -1;

    uint64_t reqs   = out->reads[c] + out->writes[c];
    uint64_t misses = out->read_misses[c] + out->write_misses[c];
    out->total_reqs[c]   = reqs;
    out->total_misses[c] = misses;
    out->hit_rate[c]     = (reqs == 0) ? 0.0 : (1.0 - (double)misses / (double)reqs);
    out->xbar_bytes[c]   = misses * l1_line_size;
  }
  return 0;
}

static uint64_t sum64(const std::vector<uint64_t>& v) {
  uint64_t s = 0;
  for (auto x : v) s += x;
  return s;
}

static uint64_t max64(const std::vector<uint64_t>& v) {
  uint64_t m = 0;
  for (auto x : v) if (x > m) m = x;
  return m;
}

struct L2Metrics {
  uint64_t reads      = 0;
  uint64_t writes    = 0;
  uint64_t read_miss  = 0;
  uint64_t write_miss = 0;
};

// One L2 counter block per cluster (MPM is shared per cluster; query rep core).
static int query_l2_per_cluster(vx_device_h dev,
                                uint32_t num_cores_u,
                                uint32_t num_clusters_u,
                                bool l2_en,
                                std::vector<L2Metrics>* out) {
  out->assign(num_clusters_u, L2Metrics{});
  if (!l2_en || num_clusters_u == 0 || num_cores_u == 0)
    return 0;
  const uint32_t cores_per_cluster =
      (num_cores_u + num_clusters_u - 1u) / num_clusters_u;
  for (uint32_t cl = 0; cl < num_clusters_u; ++cl) {
    const uint32_t rep_core = cl * cores_per_cluster;
    if (rep_core >= num_cores_u)
      continue;
    L2Metrics& L = (*out)[cl];
    if (vx_mpm_query(dev, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_L2CACHE_READS,
                     rep_core, &L.reads) != 0)
      return -1;
    if (vx_mpm_query(dev, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_L2CACHE_WRITES,
                     rep_core, &L.writes) != 0)
      return -1;
    if (vx_mpm_query(dev, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_L2CACHE_MISS_R,
                     rep_core, &L.read_miss) != 0)
      return -1;
    if (vx_mpm_query(dev, VX_DCR_MPM_CLASS_MEM, VX_CSR_MPM_L2CACHE_MISS_W,
                     rep_core, &L.write_miss) != 0)
      return -1;
  }
  return 0;
}

struct PhaseTotals {
  uint64_t l1_miss = 0;
  uint64_t l1_xbar = 0;
  uint64_t l2_reads = 0;
  uint64_t l2_read_miss = 0;
  uint64_t l2_write_miss = 0;
  uint64_t max_cycle = 0;
};

struct L1HitDispersion {
  double mean_pct = 0.0;
  double stddev_pct = 0.0;
  double min_pct = 0.0;
  double max_pct = 0.0;
  uint32_t n_cores = 0;
};

static L1HitDispersion compute_l1_hit_dispersion(const CacheMetrics& m,
                                                 uint32_t num_cores) {
  L1HitDispersion d{};
  std::vector<double> hr;
  hr.reserve(num_cores);
  for (uint32_t c = 0; c < num_cores; ++c) {
    if (m.total_reqs[c] == 0)
      continue;
    hr.push_back(m.hit_rate[c] * 100.0);
  }
  d.n_cores = (uint32_t)hr.size();
  if (hr.empty())
    return d;
  d.min_pct = *std::min_element(hr.begin(), hr.end());
  d.max_pct = *std::max_element(hr.begin(), hr.end());
  d.mean_pct = std::accumulate(hr.begin(), hr.end(), 0.0) / (double)hr.size();
  double var = 0.0;
  for (double x : hr) {
    const double t = x - d.mean_pct;
    var += t * t;
  }
  d.stddev_pct = std::sqrt(var / (double)hr.size());
  return d;
}

static void emit_l1_per_core_csv(const char* phase,
                                 uint32_t num_cores,
                                 uint64_t l1_line_size,
                                 const CacheMetrics& m) {
  for (uint32_t c = 0; c < num_cores; ++c) {
    std::printf(
        "GNN_CSV,phase=%s,scope=L1_PER_CORE,core=%u,L1_DCACHE_READS=%lu,"
        "L1_DCACHE_WRITES=%lu,L1_DCACHE_MISSES=%lu,HIT_RATE_PCT=%.4f,"
        "CROSSBAR_TRAFFIC_BYTES=%lu,MCYCLE=%lu\n",
        phase, c,
        (unsigned long)m.reads[c],
        (unsigned long)m.writes[c],
        (unsigned long)m.total_misses[c],
        m.hit_rate[c] * 100.0,
        (unsigned long)m.xbar_bytes[c],
        (unsigned long)m.cycles[c]);
  }
  (void)l1_line_size;
}

static void emit_l2_cluster_csv(const char* phase,
                                const std::vector<L2Metrics>& by_cluster) {
  for (uint32_t cl = 0; cl < by_cluster.size(); ++cl) {
    const L2Metrics& L = by_cluster[cl];
    std::printf(
        "GNN_CSV,phase=%s,scope=L2_CLUSTER,cluster=%u,L2_READS=%lu,L2_WRITES=%lu,"
        "L2_READ_MISS=%lu,L2_WRITE_MISS=%lu\n",
        phase, cl,
        (unsigned long)L.reads,
        (unsigned long)L.writes,
        (unsigned long)L.read_miss,
        (unsigned long)L.write_miss);
  }
}

static void emit_l1_dispersion_csv(const char* phase, const L1HitDispersion& d) {
  std::printf(
      "GNN_CSV,phase=%s,scope=L1_HIT_DISPERSION,N_CORES_WITH_REQS=%u,"
      "MEAN_HIT_RATE_PCT=%.4f,STDDEV_HIT_RATE_PCT=%.4f,MIN_HIT_RATE_PCT=%.4f,"
      "MAX_HIT_RATE_PCT=%.4f\n",
      phase,
      (unsigned)d.n_cores,
      d.mean_pct,
      d.stddev_pct,
      d.min_pct,
      d.max_pct);
}

static void emit_phase_totals_csv(const char* phase,
                                  uint32_t num_cores,
                                  uint64_t l1_line_size,
                                  const PhaseTotals& t) {
  std::printf(
      "GNN_CSV,phase=%s,scope=PHASE_TOTAL,core=ALL,L1_DCACHE_MISSES=%lu,"
      "CROSSBAR_TRAFFIC_BYTES=%lu,L2_READS=%lu,L2_READ_MISS=%lu,L2_WRITE_MISS=%lu,"
      "CYCLES_MAX_CORE=%lu\n",
      phase,
      (unsigned long)t.l1_miss,
      (unsigned long)t.l1_xbar,
      (unsigned long)t.l2_reads,
      (unsigned long)t.l2_read_miss,
      (unsigned long)t.l2_write_miss,
      (unsigned long)t.max_cycle);
  (void)num_cores;
  (void)l1_line_size;
}

struct PhaseSnapshot {
  CacheMetrics        l1_per_core;
  std::vector<L2Metrics> l2_per_cluster;
  PhaseTotals         totals;
  L1HitDispersion     dispersion;
};

static void compute_reference(std::vector<float>& out) {
  out.assign(GNN_NUM_NODES * GNN_FEATURE_DIM, 0.0f);
  for (uint32_t v = 0; v < GNN_NUM_NODES; ++v) {
    uint32_t s = row_ptr[v];
    uint32_t e = row_ptr[v + 1];
    float acc[GNN_FEATURE_DIM] = {0.0f};
    for (uint32_t k = s; k < e; ++k) {
      uint32_t n = col_ind[k];
      const float* nf = &node_features[n * GNN_FEATURE_DIM];
      for (uint32_t f = 0; f < GNN_FEATURE_DIM; ++f) acc[f] += nf[f];
    }
    for (uint32_t f = 0; f < GNN_FEATURE_DIM; ++f) {
      out[v * GNN_FEATURE_DIM + f] = acc[f];
    }
  }
}

int main(int argc, char** argv) {
  parse_args(argc, argv);

  std::cout << "open device" << std::endl;
  RT_CHECK(vx_dev_open(&device));

  uint64_t num_cores = 0;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_CORES,       &num_cores));
  uint64_t num_threads = 0, num_warps = 0;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_THREADS,     &num_threads));
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_WARPS,       &num_warps));
  uint64_t l1_line_size = 0;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_CACHE_LINE_SIZE, &l1_line_size));
  uint64_t isa_flags = 0;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_ISA_FLAGS, &isa_flags));
  const bool l2_enabled = (isa_flags & VX_ISA_EXT_L2CACHE) != 0;
  uint64_t num_clusters = 1;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_CLUSTERS, &num_clusters));

  std::cout << "config: num_cores=" << num_cores
            << ", num_clusters=" << num_clusters
            << ", num_warps=" << num_warps
            << ", num_threads=" << num_threads
            << ", l1_line_size=" << l1_line_size << "B"
            << ", l2_cache=" << (l2_enabled ? "on" : "off") << std::endl;

  if (!l2_enabled) {
    std::cerr
        << "Note: L2 not present in this ISA config. Rebuild/run with "
           "CONFIGS=-DL2_ENABLE -DL2_MESH_ENABLE or "
           "./ci/blackbox.sh --l2cache --l2mesh ... for GNN_CSV L2_CLUSTER rows.\n";
  }
  if (std::getenv("VORTEX_MESH_STATS") != nullptr && !l2_enabled) {
    std::cerr << "Warning: VORTEX_MESH_STATS set but L2 is disabled.\n";
  }

  const uint32_t num_minis = GNN_NUM_MINI_PARTITIONS;
  if (num_cores != GNN_NUM_PHYSICAL_CORES) {
    std::cerr << "Warning: simulator num_cores=" << num_cores
              << " != GNN_NUM_PHYSICAL_CORES=" << GNN_NUM_PHYSICAL_CORES
              << " (affinity table modulo num_cores is applied).\n";
  }

  const uint64_t row_ptr_bytes      = sizeof(row_ptr);
  const uint64_t col_ind_bytes      = sizeof(col_ind);
  const uint64_t features_in_bytes  = sizeof(node_features);
  const uint64_t features_out_bytes = (uint64_t)GNN_NUM_NODES
                                    * GNN_FEATURE_DIM * sizeof(float);
  const uint64_t mini_tbl_bytes     = (uint64_t)num_minis * sizeof(uint32_t);
  const uint64_t child_pool_bytes   = (uint64_t)num_minis * sizeof(kernel_arg_t);

  std::cout << "allocate device memory" << std::endl;
  RT_CHECK(vx_mem_alloc(device, row_ptr_bytes,      VX_MEM_READ,       &row_ptr_buffer));
  RT_CHECK(vx_mem_alloc(device, col_ind_bytes,      VX_MEM_READ,       &col_ind_buffer));
  RT_CHECK(vx_mem_alloc(device, features_in_bytes,  VX_MEM_READ,       &features_in_buffer));
  RT_CHECK(vx_mem_alloc(device, features_out_bytes, VX_MEM_READ_WRITE, &features_out_buffer));
  RT_CHECK(vx_mem_alloc(device, mini_tbl_bytes,     VX_MEM_READ,       &mini_start_buffer));
  RT_CHECK(vx_mem_alloc(device, mini_tbl_bytes,     VX_MEM_READ,       &mini_end_buffer));
  RT_CHECK(vx_mem_alloc(device, mini_tbl_bytes,     VX_MEM_READ,       &mini_aff_buffer));
  RT_CHECK(vx_mem_alloc(device, child_pool_bytes,   VX_MEM_READ_WRITE, &child_pool_buffer));
  RT_CHECK(vx_mem_alloc(device, sizeof(master_arg_t), VX_MEM_READ_WRITE, &master_arg_buffer));

  uint64_t row_ptr_addr = 0, col_ind_addr = 0;
  uint64_t features_in_addr = 0, features_out_addr = 0;
  uint64_t mini_start_addr = 0, mini_end_addr = 0, mini_aff_addr = 0;
  uint64_t child_pool_addr = 0, master_arg_addr = 0;
  RT_CHECK(vx_mem_address(row_ptr_buffer,       &row_ptr_addr));
  RT_CHECK(vx_mem_address(col_ind_buffer,       &col_ind_addr));
  RT_CHECK(vx_mem_address(features_in_buffer,   &features_in_addr));
  RT_CHECK(vx_mem_address(features_out_buffer,  &features_out_addr));
  RT_CHECK(vx_mem_address(mini_start_buffer,    &mini_start_addr));
  RT_CHECK(vx_mem_address(mini_end_buffer,      &mini_end_addr));
  RT_CHECK(vx_mem_address(mini_aff_buffer,      &mini_aff_addr));
  RT_CHECK(vx_mem_address(child_pool_buffer,    &child_pool_addr));
  RT_CHECK(vx_mem_address(master_arg_buffer,    &master_arg_addr));

  std::cout << "upload graph + features + mini-partition tables" << std::endl;
  RT_CHECK(vx_copy_to_dev(row_ptr_buffer,     row_ptr,        0, row_ptr_bytes));
  RT_CHECK(vx_copy_to_dev(col_ind_buffer,     col_ind,        0, col_ind_bytes));
  RT_CHECK(vx_copy_to_dev(features_in_buffer, node_features,  0, features_in_bytes));
  RT_CHECK(vx_copy_to_dev(mini_start_buffer,  mini_partition_start_node, 0, mini_tbl_bytes));
  RT_CHECK(vx_copy_to_dev(mini_end_buffer,    mini_partition_end_node,   0, mini_tbl_bytes));
  RT_CHECK(vx_copy_to_dev(mini_aff_buffer,    mini_partition_core_affinity, 0, mini_tbl_bytes));

  std::vector<kernel_arg_t> child_pool(num_minis);
  for (uint32_t i = 0; i < num_minis; ++i) {
    child_pool[i] = {};
    child_pool[i].entry_kind        = GNN_ENTRY_SPMM;
    child_pool[i].row_ptr_addr      = row_ptr_addr;
    child_pool[i].col_ind_addr      = col_ind_addr;
    child_pool[i].features_in_addr  = features_in_addr;
    child_pool[i].features_out_addr = features_out_addr;
    child_pool[i].start_node        = mini_partition_start_node[i];
    child_pool[i].end_node          = mini_partition_end_node[i];
  }
  RT_CHECK(vx_copy_to_dev(child_pool_buffer, child_pool.data(), 0, child_pool_bytes));

  std::cout << "upload kernel" << std::endl;
  RT_CHECK(vx_upload_kernel_file(device, kernel_file, &krnl_buffer));
  uint64_t kernel_pc = 0;
  RT_CHECK(vx_mem_address(krnl_buffer, &kernel_pc));

  const uint32_t child_block_x = (uint32_t)(num_warps * num_threads);
  const uint32_t master_grid_dim = 1u;
  const uint32_t master_block_dim = 1u;

  auto run_master_phase = [&](const char* phase_name, uint32_t affinity_mode,
                              PhaseSnapshot* snap_out) {
    RT_CHECK(vx_perf_reset(device));

    master_arg_t ma{};
    ma.entry_kind                   = GNN_ENTRY_MASTER;
    ma.mini_partition_start_addr    = mini_start_addr;
    ma.mini_partition_end_addr      = mini_end_addr;
    ma.mini_partition_affinity_addr = mini_aff_addr;
    ma.child_arg_pool_addr          = child_pool_addr;
    ma.kernel_pc                    = kernel_pc;
    ma.num_minis                    = num_minis;
    ma.affinity_mode                = affinity_mode;
    ma.child_block_x                = child_block_x;
    ma.num_cores                    = (uint32_t)num_cores;

    RT_CHECK(vx_copy_to_dev(master_arg_buffer, &ma, 0, sizeof(ma)));

    RT_CHECK(vx_start_g_affine(device, krnl_buffer, master_arg_buffer,
                               1, &master_grid_dim, &master_block_dim, 0,
                               VORTEX_AFFINITY_ANY));
    RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));

    CacheMetrics m;
    RT_CHECK(query_cache_metrics(device, (uint32_t)num_cores,
                                 l1_line_size, &m));

    std::vector<L2Metrics> l2_cl;
    RT_CHECK(query_l2_per_cluster(device, (uint32_t)num_cores,
                                  (uint32_t)num_clusters, l2_enabled, &l2_cl));

    PhaseTotals t{};
    t.l1_miss   = sum64(m.total_misses);
    t.l1_xbar   = sum64(m.xbar_bytes);
    t.max_cycle = max64(m.cycles);
    for (const auto& L : l2_cl) {
      t.l2_reads      += L.reads;
      t.l2_read_miss  += L.read_miss;
      t.l2_write_miss += L.write_miss;
    }

    L1HitDispersion disp = compute_l1_hit_dispersion(m, (uint32_t)num_cores);

    emit_l1_per_core_csv(phase_name, (uint32_t)num_cores, l1_line_size, m);
    if (l2_enabled)
      emit_l2_cluster_csv(phase_name, l2_cl);
    else
      std::printf("GNN_CSV,phase=%s,scope=L2,L2_PRESENT=0\n", phase_name);

    emit_l1_dispersion_csv(phase_name, disp);
    emit_phase_totals_csv(phase_name, (uint32_t)num_cores, l1_line_size, t);

    snap_out->l1_per_core     = m;
    snap_out->l2_per_cluster  = std::move(l2_cl);
    snap_out->totals          = t;
    snap_out->dispersion      = disp;
  };

  struct ModeSpec {
    const char* name;
    uint32_t mode;
  };
  const ModeSpec modes[] = {
      {"home", GNN_AFFINITY_HOME},
      {"any", GNN_AFFINITY_ANY},
      {"remote", GNN_AFFINITY_REMOTE},
  };
  PhaseSnapshot snaps[3]{};

  for (int m = 0; m < 3; ++m) {
    std::cout << "=== Run: " << modes[m].name
              << " (KMU affinity, " << num_minis << " child launches) ==="
              << std::endl;
    run_master_phase(modes[m].name, modes[m].mode, &snaps[m]);
  }

  std::cout << "\n"
            << "======== GNN_SPMM_COMPARISON (single processor.run() per phase, "
            << num_minis << " device launches) ========\n"
            << std::left << std::setw(20) << "Mode"
            << std::setw(18) << "L1_MISS"
            << std::setw(18) << "L1_XBAR_B"
            << std::setw(14) << "L2_READS"
            << std::setw(14) << "L2_RD_MISS"
            << std::setw(16) << "MCYCLE_MAX"
            << "\n"
            << std::string(100, '-') << "\n";
  for (int m = 0; m < 3; ++m) {
    const PhaseTotals& t = snaps[m].totals;
    std::cout << std::setw(20) << modes[m].name
              << std::setw(18) << t.l1_miss
              << std::setw(18) << t.l1_xbar
              << std::setw(14) << t.l2_reads
              << std::setw(14) << t.l2_read_miss
              << std::setw(16) << t.max_cycle << "\n";
  }
  std::cout << std::string(100, '-') << "\n"
            << "--- L1 hit-rate dispersion (stddev % across cores) ---\n";
  for (int m = 0; m < 3; ++m) {
    std::cout << std::setw(20) << modes[m].name
              << std::fixed << std::setprecision(4)
              << " stddev=" << snaps[m].dispersion.stddev_pct
              << " mean=" << snaps[m].dispersion.mean_pct << "\n";
  }
  if (snaps[2].totals.max_cycle > snaps[0].totals.max_cycle) {
    const double gap =
        100.0 *
        static_cast<double>(snaps[2].totals.max_cycle - snaps[0].totals.max_cycle) /
        static_cast<double>(snaps[0].totals.max_cycle);
    std::cout << "HOME vs REMOTE MCYCLE max gap: " << std::fixed
              << std::setprecision(1) << gap << "%\n";
  }
  std::cout << std::string(100, '-') << "\n";
  std::cout << "(Set VORTEX_MESH_STATS=1 with --l2mesh for L2 hop histograms on stderr.)\n";

  int errors = 0;
  if (verify_result) {
    std::cout << "verify result" << std::endl;
    std::vector<float> dev_out(GNN_NUM_NODES * GNN_FEATURE_DIM, 0.0f);
    RT_CHECK(vx_copy_from_dev(dev_out.data(), features_out_buffer, 0,
                              features_out_bytes));

    std::vector<float> ref;
    compute_reference(ref);

    int reported = 0;
    for (uint32_t i = 0; i < dev_out.size(); ++i) {
      float a = dev_out[i], b = ref[i];
      float diff = std::fabs(a - b);
      float tol  = std::fmax(1e-3f, std::fabs(b) * 1e-3f);
      if (diff > tol) {
        if (reported < 8) {
          std::cout << "  mismatch idx=" << i << " got=" << a << " ref=" << b
                    << " diff=" << diff << std::endl;
          ++reported;
        }
        ++errors;
      }
    }
  }

  std::cout << "cleanup" << std::endl;
  cleanup();

  if (errors != 0) {
    std::cout << "Found " << errors << " errors!\nFAILED!" << std::endl;
    return 1;
  }
  std::cout << "PASSED!" << std::endl;
  return 0;
}
