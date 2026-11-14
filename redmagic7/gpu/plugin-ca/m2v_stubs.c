/*
 * RedMagic 7: HakiosMetalCA.m weak-imports UniXNU's Metal on Vulkan (NativeBridgeMetalFrontend), used only under
 * boot-arg metal_m2v=1. This port does not ship it; these stand-ins satisfy the link and are never reached without
 * that boot-arg (native_bridge_metal_open is only called behind hm_boot_arg_set("metal_m2v=1")).
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>

void *native_bridge_metal_open(const char *loader, const char *cache, uint64_t registryID, void *missing)
{
	(void)loader; (void)cache; (void)registryID; (void)missing;
	return NULL;
}
uint64_t native_bridge_metal_first_problem(char *out, size_t size) { if (size) out[0] = '\0'; return 0; }
size_t native_bridge_metal_status(char *out, size_t size) { if (size) out[0] = '\0'; return 0; }
size_t native_bridge_metal_formats(char *out, size_t size) { if (size) out[0] = '\0'; return 0; }
const void *(*native_bridge_metal_variant_hook)(const char *, const char *, const char *, int, size_t *) = NULL;
