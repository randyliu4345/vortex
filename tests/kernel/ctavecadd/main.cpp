#include <stdio.h>
#include <vx_print.h>
#include <vx_intrinsics.h>

int main()
{
	size_t cta_x = csr_read(VX_CSR_CTA_X);
	size_t cta_y = csr_read(VX_CSR_CTA_Y);
	size_t cta_z = csr_read(VX_CSR_CTA_Z);
	size_t cta_id = csr_read(VX_CSR_CTA_ID);

	vx_printf("cta_x: %d, \t cta_y: %d, \t cta_z: %d, \t cta_id: %d\n", cta_x, cta_y, cta_z, cta_id);

	return 0;
}