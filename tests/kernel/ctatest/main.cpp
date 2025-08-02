#include <stdio.h>
#include <vx_print.h>
#include <vx_intrinsics.h>

int main()
{
	size_t cta_x = csr_read(VX_CSR_CTA_X);
	vx_printf("cta_x: %d\n", cta_x);

	return 0;
}