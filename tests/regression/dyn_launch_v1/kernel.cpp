#include <vx_spawn.h>
#include <vx_print.h>
#include "common.h"

static void hello_body(void* /*arg*/) {
  vx_printf("Hello ");
}

static void world_body(void* /*arg*/) {
  vx_printf("World!\n");
}

static void parent_body(void* /*arg*/) {
  uint32_t g = 1, b = 1;
  vx_spawn_threads(1, &g, &b, reinterpret_cast<vx_kernel_func_cb>(hello_body), nullptr);
  vx_spawn_threads(1, &g, &b, reinterpret_cast<vx_kernel_func_cb>(world_body), nullptr);
}

int main() {
  uint32_t g = 1, b = 1;
  return vx_spawn_threads(1, &g, &b,
                          reinterpret_cast<vx_kernel_func_cb>(parent_body), nullptr);
}
