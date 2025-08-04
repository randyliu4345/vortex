#include <stdio.h>
#include <vx_print.h>
#include <vx_intrinsics.h>

int main()
{
	size_t cta_z = csr_read(VX_CSR_CTA_Z);
	vx_printf("cta_z: %d\n", cta_z);

	return 0;
}