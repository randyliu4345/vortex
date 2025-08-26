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
	int warpId = static_cast<int>(csr_read(VX_CSR_CTA_ID));
	int warpSize = vx_num_threads();
	int threadId = vx_thread_id();

	// int cond = (warpId == 0) && (threadId == 0);
	// int sp = vx_split(cond);

	// if (cond) {
	// 	asm volatile(
	// 		"la   a0, _edata\n\t"
	// 		"la   a2, _end\n\t"
	// 		"sub  a2, a2, a0\n\t"
	// 		"li   a1, 0\n\t"
	// 		"call memset\n\t"
	// 		:
	// 		:
	// 		: "memory", "ra",
	// 			"a0","a1","a2","a3","a4","a5","a6","a7",
	// 			"t0","t1","t2","t3","t4","t5","t6");
	// }

	// vx_join(sp);


	int g_threadId = warpId * warpSize + threadId;

	// vx_printf("thread id: %d\n", threadId);

	auto src0_ptr = reinterpret_cast<TYPE*>(arg->src0_addr);
	auto src1_ptr = reinterpret_cast<TYPE*>(arg->src1_addr);
	auto dst_ptr  = reinterpret_cast<TYPE*>(arg->dst_addr);

	dst_ptr[g_threadId] = src0_ptr[g_threadId] + src1_ptr[g_threadId];

	vx_tmc(warpId == 0);

	return 0;
}
