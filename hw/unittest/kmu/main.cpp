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

#include "vl_simulator.h"
#include "VVX_kmu_top.h"
#include <iostream>
#include "VX_config.h"
#include "VX_types.h"
#include <cassert>

#ifndef TRACE_START_TIME
#define TRACE_START_TIME 0ull
#endif

#ifndef TRACE_STOP_TIME
#define TRACE_STOP_TIME -1ull
#endif

static uint64_t timestamp = 0;
static bool trace_enabled = false;
static uint64_t trace_start_time = TRACE_START_TIME;
static uint64_t trace_stop_time  = TRACE_STOP_TIME;

double sc_time_stamp() { 
  return timestamp;
}

bool sim_trace_enabled() {
  if (timestamp >= trace_start_time 
   && timestamp < trace_stop_time)
    return true;
  return trace_enabled;
}

void sim_trace_enable(bool enable) {
  trace_enabled = enable;
}

template <typename T>
int write_dcr(vl_simulator<T>& sim, const int addr, const int value, int tick) {
    sim->dcr_wr_valid = 1;
    sim->dcr_wr_addr = addr;
    sim->dcr_wr_data = value;

    tick = sim.step(tick, 2);
    sim->dcr_wr_valid = 0;

    return tick;
}

int main(int argc, char **argv) {
  // Initialize Verilators variables
  Verilated::commandArgs(argc, argv);

  vl_simulator<VVX_kmu_top> sim;
  int tick = 0;

  int dummy_pc = 0x12345678;
  int dummy_param = 0x87654321;
  int dummy_grid[3] = {1, 2, 3};
  int dummy_block[3] = {4, 5, 6};

//   sim->cp_valid = 0;


    tick = write_dcr(sim, VX_DCR_BASE_STARTUP_ADDR0, dummy_pc, tick);
    tick = write_dcr(sim, VX_DCR_BASE_GRID_DIM0, dummy_grid[0], tick);
    tick = write_dcr(sim, VX_DCR_BASE_GRID_DIM1, dummy_grid[1], tick);
    tick = write_dcr(sim, VX_DCR_BASE_GRID_DIM2, dummy_grid[2], tick);
    tick = write_dcr(sim, VX_DCR_BASE_BLOCK_DIM0, dummy_block[0], tick);
    tick = write_dcr(sim, VX_DCR_BASE_BLOCK_DIM1, dummy_block[1], tick);
    tick = write_dcr(sim, VX_DCR_BASE_BLOCK_DIM2, dummy_block[2], tick);
    tick = write_dcr(sim, VX_DCR_BASE_STARTUP_ARG0, dummy_param, tick);

    tick = sim.reset(tick);

    assert(sim->pc == dummy_pc);
    assert(sim->param == dummy_param);
    assert(sim->grid_dim[0] == dummy_grid[0]);
    assert(sim->grid_dim[1] == dummy_grid[1]);
    assert(sim->grid_dim[2] == dummy_grid[2]);
    assert(sim->block_dim[0] == dummy_block[0]);
    assert(sim->block_dim[1] == dummy_block[1]);
    assert(sim->block_dim[2] == dummy_block[2]);

//   sim->cp_valid = 1;

  tick = sim.step(tick, 2);
//   sim->cp_valid = 0;
  tick = sim.step(tick, 2);

  return 0;
}