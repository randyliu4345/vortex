//!/bin/bash

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

typedef struct packed {
    logic[31:0]          num_warps;
    logic[`XLEN-1:0]     start_pc;
    logic[`XLEN-1:0]     param;
    logic[31:0]          cta_x;
    logic[31:0]          cta_y;
    logic[31:0]          cta_z;
    logic[31:0]          cta_id;
    logic[`NUM_THREADS-1:0] remain_mask;
} kmu_req_data_t;

interface VX_kmu_bus_if #(
    parameter NUM_LANES = 1
) ();
    `UNUSED_PARAM(NUM_LANES);

    logic       req_valid;
    kmu_req_data_t  req_data;
    logic       req_ready;

    modport master (
        output req_valid,
        output req_data,
        input  req_ready
    );

    modport slave (
        input  req_valid,
        input  req_data,
        output req_ready
    );

endinterface
