#include <iostream>
#include <unistd.h>
#include <vortex.h>
#include <vector>
#include "common.h"

#define RT_CHECK(_expr)                                         \
   do {                                                         \
     int _ret = _expr;                                          \
     if (0 == _ret) break;                                      \
     printf("Error: '%s' returned %d!\n", #_expr, (int)_ret);   \
     cleanup(); exit(-1);                                       \
   } while (false)

const char* kernel_file = "kernel.vxbin";
uint32_t count = 0;
vx_device_h device = nullptr;
vx_buffer_h src_buffer = nullptr, dst_buffer = nullptr, krnl_buffer = nullptr, args_buffer = nullptr;
kernel_arg_t kernel_arg = {};

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "n:k:h")) != -1) {
    if (c == 'n') count = atoi(optarg);
    else if (c == 'k') kernel_file = optarg;
    else exit(0);
  }
}

void cleanup() {
  if (device) {
    vx_mem_free(src_buffer); vx_mem_free(dst_buffer);
    vx_mem_free(krnl_buffer); vx_mem_free(args_buffer);
    vx_dev_close(device);
  }
}

static int32_t ref_value(int32_t value, uint32_t task_id, const std::vector<int32_t>& src) {
  if (task_id > 1) { if (task_id > 2) value += 6; else value += 5; }
  else { if (task_id > 0) value += 4; else value += 3; }
  for (int i = 0, n = (int)task_id; i < n; ++i) value += src[i];
  switch (task_id & 3u) {
  case 0: value += 1; break; case 1: value -= 1; break;
  case 2: value *= 2; break; default: value += 7; break;
  }
  return value;
}

int main(int argc, char *argv[]) {
  parse_args(argc, argv);
  if (count == 0) count = 4;
  RT_CHECK(vx_dev_open(&device));
  uint64_t nc, nw, nt;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_CORES, &nc));
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_WARPS, &nw));
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_THREADS, &nt));
  uint32_t num_points = count * (uint32_t)(nc * nw * nt);
  uint32_t buf_size = num_points * sizeof(int32_t);
  kernel_arg.num_points = num_points;
  RT_CHECK(vx_mem_alloc(device, buf_size, VX_MEM_READ, &src_buffer));
  RT_CHECK(vx_mem_address(src_buffer, &kernel_arg.src_addr));
  RT_CHECK(vx_mem_alloc(device, buf_size, VX_MEM_WRITE, &dst_buffer));
  RT_CHECK(vx_mem_address(dst_buffer, &kernel_arg.dst_addr));
  std::vector<int32_t> h_src(num_points), h_dst(num_points);
  for (uint32_t i = 0; i < num_points; ++i) h_src[i] = (int32_t)(i * 3 + 1);
  RT_CHECK(vx_copy_to_dev(src_buffer, h_src.data(), 0, buf_size));
  RT_CHECK(vx_upload_kernel_file(device, kernel_file, &krnl_buffer));
  RT_CHECK(vx_upload_bytes(device, &kernel_arg, sizeof(kernel_arg_t), &args_buffer));
  RT_CHECK(vx_start(device, krnl_buffer, args_buffer));
  RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));
  RT_CHECK(vx_copy_from_dev(h_dst.data(), dst_buffer, 0, buf_size));
  int errors = 0;
  for (uint32_t i = 0; i < num_points; ++i) {
    if (h_dst[i] != ref_value(h_src[i], i, h_src)) ++errors;
  }
  cleanup();
  return errors ? 1 : 0;
}
