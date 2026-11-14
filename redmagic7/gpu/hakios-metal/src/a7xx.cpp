// RedMagic 7 port (MIT, as the rest of hakios-metal): an A6XX-encoded 3D stream (render.cpp, command.cpp) rewritten
// for the Adreno 730 (A7XX). See include/hakios-metal/a7xx.hpp: translateA7xx for what is rewritten and why.
#include "hakios-metal/a7xx.hpp"

#include <unordered_map>
#include <unordered_set>

#include "hakios-metal/a6xx.hpp"

namespace hakios::metal::a7xx {

namespace {

bool gA7xx = false;

// a6xx.xml: registers whose A7XX offset differs (redmagic7/gpu/a7xx-delta.py; layouts checked equal with
// a7xx-fields.py, HLSQ_INVALIDATE_CMD's bindless bits aside, which invalidateCmd() maps)
const std::unordered_map<uint32_t, uint32_t> kMoved = {
    {0xb982, 0xa9c7}, {0xb983, 0xa9c8}, {0xb984, 0xa9c9}, {0xb985, 0xa9ca}, {0xb986, 0xa9cb},   // HLSQ_CONTROL_1..5_REG
    {0xb987, 0xa9cd},                                                                         // HLSQ_CS_CNTL
    {0xb990, 0xa9d4}, {0xb991, 0xa9d5}, {0xb992, 0xa9d6}, {0xb993, 0xa9d7}, {0xb994, 0xa9d8},   // HLSQ_CS_NDRANGE_0..
    {0xb995, 0xa9d9}, {0xb996, 0xa9da}, {0xb998, 0xa9db},                                     // ..6, HLSQ_CS_CNTL_1
    {0xb999, 0xa9dc}, {0xb99a, 0xa9dd}, {0xb99b, 0xa9de},                                     // HLSQ_CS_KERNEL_GROUP_X..Z
    {0xb800, 0xa827}, {0xb801, 0xa83f}, {0xb802, 0xa867}, {0xb803, 0xa898}, {0xbb10, 0xab03},   // HLSQ_VS/HS/DS/GS/FS_CNTL
    {0xb980, 0xa9c6},                                                                         // HLSQ_FS_CNTL_0
    {0xbb08, 0xab1f},                                                                         // HLSQ_INVALIDATE_CMD
    {0x9981, 0x9809}, {0x9980, 0x9107},                                                       // PC_POLYGON_MODE, PC_RASTER_CNTL
    {0xacc0, 0xa9bf},                                                                         // SP_2D_DST_FORMAT
    // the 2D source block, SP_PS_2D_SRC_INFO .. SP_PS_UNKNOWN_B4D0, all 0x200 lower on A7XX (a6xx.xml)
    {0xb4c0, 0xb2c0}, {0xb4c1, 0xb2c1}, {0xb4c2, 0xb2c2}, {0xb4c3, 0xb2c3}, {0xb4c4, 0xb2c4}, {0xb4c5, 0xb2c5},
    {0xb4c6, 0xb2c6}, {0xb4c7, 0xb2c7}, {0xb4c8, 0xb2c8}, {0xb4c9, 0xb2c9}, {0xb4ca, 0xb2ca}, {0xb4cb, 0xb2cb},
    {0xb4cc, 0xb2cc}, {0xb4cd, 0xb2cd}, {0xb4ce, 0xb2ce}, {0xb4cf, 0xb2cf}, {0xb4d0, 0xb2d0},
    {0xb4d1, 0xab21},                                                                         // SP_WINDOW_OFFSET
};
// a6xx.xml: A6XX-only registers, not written on A7XX (Gallium's fd6_emit_static_regs skips the same)
const std::unordered_set<uint32_t> kGoneRegs = {
    a6xx::REG_A6XX_TPL1_UNKNOWN_B605, a6xx::REG_A6XX_HLSQ_UNKNOWN_BE00, a6xx::REG_A6XX_HLSQ_UNKNOWN_BE01,
    a6xx::REG_A6XX_HLSQ_DBG_ECO_CNTL, a6xx::REG_A6XX_HLSQ_SHARED_CONSTS, a6xx::REG_A6XX_VPC_UNKNOWN_9107,
    a6xx::REG_A6XX_VPC_UNKNOWN_9210, a6xx::REG_A6XX_VPC_UNKNOWN_9211,
    a6xx::REG_A6XX_RB_UNKNOWN_8819, a6xx::REG_A6XX_RB_UNKNOWN_881A, a6xx::REG_A6XX_RB_UNKNOWN_881B,
    a6xx::REG_A6XX_RB_UNKNOWN_881C, a6xx::REG_A6XX_RB_UNKNOWN_881D, a6xx::REG_A6XX_RB_UNKNOWN_881E,
    a6xx::REG_A6XX_RB_UNKNOWN_8A00, a6xx::REG_A6XX_RB_UNKNOWN_8A10, a6xx::REG_A6XX_RB_UNKNOWN_8A20,
    a6xx::REG_A6XX_RB_UNKNOWN_8A30,
};
// FD730's magic values (freedreno_devices.py a730_magic_regs) where kStaticState has the A635's
const std::unordered_map<uint32_t, uint32_t> kMagic = {
    {a6xx::REG_A6XX_TPL1_DBG_ECO_CNTL, 0x01000000}, {a6xx::REG_A6XX_GRAS_DBG_ECO_CNTL, 0x00000800},
    {a6xx::REG_A6XX_SP_CHICKEN_BITS, 0x00001440},   {a6xx::REG_A6XX_UCHE_CLIENT_PF, 0x00000084},
    {a6xx::REG_A6XX_PC_MODE_CNTL, 0x0000003f},      {a6xx::REG_A6XX_SP_DBG_ECO_CNTL, 0x10000000},
    {a6xx::REG_A6XX_RB_DBG_ECO_CNTL, 0x00000000},   {a6xx::REG_A6XX_RB_UNKNOWN_8E01, 0x00000000},
    {a6xx::REG_A6XX_VPC_DBG_ECO_CNTL, 0x02000000},  {a6xx::REG_A6XX_UCHE_UNKNOWN_0E12, 0x03200000},
};

constexpr uint32_t kInvalidReg = 0xfc;   // regid(63, 0)

struct Out {
    pm4::Stream& s;
    void reg(uint32_t r, uint32_t v) { s.registers(r, &v, 1); }
};

uint32_t invalidateCmd(uint32_t v) {
    uint32_t out = v & 0xffu;                                   // the per-stage state and the two IBO bits
    if (v & (0x1fu << 9)) out |= 0xffu << 9;                    // A6XX CS_BINDLESS 9-13 -> A7XX 9-16
    if (v & (0x1fu << 14)) out |= 0xffu << 17;                  // A6XX GFX_BINDLESS 14-18 -> A7XX 17-24
    return out;                                                 // (shared-const bits 8 and 19 do not exist on A7XX)
}

}  // namespace

void setA7xx(bool on) { gA7xx = on; }
bool isA7xx() { return gA7xx; }

bool isTranslated(const pm4::Stream& s) {
    const std::vector<uint32_t>& w = s.words();
    size_t i = 0;
    while (i < w.size() && w[i] == pm4::type7(pm4::kNop, 0)) i++;   // (appendStream's empty NOP before each encoder's words)
    return w.size() >= i + 2 && w[i] == pm4::type7(pm4::kNop, 1) && w[i + 1] == kTranslatedMark;
}

pm4::Stream translateA7xx(const pm4::Stream& from) {
    using namespace a6xx;
    if (isTranslated(from)) return from;   // (a stream appended twice, e.g. a pass's sysmem stream: rewritten once)
    pm4::Stream to;
    Out o{to};
    to.packet(pm4::kNop, &kTranslatedMark, 1);
    const std::vector<uint32_t>& w = from.words();
    uint32_t hlsqControl[3] = {kInvalidReg * 0x01010101u, kInvalidReg * 0x01010101u, kInvalidReg * 0x01010101u};
    for (size_t i = 0; i < w.size();) {
        pm4::Header h = pm4::decode(w[i]);
        if (h.kind == pm4::Header::kRegisters) {
            for (uint32_t k = 0; k < h.count; k++) {
                const uint32_t in = h.reg + k;
                uint32_t v = w[i + 1 + k];
                if (kGoneRegs.count(in)) continue;
                auto m = kMagic.find(in);
                if (m != kMagic.end()) v = m->second;
                auto mv = kMoved.find(in);
                const uint32_t reg = mv != kMoved.end() ? mv->second : in;
                switch (in) {
                case REG_A6XX_HLSQ_INVALIDATE_CMD:
                    v = invalidateCmd(v);
                    break;
                case REG_A6XX_RB_CCU_CNTL:
                    if ((v & ~A6XX_RB_CCU_CNTL_CONCURRENT_RESOLVE) != rbCcuCntlSysmem()) {
                        // GMEM (encodeGmem's rbCcuCntlGmem): hakios lays tiles out for the A635's 512 KiB and puts the
                        // colour CCU at its end (480 KiB); the A730's GMEM is 2 MiB (KGSL gen7_0_0 SZ_2M), so the same
                        // layout fits. RB_CCU_CNTL2 for it: depth CCU at 0 full size (fd6_emit_ccu_cntl, gmem), the
                        // colour CCU at hakios's offset with FD730's fraction (EIGHTH, gmem_ccu_color_cache_fraction).
                        // Then fd7_emit_static_binning_regs: buffers in GMEM (8812 0), 8E06 0, 8007 0, 8110 2, 8E09 4,
                        // and RB_BLIT_CLEAR_MODE GMEM.
                        const uint32_t colorOffset = ((v >> 23) & 0x1ffu) << 12 | ((v >> 9) & 1u) << 21;
                        o.reg(0x8e07, 0x1);
                        o.reg(0x88e5, ((colorOffset >> 21) & 1u) << 2 | 3u << 21 | ((colorOffset >> 12) & 0x1ffu) << 23);
                        o.reg(0x8812, 0);
                        o.reg(0x8e06, 0);
                        o.reg(0x8007, 0);
                        o.reg(REG_A6XX_GRAS_UNKNOWN_8110, 0x2);
                        o.reg(0x8e09, 0x4);
                        o.reg(0x88e4, 1);   // RB_BLIT_CLEAR_MODE: CLEAR_MODE_GMEM
                        continue;
                    }
                    // sysmem. A7XX split it: RB_CCU_CNTL keeps the static bits (fast clear disabled, no concurrent resolve on
                    // FD730), RB_CCU_CNTL2 (0x88e5) the per-pass layout: sysmem, depth CCU at 0 and colour CCU after
                    // the 4 x 256 KiB depth caches (fd6_screen.cc: color_ccu_offset 0x100000 -> COLOR_OFFSET 0x100)
                    o.reg(0x8e07, 0x1);
                    o.reg(0x88e5, 0x100u << 23);
                    // what Gallium's sysmem prep adds on A7XX (fd6_gmem.cc emit_sysmem_prep): all buffers in sysmem,
                    // FD730's RB_UNKNOWN_8E06, GRAS_UNKNOWN_8007/8110, RB_UNKNOWN_8E09; and its static A7XX state
                    // (fd6_emit_static_regs): the raw magic registers, PC_RASTER_CNTL_V2, GRAS_LRZ_CNTL2
                    o.reg(0x8812, 0x3ff);
                    o.reg(0x8e06, 0x02080000);
                    o.reg(0x8007, 0);
                    o.reg(REG_A6XX_GRAS_UNKNOWN_8110, 0x2);
                    o.reg(0x8e09, 0x4);
                    for (const Magic& mr : kMagicRaw) o.reg(mr.reg, mr.value);
                    o.reg(0x9317, 0);   // PC_RASTER_CNTL_V2
                    o.reg(0x810b, 0);   // GRAS_LRZ_CNTL2
                    continue;
                case REG_A6XX_RB_RENDER_CNTL:
                    // A7XX has no CCUSINGLECACHELINESIZE (bits 3-5) or FLAG_* here: sysmem = not binning, TYPE_TILED,
                    // LR_TB = 0 (fd6_gmem.cc update_render_cntl), and its GRAS_SU_RENDER_CNTL copy beside it
                    v &= 0x1840u;
                    o.reg(reg, v);
                    o.reg(0x8116, 0);
                    continue;
                case REG_A6XX_GRAS_BIN_CONTROL:
                case REG_A6XX_RB_BIN_CONTROL:
                    v &= ~0x00c00000u;   // BUFFERS_LOCATION is A6XX-only (RB_UNKNOWN_8812 says sysmem instead)
                    break;
                case REG_A6XX_RB_DEPTH_BUFFER_INFO:
                    // A7XX names the depth layout (TILEMODE, bits 5-6); A6XX's RB always used TILE6_3, the layout
                    // texture.cpp gives a depth texture. Gallium: .tilemode = TILE6_3 with any depth format (v23: without
                    // it the third depth draw read 128 KiB past the depth texture and hung)
                    if ((v & 0x7u) != 0) v = (v & ~(3u << 5)) | (3u << 5);
                    break;
                case REG_A6XX_RB_STENCIL_INFO:
                    if (v & 1u) v = (v & ~(3u << 2)) | (3u << 2);   // separate stencil: TILEMODE TILE6_3 (bits 2-3)
                    break;
                case REG_A6XX_GRAS_CL_CNTL:
                case REG_A6XX_RB_DEPTH_CNTL:
                    v |= 1u << 5;        // Z_CLAMP_ENABLE: "with a7xx the hw doesn't do the clamping for us" (Gallium)
                    break;
                case REG_A6XX_SP_FS_PREFETCH_CNTL:
                    v |= (0x1ffu << 6) | (0x1ffu << 16);   // CONSTSLOTID, CONSTSLOTID4COORD (fd6_program.cc, A7XX)
                    break;
                case 0xa99f: case 0xa9a0: case 0xa9a1: case 0xa9a2: {
                    // SP_FS_PREFETCH[n].CMD: A7XX packs it differently and takes the sampler and texture ids from
                    // SP_FS_BINDLESS_PREFETCH[n] even when not bindless (fd6_program.cc); A6XX: SRC 0-6, SAMP_ID 7-10,
                    // TEX_ID 11-15, DST 16-21, WRMASK 22-25, HALF 26, BINDLESS 28, CMD 29-31. A7XX: SRC 0-6, DST 13-18,
                    // WRMASK 19-22, HALF 23, BINDLESS 25, CMD 26-29.
                    const uint32_t samp = (v >> 7) & 0xfu, tex = (v >> 11) & 0x1fu;
                    const uint32_t a7 = (v & 0x7fu) | ((v >> 16) & 0x3fu) << 13 | ((v >> 22) & 0xfu) << 19 | ((v >> 26) & 1u) << 23 |
                                        ((v >> 28) & 1u) << 25 | ((v >> 29) & 0x7u) << 26;
                    o.reg(in, a7);
                    o.reg(0xa9a3 + (in - 0xa99f), samp | tex << 16);
                    continue;
                }
                default:
                    break;
                }
                if (in >= 0xb983 && in <= 0xb985) hlsqControl[in - 0xb983] = v;
                o.reg(reg, v);
                // A7XX's copies and companions of what was just written
                switch (in) {
                case REG_A6XX_PC_PRIMITIVE_CNTL_0: o.reg(0x9109, v); break;        // VPC_PRIMITIVE_CNTL_0
                case REG_A6XX_RB_BLIT_INFO: o.reg(0x88e4, 1); break;               // RB_BLIT_CLEAR_MODE GMEM before each BLIT (Gallium)
                case 0x9981: o.reg(0x9307, v); break;                              // PC_POLYGON_MODE -> VPC_POLYGON_MODE2
                case REG_A6XX_RB_DITHER_CNTL: o.reg(0xa9ac, v); break;             // SP_DITHER_CNTL
                case REG_A6XX_GRAS_LRZ_BUFFER_BASE: o.reg(0x8113, 0); break;   // GRAS_LRZ_DEPTH_BUFFER_INFO: no LRZ (fd6_gmem.cc emit_lrz)
                case REG_A6XX_GRAS_2D_BLIT_CNTL:
                    // the 2D engine on A7XX: TPL1_2D_SRC_CNTL after the blit control, a 2D source, no raw copy, no texel
                    // offset (turnip's r2d_setup_common, A7XX)
                    o.reg(0xb2d2, 1u << 29);
                    break;
                case REG_A6XX_SP_VS_PVT_MEM_HW_STACK_OFFSET: o.reg(0xa82d, 0); break;   // SP_VS_VGPR_CONFIG
                case REG_A6XX_SP_FS_PVT_MEM_HW_STACK_OFFSET: o.reg(0xaa01, 0); break;   // SP_FS_VGPR_CONFIG
                case REG_A6XX_PC_PS_CNTL:
                    o.reg(REG_A6XX_GRAS_UNKNOWN_8110, 0x2);
                    o.reg(0xa9aa, 0);   // HLSQ_FS_UNKNOWN_A9AA: constants loaded
                    break;
                case REG_A6XX_SP_FS_OUTPUT_CNTL0:
                    o.reg(0xaa02, 0);   // SP_PS_ALIASED_COMPONENTS_CONTROL
                    o.reg(0xaa03, 0);   // SP_PS_ALIASED_COMPONENTS
                    break;
                case 0xb986: {
                    // after HLSQ_CONTROL_5_REG: HLSQ_UNKNOWN_A9AE, the FS's system-value registers (fd6_program.cc):
                    // centre-rhw 1, every other ij 2, face/sampleid/samplemask 1 each, xy and zw coords 2 each
                    auto valid = [](uint32_t r) { return (r & 0xffu) != kInvalidReg; };
                    uint32_t n = 0;
                    for (int b = 0; b < 3; b++) n += valid(hlsqControl[0] >> (8 * b));   // face, sampleid, samplemask
                    n += valid(hlsqControl[0] >> 24);                                       // centerrhw
                    for (int b = 0; b < 4; b++) n += 2 * valid(hlsqControl[1] >> (8 * b));
                    for (int b = 0; b < 4; b++) n += 2 * valid(hlsqControl[2] >> (8 * b));
                    o.reg(0xa9ae, n | (1u << 8) | (1u << 9));
                    break;
                }
                default:
                    break;
                }
            }
        } else {
            const uint32_t* p = h.count ? &w[i + 1] : nullptr;
            if (h.opcode == CP_EVENT_WRITE && h.count >= 1) {
                // The events as Mesa's fd_gpu_events<A7XX> names them (freedreno_gpu_event.h): CACHE_INVALIDATE (49) is
                // CACHE_INVALIDATE7 (51), 49 being CACHE_CLEAN there; CACHE_FLUSH_TS is CACHE_CLEAN; the CCU flushes
                // are the CCU_CLEAN_* events, which write nothing. An event the A6XX stream has write a value (4 words:
                // address, value) and waits for keeps that write: as itself where A7XX's event writes (RB_DONE_TS,
                // CACHE_CLEAN), otherwise the A7XX event alone and then CACHE_CLEAN writing the value -- in either
                // case with CP_EVENT_WRITE7's WRITE_ENABLED (bit 27), as KGSL's gen7 ring writes its timestamps
                std::vector<uint32_t> e(p, p + h.count);
                const uint32_t ev = e[0] & 0xffu;
                uint32_t ev7 = ev;
                bool writes = true;   // A7XX's event can carry the write itself
                switch (ev) {
                case CACHE_INVALIDATE: ev7 = kCacheInvalidate7; break;
                case CACHE_FLUSH_TS: ev7 = kCacheClean; break;
                case PC_CCU_FLUSH_COLOR_TS: ev7 = 33; writes = false; break;   // CCU_CLEAN_COLOR
                case PC_CCU_FLUSH_DEPTH_TS: ev7 = 32; writes = false; break;   // CCU_CLEAN_DEPTH
                case PC_CCU_RESOLVE_TS: ev7 = 26; writes = false; break;       // CCU_RESOLVE_CLEAN
                default: break;
                }
                if (h.count >= 4 && !writes) {
                    // RedMagic v51: the CCU clean, a WFI, and the value written by the CP itself (CP_MEM_WRITE). Its
                    // earlier form, CACHE_CLEAN with WRITE_ENABLED, wrote at a pass's end but never inside one: the
                    // flush before a framebuffer-fetch draw (kTableFbReadFlushAt) stayed 0 and its CP_WAIT_REG_MEM
                    // hung the GPU at the lock screen's first frame (METAL-PORT.md, v50). Turnip's A7XX flushes inside
                    // a pass are plain events and a WFI as well.
                    const uint32_t plain = (e[0] & ~0xffu & ~(1u << 30) & ~(1u << 27)) | ev7;
                    to.packet(h.opcode, &plain, 1);
                    to.packet(CP_WAIT_FOR_IDLE, nullptr, 0);
                    const uint32_t mw[3] = {e[1], e[2], e[3]};
                    to.packet(kCpMemWrite, mw, 3);
                    i += 1 + h.count;
                    continue;
                } else {
                    e[0] = (e[0] & ~0xffu) | ev7;
                }
                if (h.count >= 4) e[0] = (e[0] & ~(1u << 30)) | (1u << 27);   // WRITE_ENABLED (and no A6XX TIMESTAMP)
                to.packet(h.opcode, e.data(), h.count);
            } else if ((h.opcode == CP_LOAD_STATE6_GEOM || h.opcode == CP_LOAD_STATE6_FRAG) && h.count >= 1 &&
                       ((p[0] >> 14) & 3u) == ST6_SHADER && ((p[0] >> 18) & 0xfu) >= 0x8 && ((p[0] >> 18) & 0xfu) <= 0xc) {
                // no shader preload for the 3D stages on A7XX (fd6_emit_shader preloads only on A6XX)
            } else if (h.opcode == CP_SET_DRAW_STATE) {
                // left out: the stream sets no state groups, and the vendor SQE is known to refuse the packet
                // (ios-mondrian docs/MONDIAN_GPU.md)
            } else {
                to.packet(h.opcode, p, h.count);
            }
        }
        i += 1 + h.count;
    }
    return to;
}

}  // namespace hakios::metal::a7xx
