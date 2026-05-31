#include <iostream>
#include <unistd.h>
#include <vortex.h>
#include <vector>
#include <assert.h>
#include "common.h"

#define RT_CHECK(_expr)                                         \
   do {                                                         \
     int _ret = _expr;                                          \
     if (0 == _ret)                                             \
       break;                                                   \
     printf("Error: '%s' returned %d!\n", #_expr, (int)_ret);   \
     cleanup();                                                 \
     exit(-1);                                                  \
   } while (false)

const char* kernel_file = "kernel.vxbin";
uint32_t count = 0;

vx_device_h device = nullptr;
vx_buffer_h src_buffer = nullptr;
vx_buffer_h dst_buffer = nullptr;
vx_buffer_h krnl_buffer = nullptr;
vx_buffer_h args_buffer = nullptr;
kernel_arg_t kernel_arg = {};

static void show_usage() {
   std::cout << "CFG diverge lab test." << std::endl;
   std::cout << "Usage: [-n points] [-k kernel] [-h]" << std::endl;
}

static void parse_args(int argc, char **argv) {
  int c;
  while ((c = getopt(argc, argv, "n:k:h")) != -1) {
    switch (c) {
    case 'n': count = atoi(optarg); break;
    case 'k': kernel_file = optarg; break;
    case 'h': show_usage(); exit(0);
    default: show_usage(); exit(-1);
    }
  }
}

void cleanup() {
  if (device) {
    vx_mem_free(src_buffer);
    vx_mem_free(dst_buffer);
    vx_mem_free(krnl_buffer);
    vx_mem_free(args_buffer);
    vx_dev_close(device);
  }
}

static int32_t ref_value(int32_t value, uint32_t task_id, const std::vector<int32_t>& src) {
  if (task_id > 1) {
    if (task_id > 2) value += 6;
    else value += 5;
  } else {
    if (task_id > 0) value += 4;
    else value += 3;
  }
  for (int i = 0, n = (int)task_id; i < n; ++i)
    value += src[i];
  switch (task_id & 3u) {
  case 0: value += 1; break;
  case 1: value -= 1; break;
  case 2: value *= 2; break;
  default: value += 7; break;
  }
  return value;
}

int main(int argc, char *argv[]) {
  parse_args(argc, argv);
  if (count == 0) count = 4;

  std::cout << "open device connection" << std::endl;
  RT_CHECK(vx_dev_open(&device));

  uint64_t num_cores, num_warps, num_threads;
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_CORES, &num_cores));
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_WARPS, &num_warps));
  RT_CHECK(vx_dev_caps(device, VX_CAPS_NUM_THREADS, &num_threads));

  uint32_t total_threads = num_cores * num_warps * num_threads;
  uint32_t num_points = count * total_threads;
  uint32_t buf_size = num_points * sizeof(int32_t);
  kernel_arg.num_points = num_points;

  RT_CHECK(vx_mem_alloc(device, buf_size, VX_MEM_READ, &src_buffer));
  RT_CHECK(vx_mem_address(src_buffer, &kernel_arg.src_addr));
  RT_CHECK(vx_mem_alloc(device, buf_size, VX_MEM_WRITE, &dst_buffer));
  RT_CHECK(vx_mem_address(dst_buffer, &kernel_arg.dst_addr));

  std::vector<int32_t> h_src(num_points);
  std::vector<int32_t> h_dst(num_points);
  for (uint32_t i = 0; i < num_points; ++i)
    h_src[i] = (int32_t)(i * 3 + 1);

  RT_CHECK(vx_copy_to_dev(src_buffer, h_src.data(), 0, buf_size));
  RT_CHECK(vx_upload_kernel_file(device, kernel_file, &krnl_buffer));
  RT_CHECK(vx_upload_bytes(device, &kernel_arg, sizeof(kernel_arg_t), &args_buffer));
  RT_CHECK(vx_start(device, krnl_buffer, args_buffer));
  RT_CHECK(vx_ready_wait(device, VX_MAX_TIMEOUT));
  RT_CHECK(vx_copy_from_dev(h_dst.data(), dst_buffer, 0, buf_size));

  int errors = 0;
  for (uint32_t i = 0; i < num_points; ++i) {
    int32_t expect = ref_value(h_src[i], i, h_src);
    if (h_dst[i] != expect) {
      std::cout << "error #" << i << ": got " << h_dst[i] << " expected " << expect << std::endl;
      ++errors;
    }
  }

  cleanup();
  if (errors) {
    std::cout << "FAILED!" << std::endl;
    return errors;
  }
  std::cout << "PASSED!" << std::endl;
  return 0;
}
