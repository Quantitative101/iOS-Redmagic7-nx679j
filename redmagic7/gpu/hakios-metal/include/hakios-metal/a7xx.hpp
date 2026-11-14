// The Adreno 730 (A7XX, chip 0x07030001, Mesa FD730) registers a compute dispatch writes where they differ from
// a6xx.hpp: offsets and fields from Mesa 25.0.7's generated a6xx.xml.h (REG_A7XX_*, A7XX_*), the sequence from
// Mesa's computerator (src/freedreno/computerator/a6xx.cc, cs_program_emit<A7XX> and a6xx_emit_grid<A7XX>), and the
// chip's raw magic registers from its device table (freedreno_devices.py a730_raw_magic_regs, printed by
// redmagic7/gpu/dump-devinfo.c). RedMagic 7 port; MIT like the rest of hakios-metal.
#pragma once
#include <cstdint>

#include "hakios-metal/pm4.hpp"

namespace hakios::metal::a7xx {

inline constexpr uint32_t REG_SP_CS_CNTL_1 = 0xa9c3;          // A7XX variant (A6XX: SP_CS_CNTL_1 at SP_CS_CNTL_0 + 1)
inline constexpr uint32_t REG_HLSQ_FS_CNTL_0 = 0xa9c6;
inline constexpr uint32_t REG_HLSQ_CONTROL_2_REG = 0xa9c8;    // .. _5_REG at 0xa9cb
inline constexpr uint32_t REG_HLSQ_CS_CNTL = 0xa9cd;
inline constexpr uint32_t REG_HLSQ_CS_NDRANGE_0 = 0xa9d4;     // .. _6 at 0xa9da
inline constexpr uint32_t REG_HLSQ_CS_CNTL_1 = 0xa9db;
inline constexpr uint32_t REG_HLSQ_CS_KERNEL_GROUP_X = 0xa9dc;  // .. _Z at 0xa9de
inline constexpr uint32_t REG_HLSQ_CS_LAST_LOCAL_SIZE = 0xa9df;
inline constexpr uint32_t REG_HLSQ_INVALIDATE_CMD = 0xab1f;   // same low bits as A6XX (VS..GFX_IBO = 0x01..0x80)

// HLSQ_CS_CNTL_1 (A7XX): LINEARLOCALIDREGID [7:0], THREADSIZE bit 9, WORKGROUPRASTORDERZFIRSTEN bit 11,
// WGTILEWIDTH [25:20], WGTILEHEIGHT [31:26]; computerator: regid(63,0), the kernel's thread size, z-first on, width 4,
// height by local_size[1] (3 if %8, 5 if %4, 9 if %2, else 17).
constexpr uint32_t hlsqCsCntl1(uint32_t threadsize, uint32_t localY) {
    uint32_t h = localY % 8 == 0 ? 3 : localY % 4 == 0 ? 5 : localY % 2 == 0 ? 9 : 17;
    return 0xfcu | (threadsize << 9) | 0x800u | (4u << 20) | (h << 26);
}
// SP_CS_CNTL_1 (A7XX): LINEARLOCALIDREGID [7:0], THREADSIZE bit 8, WORKITEMRASTORDER bit 15 (1 = linear, as computerator)
constexpr uint32_t spCsCntl1(uint32_t threadsize) { return 0xfcu | (threadsize << 8) | (1u << 15); }
// HLSQ_CS_LAST_LOCAL_SIZE: LOCALSIZEX-1 [11:2], Y-1 [21:12], Z-1 [31:22]
constexpr uint32_t lastLocalSize(uint32_t x, uint32_t y, uint32_t z) {
    return ((x - 1) << 2) | ((y - 1) << 12) | ((z - 1) << 22);
}

// vgt_event_type on A7XX (adreno_pm4.xml): 49 is CACHE_CLEAN there, not A6XX's CACHE_INVALIDATE; the UCHE
// invalidate is CACHE_INVALIDATE7 = 51 (and CACHE_FLUSH7 = 50). Sending 49 left UCHE holding the previous
// dispatch's IBO descriptors: v16 measured the compiled kernels writing through a freed buffer's address
// (SMMU write fault at the earlier dispatch's output + 0x340).
inline constexpr uint32_t kCacheClean = 49;
// CP_MEM_WRITE (adreno_pm4.xml 0x3d): address lo, hi, then the words the CP writes there itself
inline constexpr uint32_t kCpMemWrite = 0x3d;
// SP_BINDLESS_BASE[0] of the graphics stages on A7XX (a6xx.xml: 0xab0a, stride 2, 8 of them; A6XX had them at 0xab10)
inline constexpr uint32_t REG_SP_BINDLESS_BASE_0 = 0xab0a;
inline constexpr uint32_t kCacheFlush7 = 50;
inline constexpr uint32_t kCacheInvalidate7 = 51;

// SP_CS_BINDLESS_BASE[n].DESCRIPTOR (A7XX: 8 of them at 0xa9e8 + 2n, A6XX: 5): ADDR in bits 63:2, DESC_SIZE 1:0
inline constexpr uint32_t REG_SP_CS_BINDLESS_BASE_0 = 0xa9e8;
inline constexpr uint64_t kBindlessDescriptor64B = 3;

struct Magic {
    uint32_t reg, value;
};
inline constexpr Magic kMagicRaw[] = {
    {0x0e17, 0x00840004}, {0xb602, 0x00040724}, {0xae08, 0x00002400}, {0xae09, 0x00000000}, {0xae0a, 0x00000000},
    {0x0e10, 0x00000000}, {0x0e11, 0x00000040}, {0xae6c, 0x00008000}, {0x9e00, 0x20080000}, {0x9e24, 0x21fc7f00},
    {0xa600, 0x00000000}, {0xae06, 0x00000000}, {0xae6a, 0x00000000}, {0xae6b, 0x00000080}, {0xae73, 0x00000000},
    {0xab02, 0x00000000}, {0xab01, 0x00000000}, {0xab22, 0x00000000}, {0xb310, 0x00000000}, {0x8120, 0x09510840},
    {0x8121, 0x00000a62}, {0x0ce2, 0x00000000}, {0x0ce3, 0x00000000}, {0x0ce4, 0x00000000}, {0x0ce5, 0x00000000},
    {0x0ce6, 0x00000000}, {0x0ce7, 0x00000000}, {0x80a7, 0x00000000}, {0x8e79, 0x00000000}, {0x8899, 0x00000000},
    {0x88f5, 0x00000000},
};

// The 3D stream on A7XX (src/a7xx.cpp). render.cpp and command.cpp encode for A6XX; with setA7xx(true) (the probe sets
// it for the Adreno 730, before anything is drawn) every encoder's stream is rewritten as it is appended to a command
// buffer: the registers that moved (a7xx-delta.py) to their A7XX offsets, the A6XX-only ones left out, FD730's magic
// values for the A635's, RB_CCU_CNTL split into RB_CCU_CNTL + RB_CCU_CNTL2 with Gallium's A7XX sysmem prep and static
// registers beside it, RB_RENDER_CNTL/BIN_CONTROL without their A6XX-only fields, the A7XX copies (VPC_PRIMITIVE_CNTL_0,
// VPC_POLYGON_MODE2, SP_DITHER_CNTL, SP_xS_VGPR_CONFIG, HLSQ_FS_UNKNOWN_A9AA, HLSQ_UNKNOWN_A9AE, SP_PS_ALIASED_*),
// Z clamping on, CACHE_INVALIDATE as CACHE_INVALIDATE7 and every event that writes memory with WRITE_ENABLED (bit 27),
// no 3D shader preload, no CP_SET_DRAW_STATE. Compute (compute.cpp with Options::a7xx) is A7XX already and not
// rewritten. A GMEM pass is not handled (RB_CCU_CNTL2 is set for sysmem): draw with render::Options::gmem off.
// a translated stream opens with a CP_NOP carrying this word, so that a stream appended twice is rewritten once
inline constexpr uint32_t kTranslatedMark = 0xa7a7a7a7;
bool isTranslated(const pm4::Stream& s);
void setA7xx(bool on);
bool isA7xx();
pm4::Stream translateA7xx(const pm4::Stream& from);

}  // namespace hakios::metal::a7xx
