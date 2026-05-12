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

#include <vx_e2e_eval.h>

#include <cerrno>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static std::mutex g_mu;
static uint64_t g_total_pcie_transactions = 0;
static uint64_t g_total_pcie_bytes = 0;
static uint64_t g_start_cycle = 0; // SimX window start (0 after reset)
static uint64_t g_end_cycle = 0;   // Cumulative SimPlatform cycles across runs in the window
static bool g_any_activity = false;

static int env_enabled() {
  const char* v = std::getenv("VORTEX_E2E_EVAL");
  return (v != nullptr && v[0] == '1' && v[1] == '\0');
}

static std::string eval_output_dir() {
  const char* d = std::getenv("VORTEX_E2E_EVAL_DIR");
  if (d != nullptr && d[0] != '\0')
    return std::string(d);
  return std::string("e2e_eval_output");
}

static int mkdir_p(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur.push_back(path[i]);
    if (path[i] == '/' && cur.size() > 1) {
      if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST)
        return -1;
    }
  }
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
    return -1;
  return 0;
}

int vx_e2e_eval_enabled(void) {
  return env_enabled() ? 1 : 0;
}

void vx_e2e_eval_reset(void) {
  if (!env_enabled())
    return;
  std::lock_guard<std::mutex> lock(g_mu);
  g_total_pcie_transactions = 0;
  g_total_pcie_bytes = 0;
  g_start_cycle = 0;
  g_end_cycle = 0;
  g_any_activity = false;
}

void vx_e2e_record_pcie_copy(uint64_t byte_count) {
  if (!env_enabled())
    return;
  std::lock_guard<std::mutex> lock(g_mu);
  g_any_activity = true;
  ++g_total_pcie_transactions;
  g_total_pcie_bytes += byte_count;
}

void vx_e2e_record_pcie_launch(void) {
  if (!env_enabled())
    return;
  std::lock_guard<std::mutex> lock(g_mu);
  g_any_activity = true;
  ++g_total_pcie_transactions;
}

void vx_e2e_on_sim_run_end(uint64_t run_sim_cycles) {
  if (!env_enabled())
    return;
  std::lock_guard<std::mutex> lock(g_mu);
  g_any_activity = true;
  g_end_cycle += run_sim_cycles;
}

static void write_csv_and_print_locked() {
  const uint64_t pure_gpu = (g_end_cycle >= g_start_cycle) ? (g_end_cycle - g_start_cycle) : 0;
  const uint64_t bw_cycles = g_total_pcie_bytes / PCIE_BANDWIDTH_LATENCY_BYTES;
  const uint64_t modeled_pcie = g_total_pcie_transactions * PCIE_FIXED_LATENCY + bw_cycles;
  const uint64_t total_e2e = pure_gpu + modeled_pcie;

  std::fprintf(stdout,
               "e2e_eval,start_cycle,%llu,end_cycle,%llu,pure_gpu_cycles,%llu,"
               "total_pcie_transactions,%llu,total_pcie_bytes,%llu,"
               "modeled_pcie_delay_cycles,%llu,total_e2e_cycles,%llu,"
               "PCIE_FIXED_LATENCY,%llu,PCIE_BANDWIDTH_LATENCY_BYTES,%llu\n",
               (unsigned long long)g_start_cycle,
               (unsigned long long)g_end_cycle,
               (unsigned long long)pure_gpu,
               (unsigned long long)g_total_pcie_transactions,
               (unsigned long long)g_total_pcie_bytes,
               (unsigned long long)modeled_pcie,
               (unsigned long long)total_e2e,
               (unsigned long long)PCIE_FIXED_LATENCY,
               (unsigned long long)PCIE_BANDWIDTH_LATENCY_BYTES);

  const std::string dir = eval_output_dir();
  if (mkdir_p(dir) != 0) {
    std::fprintf(stderr, "e2e_eval: could not create directory %s\n", dir.c_str());
    return;
  }

  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  char path[512];
  std::snprintf(path, sizeof(path), "%s/e2e_eval_%lld.csv", dir.c_str(),
                (long long)(ts.tv_sec * 1000000000LL + ts.tv_nsec));

  FILE* fp = std::fopen(path, "w");
  if (fp == nullptr) {
    std::fprintf(stderr, "e2e_eval: could not open %s for write\n", path);
    return;
  }
  std::fprintf(fp,
               "start_cycle,end_cycle,pure_gpu_cycles,total_pcie_transactions,total_pcie_bytes,"
               "modeled_pcie_delay_cycles,total_e2e_cycles,PCIE_FIXED_LATENCY,PCIE_BANDWIDTH_LATENCY_BYTES\n");
  std::fprintf(fp,
               "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
               (unsigned long long)g_start_cycle,
               (unsigned long long)g_end_cycle,
               (unsigned long long)pure_gpu,
               (unsigned long long)g_total_pcie_transactions,
               (unsigned long long)g_total_pcie_bytes,
               (unsigned long long)modeled_pcie,
               (unsigned long long)total_e2e,
               (unsigned long long)PCIE_FIXED_LATENCY,
               (unsigned long long)PCIE_BANDWIDTH_LATENCY_BYTES);
  std::fclose(fp);

  std::fprintf(stdout, "e2e_eval,wrote,%s\n", path);
}

void vx_e2e_eval_finalize(void) {
  if (!env_enabled())
    return;
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_any_activity)
    return;
  write_csv_and_print_locked();
}

namespace vortex {

void print_e2e_evaluation() {
  vx_e2e_eval_finalize();
}

} // namespace vortex
