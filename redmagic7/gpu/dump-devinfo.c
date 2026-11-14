// Print what Mesa's device table says of a chip (raw magic registers, instruction cache size, threadsize, ...),
// for porting hakios-metal's dispatch to the Adreno 730. Built against the host Mesa build (ref/mesa-host).
#include <stdio.h>
#include <stdlib.h>
#include "freedreno_dev_info.h"

int main(int argc, char **argv)
{
	uint64_t chip = argc > 1 ? strtoull(argv[1], NULL, 0) : 0x07030001ull;
	struct fd_dev_id id = { .gpu_id = 0, .chip_id = chip };
	const struct fd_dev_info *info = fd_dev_info_raw(&id);

	if (info == NULL) {
		printf("no device info for chip 0x%llx\n", (unsigned long long)chip);
		return 1;
	}
	printf("chip 0x%llx: chip gen %u, num_sp_cores %u, fibers_per_sp %u, threadsize_base %u, wave_granularity %u\n",
	    (unsigned long long)chip, info->chip, info->num_sp_cores, info->fibers_per_sp, info->threadsize_base,
	    info->wave_granularity);
	printf("instr_cache_size %u, supports_double_threadsize %u, has_lpac %u\n", info->a6xx.instr_cache_size,
	    info->a6xx.supports_double_threadsize, info->a6xx.has_lpac);
	for (unsigned i = 0; i < sizeof(info->a6xx.magic_raw) / sizeof(info->a6xx.magic_raw[0]); i++) {
		if (!info->a6xx.magic_raw[i].reg) {
			break;
		}
		printf("magic_raw 0x%04x = 0x%08x\n", info->a6xx.magic_raw[i].reg, info->a6xx.magic_raw[i].value);
	}
	return 0;
}
