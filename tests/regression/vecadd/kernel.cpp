// #include <vx_spawn.h>
#include <vx_intrinsics.h>
#include <vx_print.h>
#include "common.h"

// void kernel_body(kernel_arg_t* __UNIFORM__ arg) {
// 	auto src0_ptr = reinterpret_cast<TYPE*>(arg->src0_addr);
// 	auto src1_ptr = reinterpret_cast<TYPE*>(arg->src1_addr);
// 	auto dst_ptr  = reinterpret_cast<TYPE*>(arg->dst_addr);
	
// 	dst_ptr[blockIdx.x] = src0_ptr[blockIdx.x] + src1_ptr[blockIdx.x];
// }

int main() {
	kernel_arg_t* arg = (kernel_arg_t*)csr_read(VX_CSR_MSCRATCH);
	// return vx_spawn_threads(1, &arg->num_points, nullptr, (vx_kernel_func_cb)kernel_body, arg);
	size_t warpId = csr_read(VX_CSR_CTA_ID);
	size_t warpSize = csr_read(VX_CSR_NUM_THREADS);
	size_t threadOff = csr_read(VX_CSR_THREAD_ID);

	size_t threadId = warpId * warpSize + threadOff;

	// vx_printf("thread id: %d\n", threadId);

	auto src0_ptr = reinterpret_cast<TYPE*>(arg->src0_addr);
	auto src1_ptr = reinterpret_cast<TYPE*>(arg->src1_addr);
	auto dst_ptr  = reinterpret_cast<TYPE*>(arg->dst_addr);

	dst_ptr[threadId] = src0_ptr[threadId] + src1_ptr[threadId];

	return 0;
}
