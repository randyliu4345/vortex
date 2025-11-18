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

#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include <fstream>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include <cstring>
#include "processor.h"
#include "mem.h"
#include "constants.h"
#include <util.h>
#include "core.h"
#include "VX_types.h"
#include "debug_module.h"
#include "jtag_dtm.h"
#include "remote_bitbang.h"

using namespace vortex;

static void show_usage() {
   std::cout << "Usage: [-c <cores>] [-w <warps>] [-t <threads>] [-v: vector-test] [-s: stats] [-h: help] [--rbb-server=<port>] <program>" << std::endl;
}

uint32_t num_threads = NUM_THREADS;
uint32_t num_warps = NUM_WARPS;
uint32_t num_cores = NUM_CORES;
bool showStats = false;
bool vector_test = false;
const char* program = nullptr;
uint16_t rbb_server_port = 0;
volatile bool keep_running = true;

// Global debug components
DebugModule *g_dm = nullptr;
jtag_dtm_t *g_dtm = nullptr;
remote_bitbang_t *g_rbb = nullptr;

void handle_sigint(int sig) {
  (void)sig;
  keep_running = false;
}

static void parse_args(int argc, char **argv) {
  // First, check for --rbb-server option and environment variable
  for (int i = 1; i < argc; i++) {
    if (strncmp(argv[i], "--rbb-server=", 13) == 0) {
      rbb_server_port = atoi(argv[i] + 13);
      // Remove this argument by shifting remaining args
      for (int j = i; j < argc - 1; j++) {
        argv[j] = argv[j + 1];
      }
      argc--;
      i--;
    }
  }
  
  // Check environment variable
  const char *env_rbb = getenv("VORTEX_RBB_SERVER");
  if (env_rbb != nullptr && rbb_server_port == 0) {
    rbb_server_port = atoi(env_rbb);
  }
  
  // Reset optind for getopt
  optind = 1;
  
  	int c;
  	while ((c = getopt(argc, argv, "t:w:c:vsh")) != -1) {
    	switch (c) {
      case 't':
        num_threads = atoi(optarg);
        break;
      case 'w':
        num_warps = atoi(optarg);
        break;
		  case 'c':
        num_cores = atoi(optarg);
        break;
      case 'v':
        vector_test = true;
        break;
      case 's':
        showStats = true;
        break;
    	case 'h':
      	show_usage();
      	exit(0);
    		break;
    	default:
      	show_usage();
      	exit(-1);
    	}
	}

	if (optind < argc) {
		program = argv[optind];
    std::cout << "Running " << program << "..." << std::endl;
	} else {
		show_usage();
    exit(-1);
	}
}

int main(int argc, char **argv) {
  int exitcode = 0;

  parse_args(argc, argv);

  // Set up signal handler for clean shutdown
  signal(SIGINT, handle_sigint);

  // Initialize RBB server if requested
  if (rbb_server_port > 0) {
    g_dm = new DebugModule();
    g_dtm = new jtag_dtm_t(g_dm);
    g_rbb = new remote_bitbang_t(rbb_server_port, g_dtm);
    std::cout << "Remote bitbang server started on port " << rbb_server_port << std::endl;
  }

  {
    // create processor configuation
    Arch arch(num_threads, num_warps, num_cores);

    // create memory module
    RAM ram(0, MEM_PAGE_SIZE);

    // create processor
    Processor processor(arch);

    // attach memory module
    processor.attach_ram(&ram);

	  // setup base DCRs
    const uint64_t startup_addr(STARTUP_ADDR);
    processor.dcr_write(VX_DCR_BASE_STARTUP_ADDR0, startup_addr & 0xffffffff);
  #if (XLEN == 64)
    processor.dcr_write(VX_DCR_BASE_STARTUP_ADDR1, startup_addr >> 32);
  #endif
	  processor.dcr_write(VX_DCR_BASE_MPM_CLASS, 0);

    // load program
    {
      std::string program_ext(fileExtension(program));
      if (program_ext == "bin") {
        ram.loadBinImage(program, startup_addr);
      } else if (program_ext == "hex") {
        ram.loadHexImage(program);
      } else {
        std::cerr << "Error: only *.bin or *.hex images supported." << std::endl;
        return -1;
      }
    }
  #ifndef NDEBUG
    std::cout << "[VXDRV] START: program=" << program << std::endl;
  #endif
    // run simulation
  #ifdef EXT_V_ENABLE
    // vector test exitcode is a special case
    if (vector_test) {
      exitcode = (processor.run() != 1) ? 1 : 0;
    } else
  #endif
    {
      // Custom run loop with RBB server integration
      processor.initialize();
      bool done = false;
      while (!done && keep_running) {
        // Process incoming JTAG commands from OpenOCD (if RBB server is enabled)
        if (g_rbb != nullptr) {
          g_rbb->tick();
        }

        // Check if debug module has halted the CPU
        bool halted = (g_dm != nullptr && g_dm->is_halted());

        if (!halted) {
          // CPU is running - advance simulation
          processor.tick();
          done = processor.is_done();
          if (done) {
            exitcode = processor.get_exitcode();
          }
        } else {
          // CPU is halted - only service debug commands, don't advance simulation
          // Small sleep to avoid busy-waiting
          usleep(100);
        }
      }

      // If we exited due to keep_running being false, read exitcode
      if (!keep_running && !done) {
        ram.read(&exitcode, (IO_MPM_ADDR + 8), 4);
      }
    }
  }

  // Cleanup RBB server
  if (g_rbb != nullptr) {
    delete g_rbb;
    g_rbb = nullptr;
  }
  if (g_dtm != nullptr) {
    delete g_dtm;
    g_dtm = nullptr;
  }
  if (g_dm != nullptr) {
    delete g_dm;
    g_dm = nullptr;
  }

  return exitcode;
}
