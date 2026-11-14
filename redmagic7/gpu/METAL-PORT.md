# Porting hakios-metal to the RedMagic 7 (Adreno 730)

Assessment written 2026-11-12. Nothing is ported yet.

## What hakios-metal is

`oosexgang/hakios-xnu-kernel/hakios-metal` (blobless clone: `ref/hakios-xnu-kernel`, MIT; Adreno numbers from Mesa, MIT).
A Metal driver in C++ (~20k lines) for HackiOS's Adreno 7c+ Gen 3 (**a6xx**, chip 0x06030500): Metal's command model
straight onto the GPU's command processor, no Mesa GL in between. Shaders: Apple's AIR -> NIR (HackiOS's own front end
patched into Mesa) -> ir3 (Mesa), compiled on the phone. A plug-in (`plugin/HakiosMetal.m`) makes
`MTLCreateSystemDefaultDevice()` return it. Measured on their phone: compute (Apple's own kernels match an LLVM
reference bit for bit), 2D textures/samplers, triangle draws through `MTLRenderCommandEncoder`, clear, blending, MRT,
framebuffer fetch. All of CoreAnimation's 39 vertex + 95 fragment shaders compile. **CoreAnimation does not draw
through it yet** (their window server exited on `attachment_clear_frag_lph`).

Why it is the better road than the mondrian port's: mondrian runs CA's *GL* path through Mesa/OSMesa and copies every
frame back on the CPU (1-2 fps). iOS 16's CoreAnimation is a Metal client; this serves Metal directly.

## Fit with our stack

| Layer | hakios-metal expects | We have | Gap |
|---|---|---|---|
| Kernel transport | IOKit service `HakiosGPU`, `IOConnectCallStructMethod(selector = msm_drm request)`, buffers via `IOConnectMapMemory64` | `MondrianGPU` (POCO port): **identical request numbers** (0x09, 0x40-0x4c, 0x7e surface) and Linux msm_drm structs | service name only (`openKernelGpu` matches "HakiosGPU"); the param answers (chip id, GMEM size...) to check |
| GPU bring-up | a running CP, SMMU, GMU | mondrian's a730 GMU/HFI/GX/CP code, `gpu=0` on our phone so far | **never run on the RedMagic**; missing local `mondrian_cp_profile.h` (CP tables); chip is **0x07030001 (FD730)**, mondrian is 0x07030002 (FD725) |
| Register layer | `a6xx.hpp` (4498 lines, 1652 constants checked vs Mesa) | - | A7XX deltas below |
| Command streams | `compute.cpp`, `render.cpp` (3353), `texture.cpp`, `command.cpp`: turnip/fd6 **a6xx** sequences | - | re-derive from turnip's `CHIP == A7XX` paths |
| Compiler | Mesa ir3 + HackiOS AIR front end, built for iOS | Mesa 25.0.7 iOS cross setup from the Note 8 (`ref/mesa-work`); FD730 is in Mesa's device table (a7xx_gen1) | build their front-end patch into our Mesa |
| Test harness | fake GPU interpreter (2586 lines) checks a6xx register semantics | - | its checks follow the register set: port with it |

## A7XX register delta (measured)

`redmagic7/gpu/a7xx-delta.py` over `a6xx.hpp` and Mesa 25.0.7's `a6xx.xml` (which carries both generations):

- 360 register names in the header; 293 resolved by name
- **246 identical on A7XX** (84%)
- **33 at another offset**: almost all the HLSQ block folded into SP (`HLSQ_CS_*` 0xb98x -> 0xa9cx/0xa9dx,
  `HLSQ_{VS,HS,DS,GS,FS}_CNTL`, `HLSQ_INVALIDATE_CMD`, `HLSQ_CONTROL_*`), plus `PC_RASTER_CNTL`, `PC_POLYGON_MODE`,
  `SP_PS_2D_SRC*`, `SP_2D_DST_FORMAT`, `SP_WINDOW_OFFSET`
- **14 gone** (mostly `*_UNKNOWN_*` a6xx magic, `HLSQ_SHARED_CONSTS`, `HLSQ_CS_CNTL_0`)
- 67 not matched by name (array members/sub-fields): to check by hand

Beyond offsets, A7XX needs what turnip adds under `CHIP >= A7XX`: `CP_THREAD_CONTROL` (BR/BV pipes; a7xx has
concurrent binning), changed bitfields in registers that kept their offset (variants in the XML), new CCU/LRZ
registers, and the a7xx preamble. Sysmem (direct) rendering, which is all hakios-metal does, keeps this smaller.

## Plan (in order; each step has a boot-checkable result)

1. **GPU bring-up on the RedMagic** (mondrian code, no Metal yet): generate `mondrian_cp_profile.h` from Qualcomm's
   open gen7 tables (mondrian `tools/gen-cp-profile-open.py`), boot `gpu=1 gpupower=1 gmustart=1 gpugx=1 gpucp=1
   gpusubmit=1`, see GMU `FW_INIT_RESULT=0x100`, GX on, CP_ME_INIT accepted, `MondrianGPU` published. Check
   FD730-vs-FD725 differences in the GMU/power tables.
2. **Transport**: add `MondrianGPU` to `openKernelGpu` (or publish the service also as `HakiosGPU`); run their
   `hakios-metal-probe` stages 1-4 (buffers, CP_MEM_WRITE/MEMCPY blits, fences): these use only PM4 packets common to
   a6xx/a7xx, so they should work before any register port.
3. **Register layer**: `a7xx.hpp` generated from Mesa's XML for A7XX (their `tools/check-a6xx.py` method), the 33
   moved + 14 gone handled; compute dispatch first (fewest registers), checked with their probe's compute stages.
4. **Draw**: re-derive `render.cpp`'s sysmem pass from turnip's a7xx path; probe stage 9 (triangle, pixel checks).
5. **Metal plug-in + CoreAnimation**: their plug-in as-is on top; CA through Metal is unsolved upstream too, so we
   would be working at the same frontier as HackiOS from here on.

Effort: step 1 is days (code exists, needs our numbers); steps 2-3 one to two weeks; step 4 weeks; step 5 open-ended.

## Notes

- `make test` on this Mac needs their Mesa host build (`xnu-deps/build/mesa-host`) and `llvm-as` on PATH
  (Homebrew llvm); not run yet.
- A few fixture files did not fetch from the promisor (`tri_hc_fs_air.hpp`, `chain160.ll`, `tri_null_fs.ll`): re-clone
  without `--filter` if needed.

## Step 1 status (2026-11-12)

- `mondrian_cp_profile.h` generated from **Nubia's own KGSL** (`ref/redmagic/nubia-NX679S/.../drivers/gpu/msm`, sparse
  checkout added): `gen7_0_1` (chip 0x07030001, ours) uses the same `gen7_0_0` gbif/hwcg/protect tables. 5 gbif, 53 hwcg,
  45 protect, 68 restore, 1085 GEN7_* defines. Nubia's gen7_reg.h has no `GEN7_CP_LPAC_RB_RPTR` (diagnostic print only,
  guarded with `RM_LPAC_RPTR` in mondrian_gpu.c).
- Build fixes: `tools/lldbmacros/core/syntax_checker.py` shebang `#!/usr/bin/python3 -I` (its own `core/collections.py`
  shadowed the stdlib and broke the build); new headers picked by `__has_include` need a `touch` of the .c file.
- Kernel `kernel/kernel.redmagic7.v10` (17.88 MB, CP code in). Probe image `payload/shim-boot-v10gpu.img`:
  iPhone 8 layout + `gpu=1 gpupower=1 gmustart=1 gpugx=1 gpucp=1 gpusubmit=1`, Wi-Fi test off. Not flashed.
  Look for `MONDRIAN GPU:` lines: GMU `FW_INIT_RESULT`, HFI acks, GX on, CP start / ME_INIT, `MondrianGPU: service published`.

## Step 1 result (2026-11-12, v11): the GPU's command processor runs

- GMU fw `FW_INIT_RESULT 0x100` in 1.4 ms, HFI perf/bw/core/vote/hold acked, GX GDSC on, GX clocks on.
- CP: SMMU stream slots 2-5 taken (the stall in v10gpu was the diagnostic SMR dump, now limited to slots 0-2 /
  `gpusmrdump=N`), SQE fw (74672 bytes) loaded, ring memwrite canary + fence, an IB ran, **2000 uc submissions,
  0 refused, no fault**, `MondrianGPU: service published`. SMMU FSR 0x400 left at the end: to look at.
- Fixed after v11 (not booted yet): GET_PARAM CHIP_ID 0x07030001 (was the POCO's 0x07030002), MAX_FREQ 818 MHz.
- `boards.py` REDMAGIC7 cmdline now carries the GPU flags and the XR canvas by default; ADT `payload/redmagic7-ram-xr.adt`
  (`ADT_XR=1`). Next: step 2 (hakios-metal probe stages 1-4 against `MondrianGPU`).

## Step 2 result (2026-11-12, v12): hakios-metal's probe stages 1-4 pass on the RedMagic

`redmagic7/gpu/rm-metal-probe.cpp` (hakios-metal library, transport falling back to `MondrianGPU`) as
`/usr/libexec/mondrian-gpu-test.dylib`, written into the root by the kernel (`bsd/dev/rm_blob.c`, `rmblob=1`, blob from
`gpu/gen-blob.py`), loaded by the UI hook in backboardd (`gltest=1`):

    RM METAL: GPU 730, chip id 0x7030001, 818 MHz
    RM METAL: ok   CP_MEMCPY of 500000 bytes: same bytes, edges untouched, 4105 us
    RM METAL: ok   fill of 4 / 256 / 260 / 65540 / 999996 bytes at 16
    RM METAL: ok   40 command buffers in flight: handlers in order, memory as written
    RM METAL: ok   16 MB by CP_MEMCPY in 34639 us at best (484 MB/s)
    RM METAL: done, 0 failure(s)

Build: `make out/ios/libhakios-metal.a` with Xcode's iPhoneOS SDK (one `__builtin_signbit` -> `__builtin_bit_cast` in
a6xx.hpp for newer Apple clang), then the dylib linked against it (-framework IOKit CoreFoundation), ad-hoc signed.
Next: step 3, the a7xx register layer for compute (needs Mesa ir3 for iOS + HackiOS's AIR front end, or our own ir3
fixtures first).

## Step 3 first result (2026-11-12, v13): the first shader runs on the Adreno 730

    RM METAL: ok   A7XX dispatch of end_only: completed, 1024 of 1024 words untouched, 211 us
    RM METAL: ok   A7XX dispatch of fill_id, 16x64: 1024 of 1024 right, 0 untouched
                   (word 0 00001234 want 00001234, word 1023 3faf5c83 want 3faf5c83), 240 us
    RM METAL: done, 0 failure(s)

- Host Mesa 25.0.7 with HackiOS's patches built natively: `ref/mesa-host` (tarball sha256-checked, their 8 patches +
  ir3-dump, `hakios-drm` + AIR sources copied in; needs Homebrew bison 3.8 and the venv with mako on PATH). Gives
  libfreedreno_ir3/air/nir and `ir3_compiler`. hakios-metal's `tools/kernels.mk` patched for Apple ld (no --start-group).
- `out/host/ir3-kernel --chip-id 0x07030001`: FD730 fixtures in `tests/fixtures-a730/` (fill_id words identical to a6xx).
- `include/hakios-metal/a7xx.hpp` + `compute::Options::a7xx`: computerator's A7XX program/grid sequence and FD730's 31
  raw magic registers (from `gpu/dump-devinfo.c`). Run with `tail = false` (the kernel's fence closes the submission).
- Next: textures/samplers and the tail events on A7XX (CP_EVENT_WRITE7), then the AIR compiler on the phone
  (Mesa for iOS), then the draw (render.cpp) on turnip's A7XX sysmem path.

## Step 4 first result (2026-11-12, v14): Mesa's compiler runs on the phone

    RM METAL: ok   compile fill_id.ll on the phone for FD730: 64 words, instrlen 2, constlen 12, in 30 ms
    RM METAL: FAIL A7XX dispatch of the phone-compiled fill_id: 0 of 1024 right (word 0 c0ffee00 want 00004321), 794 us

- The probe dylib (7.8 MB) links HackiOS's AIR front end and Mesa's ir3 from `ref/mesa-host/build-ios`; the kernel
  writes it into the root (`rmblob=1`). Compiling the kernel takes 30 ms.
- The compiled dispatch completes but writes nothing, not even word 0. Host listing (`out/host/compiler-smoke
  --chip-id 0x07030001` over the fixture's bitcode): for FD730, ir3 adds an **early preamble**
  (`shps / getone / resinfo` prefetching IBO 0's descriptor, `EARLYPREAMBLE` set) and ends with `(jp)lock / unlock`.
  The A635 build (`tests/fixtures/fill_id_air.hpp`) has neither. Everything else is the same: driver params at c0,
  buffer 1's range at c4, immediates at c8, store through IBO 0.
- v15 runs three dispatches: the kernel as compiled; the same kernel with `EARLYPREAMBLE` cleared; and the A635
  fixture, which has no preamble. The kernel now logs any fence that finds a CP interrupt, a CP fault or an SMMU
  fault, with the fault address.

## v15 (2026-11-12): the preamble is not the cause

All three fail the same way: the kernel as compiled, the same kernel with EARLYPREAMBLE off, and the A635 fixture with
no preamble. All 1024 words are untouched. The SMMU reports a translation fault during the first of them:
`fsr 0x80000402 far 0x12300340`, an unmapped page in the user VA range, which starts at 0x10000000. VAs are handed out
in order and never reused, so a freed buffer's range stays unmapped. The hand-written fill_id passes on the same path,
so the cause is in what a compiled kernel adds: driver params, the constant range loaded from buffer 1, the UBO
descriptors and the explicit local size.
v16 logs every buffer's VA (`MondrianGPU: bo N at ...`) and every free, and clears FSR after each logged fault so
each fence reports its own. The probe prints `out` and `c` addresses for each dispatch.

## v16 (2026-11-12): stale IBO descriptors, because the cache-invalidate event has a different number on A7XX

The faulting address 0x12300340 is in bo 21, the output buffer of the stage-5 hand-written `fill_id`, which was freed
before stage 6. fsynr0 0x10013 marks the access as a write. All three stage-6 dispatches write through the old
buffer's IBO descriptor instead of their own. hakios-metal sends `CP_EVENT_WRITE` event 49 to invalidate UCHE. That is
CACHE_INVALIDATE on A6XX, but on A7XX 49 is CACHE_CLEAN and the invalidate is CACHE_INVALIDATE7 = 51
(`adreno_pm4.xml`). So `CP_LOAD_STATE6` read the new dispatch's IBO table, which sits at the same arena address,
from UCHE lines left by the stage-5 table. v17: `compute.cpp` sends 51 on A7XX (`a7xx::kCacheInvalidate7`), once
before the state loads and again where the A6XX path invalidates. render.cpp and command.cpp still use the A6XX
event numbers (CACHE_INVALIDATE, PC_CCU_*, CACHE_FLUSH_TS) and need the same review before the draw step.

## v17 (2026-11-12): event 51 does not fix it

With CACHE_INVALIDATE7, the three stage-6 dispatches still write through stage 5's freed output buffer:
`far 0x12290780`, bo 14 this boot, fsynr0 0x80013. The stage-6 outputs stay untouched. The stage-5 stream
writes correct values to its own output, so a write fault at a stage-5 address with nothing landing in stage 6
looks like the GPU is running the stage-5 stream, or stage-5 state, again. The streams and descriptor tables live
in slabs the queue recycles at the same GPU addresses. MondrianGPU writes every BO of a submission back to DRAM
first, so on the CPU side the new bytes should be there.
v18 runs five dispatches: the hand-written fill_id again on the same queue; the compiled kernel on the same queue;
then on a new queue (new slabs at new addresses) the compiled kernel, the hand-written one and the A635 build.
MondrianGPU logs every user submission (IB address, dwords, whether expanded, fence, and a checksum of the
stream as the CPU sees it).

## v18 (2026-11-12): the first IBO descriptor sticks

On the same queue and on a new one (new slabs, new IB address, different stream checksums), every dispatch after
stage 5's `fill_id` failed, the same hand-written `fill_id` with fresh buffers included. Every one wrote through stage
5's freed output at +0x340. So it is not the compiled kernels, and not stale memory at a reused address. The GPU keeps
the first non-bindless CS IBO descriptor it loaded. The program does reload (stage 5 ran end_only, then fill_id,
fine), and HLSQ_INVALIDATE_CMD (CS_IBO) plus CACHE_INVALIDATE7 do not clear the IBO state. Mesa never uses
non-bindless IBOs on A7XX: Gallium (`fd6_image.cc`) and turnip both use bindless descriptor sets, and they invalidate
the CS bindless bases (HLSQ_INVALIDATE_CMD bits 9-16 = 0xff on A7XX) before the bindless IBO preload.
v19 (`compute::Options::a7xxInvalidateBindless`, `iboDirect`) runs the hand-written fill_id plain, then with each
candidate fix, then both, then the compiled kernels with both, then plain again. If neither fix works, the next
step is the bindless path: descriptor set, SP_CS_BINDLESS_BASE, kernels compiled with bindless IBOs.

## v19 (2026-11-12): neither candidate fix helps

All 8 stage-6 dispatches fail: no fix, bindless invalidate, IBO descriptors in the packet (SS6_DIRECT), both,
the compiled kernels with both, and plain again. With the descriptor in the packet, the shader still stores through
stage 5's output, so the store address does not come from our CP_LOAD_STATE6 IBO data. The kernel's fence writes
CACHE_CLEAN (0x31 | bit 27), so results left dirty in UCHE are not the explanation either.
v20 has the CP dump the CS registers to memory (`Options::dumpTo`, CP_REG_TO_MEM) right before CP_EXEC_CS and after
it: 0xa9b0..0xa9bf, SP_CS_IBO, SP_CS_IBO_COUNT, HLSQ_CS_CNTL. It does this for stage 5's end_only and fill_id and
for three stage-6 dispatches, so we can see whether the register writes land.

## v20 (2026-11-12): SP_CS_IBO is not a register on A7XX

The register dump shows the program address (0xa9b4) changing on every dispatch, so register writes do land. 0xa9f2
reads stage 5's table address (0x122dc000) in every stage-6 dispatch. In Mesa's a6xx.xml, 0xa9f2 is `SP_CS_IBO`
with no variant attribute, but the A7XX `SP_CS_BINDLESS_BASE` array (8 entries at 0xa9e8, stride 2) covers it:
on A7XX 0xa9f2 is bindless base 5. Neither turnip nor Gallium programs a non-bindless CS IBO on A6XX+. Gallium runs
`ir3_nir_lower_io_to_bindless` on every gen-6+ shader.
v21 switches A7XX compute buffers to bindless:
- `compiler.cpp` lowers SSBO and image access to `bindless_resource_ir3` (base 0, descriptor = IBO slot) for gen-7
  compute, and the Kernel gets `bindlessIbo` from ir3's `v->bindless_ibo`.
- `compute.cpp`, for a bindlessIbo kernel: HLSQ_INVALIDATE_CMD CS_BINDLESS (0xff << 9), SP_CS_BINDLESS_BASE[0] =
  IBO table | DESC_SIZE 64B, `CP_LOAD_STATE6_FRAG ST6_IBO SS6_BINDLESS` preload, SP_CS_CONFIG.BINDLESS_IBO.
  SP_CS_IBO is no longer written for these kernels.
- `tools/kernels/fill_id_bl.asm` / `tests/fixtures-a730/fill_id_bl.hpp`: the hand-written fill_id storing through
  `stib...imm.base0`, used for stage 5 and stage 6.

## v21 result (2026-11-12): kernels compiled on the phone run on the Adreno 730

    RM METAL: ok   A7XX dispatch of fill_id (bindless IBO), 16x64: 1024 of 1024 right
    RM METAL: ok   compile fill_id.ll on the phone for FD730: 64 words, instrlen 2, constlen 12, in 17 ms
    RM METAL: phone-compiled fill_id: bindlessIbo 1, earlyPreamble 1
    RM METAL: ok   A7XX dispatch of hand-written fill_id (bindless) again: 1024 of 1024 right
    RM METAL: ok   A7XX dispatch of the phone-compiled fill_id: 1024 of 1024 right, 328 us
    RM METAL: ok   A7XX dispatch of the phone-compiled fill_id, early preamble off: 1024 of 1024 right
    RM METAL: ok   A7XX dispatch of hand-written fill_id (bindless), third time: 1024 of 1024 right
    RM METAL: ok   A7XX dispatch of the A635-compiled fill_id (not bindless): 1024 of 1024 right
    RM METAL: done, 0 failure(s)

Bindless IBOs fix it. The early preamble works. The last, non-bindless dispatch passes only because it is the first
non-bindless IBO load of the boot; the stage-5 kernel is now bindless. On A7XX, compute must stay bindless.
Still open:
- render.cpp/command.cpp use A6XX event numbers (CACHE_INVALIDATE 49 = CACHE_CLEAN on A7XX, PC_CCU_* renamed) and
  the gfx IBO (SP_IBO 0xab1a) needs the same review before the draw step.
- tail events (`Options::tail`) on A7XX.
- textures and samplers on A7XX.

## Step 5, the draw (2026-11-12, v22): render.cpp's stream rewritten for A7XX

`hakios-metal/src/a7xx.cpp` `translateA7xx`, on with `a7xx::setA7xx(true)`, rewrites every encoder stream as
render.cpp's `appendStream`/`appendDeduped` append it. The reference is Gallium fd6's A7XX branches (`fd6_emit.cc`
static regs and ccu_cntl, `fd6_gmem.cc` sysmem prep and render_cntl, `fd6_program.cc`, `fd6_rasterizer.cc`) and
`fd_gpu_events<A7XX>`:
- The 33 moved registers go to their A7XX offsets (a7xx-delta.py); `a7xx-fields.py` found the layouts unchanged
  except HLSQ_INVALIDATE_CMD's bindless bits. The 18 A6XX-only registers are dropped, and the A635 magic values are
  replaced with FD730's.
- RB_CCU_CNTL becomes A7XX RB_CCU_CNTL = 1 plus RB_CCU_CNTL2 = 0x80000000 (sysmem: colour CCU at 1 MiB). Next to it
  go RB_UNKNOWN_8812 0x3ff, 8E06, 8007, 8E09, GRAS_UNKNOWN_8110, the 31 raw magic registers, PC_RASTER_CNTL_V2 and
  GRAS_LRZ_CNTL2.
- RB_RENDER_CNTL and BIN_CONTROL lose their A6XX-only fields; GRAS_SU_RENDER_CNTL is added.
- The A7XX copies are added: VPC_PRIMITIVE_CNTL_0, VPC_POLYGON_MODE2, SP_DITHER_CNTL, SP_VS/FS_VGPR_CONFIG,
  HLSQ_FS_UNKNOWN_A9AA, HLSQ_UNKNOWN_A9AE (FS sysval count), SP_PS_ALIASED_COMPONENTS*, and the A7XX PREFETCH_CNTL
  slot ids.
- Z clamp is on.
- Events: CACHE_INVALIDATE becomes 51 and CACHE_FLUSH_TS becomes CACHE_CLEAN. PC_CCU_*_TS become the A7XX
  CCU_CLEAN_* events, followed by a CACHE_CLEAN that writes the value the stream waits for. Every event that writes
  memory gets WRITE_ENABLED (bit 27).
- Dropped: the 3D shader preload and CP_SET_DRAW_STATE.
- Sysmem only: GMEM passes are not translated (`render::Options::gmem = false`).

A translated stream opens with a NOP carrying 0xa7a7a7a7, so a stream that is appended twice is rewritten only once.
Host check: `tests/render_test.cpp` `a7xx_translation_of_the_first_triangle` (99/99 render tests pass) writes the
decoded stream to `out/host/a7xx-tri_flat.txt`.
Probe stage 7 compiles tri_flat's VS and FS on the phone, links them for RGBA8Unorm, draws one triangle into a 64x64
target and checks every pixel.

## v22 result (2026-11-12): the first triangle on the Adreno 730

    RM METAL: ok   compile and link tri_flat on the phone for FD730
    RM METAL: ok   A7XX draw of tri_flat, one triangle into 64x64 RGBA8Unorm: 2016 of 2016 inside the colour,
                   2016 of 2016 outside canary, diagonal 0 of 64 the colour, 0 other, pixel (0,63) ff804020,
                   (63,0) a5a5a5a5, 1633 us
    RM METAL: done, 0 failure(s)

Pixel-exact on the first try. Metal's orientation is right, and the top-left fill rule leaves the diagonal out. The
phone went on to the home screen normally.
Next:
- the rest of hakios's stage 9 on A7XX: the pair, per-vertex colours, a constant buffer, vertex attributes,
  textures (A7XX prefetch ids move to SP_FS_BINDLESS_PREFETCH_CMD), depth;
- the GMEM path (RB_CCU_CNTL2 for GMEM, A7XX tile registers);
- blits and clears (the BLIT event and 2D registers on A7XX);
- then CoreAnimation through hakios-metal.

## v23 (2026-11-12): the rest of the first-draw checks

Probe stage 8 (`redmagic7/gpu/rm-draw-tests.inc`, included in rm-metal-probe.cpp) ports hakios's stage-9 checks.
Every program is compiled on the phone. The checks:
- 8a: the pair of triangles.
- 8b: the second triangle alone (vertexStart 3).
- 8c: a half viewport.
- 8d: tri_cflat with two colours from a constant buffer.
- 8e: tri_color, per-vertex colours, every pixel within one unit of its barycentric weights.
- 8f: tri_attr through a vertex descriptor.
- 8g: tri_tex, a 4x4 texture through the FS prefetch.
- 8h: depth, Less + write, NEAR then FAR, over a "clear" that is itself a draw at z 1.

New in the translator:
- SP_FS_PREFETCH_CMD is repacked to the A7XX layout, with the sampler and texture ids moved to
  SP_FS_BINDLESS_PREFETCH_CMD[n] (0xa9a3+n).
- GRAS_LRZ_DEPTH_BUFFER_INFO (0x8113) is written as a copy of the depth format.

No Clear yet: clears go through CP_BLIT and the 2D engine, not yet ported, and ios-mondrian saw a vendor SQE refuse
CP_BLIT.

## v23 result (2026-11-12): 7 of 8 pass, depth hangs

All exact:
- 8a: the pair, 4096/4096.
- 8b: the second triangle alone, 4032/4032.
- 8c: half viewport, 4096/4096.
- 8d: the constant buffer, both colours, 4032/4032.
- 8e: tri_color, 4032/4032, max deviation 0.
- 8f: tri_attr, 4032/4032, max deviation 0.
- 8g: tri_tex through the prefetch, 3840/3840 texel-exact.

8h, depth: three draws submitted; the GPU read 0x123e8000, which is 128 KiB past the depth texture and unmapped
(fsr 0x80000402). The probe thread never came back from the wait. backboardd and SpringBoard kept running.
v24, following Gallium's emit_zs/emit_lrz:
- RB_DEPTH_BUFFER_INFO.TILEMODE = TILE6_3 (A7XX-only field, bits 5-6) for any depth format.
- RB_STENCIL_INFO.TILEMODE = TILE6_3 for separate stencil.
- GRAS_LRZ_DEPTH_BUFFER_INFO = 0 when LRZ is off. v23 had wrongly copied the depth format into it.

## v24 result (2026-11-12): every draw check passes on the Adreno 730

    RM METAL: ok   8h depth, Less + write, NEAR then FAR over a drawn clear: 4096 of 4096 as wanted
    RM METAL: done, 0 failure(s)

The depth fix was RB_DEPTH_BUFFER_INFO.TILEMODE = TILE6_3 plus GRAS_LRZ_DEPTH_BUFFER_INFO = 0. Every probe stage now
passes in one boot: copies and fills, compute (hand-written and compiled on the phone, bindless IBOs), and draws
(flat, viewport, constant buffer, interpolated colours, vertex fetch, texture prefetch, depth).
Still open on A7XX:
- Clear and blit: CP_BLIT and the 2D engine (SP_PS_2D_SRC*, SP_2D_DST_FORMAT moved; the BLIT event).
- GMEM passes: RB_CCU_CNTL2 for GMEM, A7XX tile and resolve registers.
- Stencil, MSAA, MRT, indexed draws, blending.
- Then CoreAnimation through hakios-metal (HackiOS's EAGL/CA hooks).

## v25 (2026-11-12): Clear and blit through the 2D engine

Translator additions, following turnip's r2d_setup_common for A7XX:
- the whole SP_PS_2D_SRC block (0xb4c0..0xb4d0) moved 0x200 lower;
- TPL1_2D_SRC_CNTL (0xb2d2) = TYPE 2D after GRAS_2D_BLIT_CNTL;
- SP_2D_DST_FORMAT was already remapped.
FD730's RB_DBG_ECO_CNTL_blit equals RB_DBG_ECO_CNTL, so no switch around CP_BLIT is needed.
Our SQE is the vendor blob (`58 01 73 01`), the same one ios-mondrian used. They first blamed CP_BLIT for an opcode
error and later traced it to stale cache, which MondrianGPU's per-submission write-back already covers; the blob's
one confirmed refusal is draw-state groups, which the translator drops.
Probe stage 9, run last, with "9 (CP_BLIT) next" printed first:
- 9a: a Clear alone.
- 9b: a Clear then tri_flat in one pass.
- 9c: a depth Clear to 1, then NEAR/FAR.
- 9d: a 64x64 blit of a texture.

## v25 result (2026-11-12): CP_BLIT works on the vendor SQE

    RM METAL: ok   9a Clear of 64x64 RGBA8Unorm through CP_BLIT: 4096 of 4096 the clear colour
    RM METAL: ok   9b Clear then tri_flat in one pass: 4096 of 4096 right
    RM METAL: ok   9c depth cleared to 1 through CP_BLIT, NEAR then FAR: 4096 of 4096 as wanted
    RM METAL: ok   9d blit of a 64x64 RGBA8 texture through the 2D engine: 4096 of 4096 right
    RM METAL: done, 0 failure(s)

The vendor SQE takes CP_BLIT. Clear, depth clear and blit work in sysmem on the Adreno 730.
Next: GMEM passes (the tiled path real frames use: RB_CCU_CNTL2 for GMEM, the A7XX tile and resolve registers, the
BLIT event), stencil, MSAA and MRT; then CoreAnimation.

## v26 (2026-11-12): GMEM passes

Translator additions:
- RB_CCU_CNTL with encodeGmem's value (anything but rbCcuCntlSysmem) becomes A7XX RB_CCU_CNTL = 1 plus a GMEM
  RB_CCU_CNTL2: depth CCU at 0, full size; colour CCU at hakios's offset (480 KiB, the A635's 512 KiB GMEM minus its
  colour cache) with FD730's EIGHTH fraction. The A730's GMEM is 2 MiB (KGSL gen7_0_0), so hakios's 512 KiB tile
  plan fits unchanged.
- Gallium's fd7_emit_static_binning_regs come with it: 8812 0, 8E06 0, 8007 0, 8110 2, 8E09 4, and
  RB_BLIT_CLEAR_MODE = GMEM.
- RB_BLIT_CLEAR_MODE (0x88e4) = GMEM before every RB_BLIT_INFO, as Gallium does before each BLIT.
- Pass end: encodeGmem's sysmem RB_CCU_CNTL brings back the sysmem layout and 8812 0x3ff. The RB_BLIT registers and
  the BLIT event are unchanged on A7XX. Hardware binning (gmemBinning) is not covered.
Probe stage 10, last, with "10 (GMEM) next" printed first:
- 10a: the pair over a loaded target.
- 10b: a Clear and T1.
- 10c: a Clear, T1 and T2 as two draws of one pass.
- 10d: 512x512, several tiles.
Each line says whether the pass went to GMEM.

## v26 result (2026-11-12): GMEM works on the Adreno 730

    RM METAL: ok   10a GMEM, tri_flat's pair over a loaded 64x64: 4096 of 4096 (GMEM)
    RM METAL: ok   10b GMEM, a Clear and T1: 4096 of 4096 (GMEM)
    RM METAL: FAIL 10c GMEM, a Clear, T1 and T2 in one pass: 4032 of 4096 (GMEM); first off (0,0): ff804020, wanted ffbf8040
    RM METAL: ok   10d GMEM, a Clear and T1 in 512x512 (several tiles): 262144 of 262144 (GMEM)

10c's failure is the test's own: T1 and T2 together cover the diagonal (as 8a shows), and the expectation wrongly
left it at the clear colour. The 4032 off-diagonal pixels are right. The expectation is fixed for the next build.
Tile loads, tile clears, multi-draw passes and multi-tile stores all work in GMEM.
Remaining before CoreAnimation: hardware binning (gmemBinning), stencil, MSAA, MRT, and running hakios's EAGL/CA
plug-in with setA7xx.

## Step 6 plan: CoreAnimation through hakios-metal (2026-11-12)

HackiOS drew its window server through Metal on their A635 phone at about 100 fps
(`ref/hackios-hackios-xnu-kernel/docs/reference/metal-measurements.md`). Their pieces:
- `userland/src/HakiosMetalCA.m` (8.7k lines, MIT; vcvkk's plug-in plus HackiOS's shader-variant choice): an
  `_MTLDevice` subclass with queue, command buffers, blit, compute and render encoders, pipelines, over
  hakios-metal's C API (`hakios_metal.h`), built as `HakiosMetal.bundle`.
- The kernel: `IOAcceleratorES` gets `MetalPluginName=HakiosMetal` and `MetalPluginClassName=HakiosMetalDevice`
  under boot-arg `metal=1` (`MiniIOMobileFramebuffer.cpp`). Our `MondrianFramebuffer.cpp` already publishes
  `IOAcceleratorES` (for CoreAnimation's 30 s wait), without the properties.
- QuartzCore's `default.metallib`: this image ships only `default.pipelinelib`. `tools/ca-metallib.py` builds one
  from the image's pipelinelib plus the air64 slice of a Mac's QuartzCore library (this Mac has one; nothing
  Apple's goes in the repository). HackiOS's `inject.c` points QuartzCore at it (boot-arg `ca_metallib=`).
- Shader caches and boot-arg tuning (metal_defer, wallpaper_cache, metal_gmem_*): later.

The RedMagic pieces:
- A. Kernel: `metal=1` adds the two properties in MondrianFramebuffer. rm_blob learns to create directories, for
  `/System/Library/Extensions/HakiosMetal.bundle/`.
- B. hakios-metal: when the GPU reports chip 0x0703xxxx, the C API sets the compiler chip and turns the A7XX
  translator on by itself, so the plug-in needs no A7XX code.
- C. Build HakiosMetalCA.m against our hakios-metal and the iOS Mesa archives; deliver bundle + Info.plist +
  default.metallib with rmblob.
- D. The ca_metallib hook (HackiOS inject.c's part) in our UI hook, or a QuartzCore lookup path that needs no hook.
- E. First boot: the logs say how far the window server gets. Recovery as always (Power+Vol Down to Android).
  The probe (gltest) is off for these boots.

## v27 (2026-11-12): first CoreAnimation-on-Metal boot

- hakios-metal: `hakios_metal_open` accepts chip 0x0703xxxx. For it, the open sets the compiler chip and
  `a7xx::setA7xx(true)`, and `compute::Options` defaults to `a7xx = true, tail = false`, so the plug-in needs no A7XX
  code. 99/99 host render tests pass.
- Plug-in: `redmagic7/gpu/plugin-ca/HakiosMetalCA.m` (HackiOS's, unchanged) plus `m2v_stubs.c`, which stands in for
  the Metal-on-Vulkan imports; those are reached only under `metal_m2v=1`. Built against our libhakios-metal.a and
  the iOS Mesa archives: 8.3 MB, ad-hoc signed.
- QuartzCore library: our image is iOS 16.7.16 (20H392), and its QuartzCore.framework ships a full default.metallib
  (uber shaders, blurs, copies), unlike HackiOS's 16.5 iPhone X image. So no ca-metallib.py and no lookup hook.
- Kernel: `metal=1` sets MetalPluginName/MetalPluginClassName on MondrianFramebuffer's IOAcceleratorES.
  `rm_kernel_mkdir` (vfs_syscalls.c, mkdir1at with UIO_SYSSPACE) lets rm_blob create parent directories. The blobs
  are /System/Library/Extensions/HakiosMetal.bundle/{HakiosMetal, Info.plist}.
- Boot-args: v26's minus `gltest=1` (no probe in backboardd), plus `metal=1`.
The bundle is written once launchd has remounted / read-write, which may be after backboardd first asks Metal for a
device. If so, the first boot just installs it and the second boot uses it. The files stay in the phone's root.

## v27 result (2026-11-12): Metal device up in the window server

- The plug-in loads in backboardd: "device: Adreno (hakios-metal, chip 0x7030001) on IOAcceleratorES; the compiler is
  in this build".
- CoreAnimation's queue, pipeline library, uber shaders (451 labelled variants, 986 pipelines from
  default.pipelinelib), blurs, home affordance and clears were compiled and linked on the phone.
- Blocker: 34 screen surfaces refused. They are IOSurfaces 'w30r' in MTLPixelFormatBGR10_XR (550/554), which
  hakios-metal does not serve, and the failed render passes and copies follow from that. Cause: the ADT's
  `artwork-display-gamut = 'P3'`, so CoreAnimation renders for a wide-colour screen.
v28: `ADT_SRGB=1` (mkadt.py) gives `artwork-display-gamut = 'sRGB'` (payload/redmagic7-ram-xr-srgb.adt), so the
surfaces should be 8-bit BGRA, a format the plug-in already links pipelines for (80/81). Kernel and boot-args as
v27. If MobileGestalt's cache keeps P3, the next step is clearing that cache.

## v28 result (2026-11-12): CoreAnimation submits through Metal, then the CP faults on a fresh mapping

With `artwork-display-gamut = 'sRGB'` there is no refusal at all. CoreAnimation's uber shaders (gen/spc, cpf/cph),
blurs, luma_boost, home affordance and clears all link. A surface with an odd row pitch (3312 bytes) is served as a
copy. The window server made 25 submissions (fences 2001-2025), then stopped. At fence 2017 the CP reported
CP_INTERRUPT_STATUS 1 and an SMMU read fault (fsr 0x80000402, fsynr0 3) at 0x13e10000. That is the first page of
fence 2021's command buffer, bo 68, mapped just before its submit while the CP was still busy with 2017-2020;
rptr 0x6ba stopped short of wptr 0x6c1. The new IB's translation was still missing when the CP fetched it.
Cause: mondrian_gpu_uc_tlb_flush waited for TLBSYNC with loops of delay(1), and delay does not fire on this board,
so the wait ended after 1000 reads whatever the SMMU was doing.
v29 (kernel): the TLB waits spin on mach_absolute_time (5 ms before, 20 ms after, logged if still active), and the
invalidate adds the context bank's TLBIALL (0x618, walk caches too) to TLBIASID. ADT and boot-args as v28.

## v29 result (2026-11-12): no fault

31 window-server submissions (fences up to 2031), no SMMU fault, no CP interrupt, no "TLB sync still active", nothing
refused. The boot ended at 98 s with the last submission at the end of the log, so the window server was still
submitting. Whether the screen is correct needs the user's eyes, or a screenshot path (next).

The user saw a black screen on v29, though the frames completed and the window server swapped 828x1792 BGRA surfaces
(one with 3328-byte rows, drawn directly; one with 3312-byte rows, through the plug-in's shadow copy).
v30 (kernel):
- `mondrian_gpu_uc_wait` counted its timeout in delay(1000) steps, and delay does not fire here, so every wait (the
  user client's CPU_PREP and WAIT_FENCE) gave up after a few thousand reads. That never showed in the probe's
  microsecond work. CoreAnimation's millisecond frames were read (write-backs, the framebuffer's copy) before the GPU
  finished. Now the deadline is by mach_absolute_time.
- MondrianFramebuffer's swap waits (up to 100 ms) for the newest submitted fence, then cleans and invalidates the
  surface's cache lines (flush_dcache64 on its kernel mapping) before the CPU copies it to the panel.
ADT and boot-args as v28/v29.

## v30 result and v31 (2026-11-12)

v30: still black on the panel; 25 submissions, 16 swaps, no fault. v31 (kernel, diagnostic): MondrianFramebuffer logs, for
the first 40 swaps, the swapped surface sampled on a 16x16 grid (how many pixels are not 0 and not black, an XOR,
the centre) and the panel centre after the copy. A black panel over a drawn surface means the copy is wrong; a
black surface means the drawing is.

## v31 result (2026-11-12): the GPU draws the boot fade; frames stop after it

    MONDRIAN FB: sample swap 1 surface 2 bpr 3328: 256/256 not 0, 2/256 not black, centre 0xffffffff
    MONDRIAN FB: panel after swap 1: centre 0xffffffff
    ... the centre goes ffffffff, fffdfdfd, fff4f4f4, ffe2e2e2, ffc7c7c7, ffa4a4a4, ff6a6a6a, ff464646, ff282828,
    ff121212, ff010101 over 30 swaps

The window server draws the boot logo's fade-out through Metal on the Adreno, and the panel shows exactly what was
drawn: the copy is right, the drawing is right. After the fade (about 30 swaps) no more swaps come. backboardd is
idle, not stuck: its main thread is in its run loop and hakios's CommandQueue waiter waits for work. The lock screen
never comes. The user switched to fastboot at about 100 s.
v32: boot-arg `metal_fps=1` (the plug-in's per-second frame and pass report). The framebuffer samples every 25th
swap after the first 40. The plan is to wait about 4 minutes before fastboot, to tell "nothing more drawn" from
"slow first boot" (shaders compiled on first use).

## v32 result (2026-11-12)

Same as v31 over 161 s: the boot fade drawn and shown (about 30 swaps), then nothing. metal_fps printed no per-second
lines, so there were no command buffers after the fade. backboardd uses the framebuffer exactly as in a software
boot (the same selectors 3/4/5/12/15/17/22/27/29/33/50/55, once each). It is not a missing notification or swap
reply. Next: instrument the render server (HackiOS inject.c logs is_ready / ignore_update_p / begin_update /
finish_update / render_update on CA::WindowServer AppleCLCDDisplay and the server) to see whether the lock
screen updates arrive and are ignored, or never come.

## v33 (2026-11-12): watching the window server's frame decisions

SpringBoard logs the same lines in a software boot and in the Metal boot: it builds its lock screen either way. The
window server stops asking the GPU to draw after the fade. `redmagic7/gpu/ca-watch.c` (MIT, HackiOS inject.c's
replace_virtual technique) replaces six CA::WindowServer virtuals in backboardd: IOMFBDisplay::is_ready /
ignore_update_p / begin_update / finish_update in AppleCLCDDisplay's vtable, and EAGLServer::renderer /
render_update. The addresses are this build's (20H392, `ipsw dyld symaddr ref/ipsw-x/dsc/... --image QuartzCore`),
and each slot is found by value, no offsets assumed. It logs the first calls, every change of answer, and counts
every 2 s for 3 minutes ("RM CAWATCH:").
It is installed as /usr/libexec/mondrian-inject.dylib, which ios-mondrian's UI hook already dlopens (its constructor
is a no-op outside backboardd). rm_blob now leaves a file alone when it already holds the same bytes: rewriting a
dylib that daemons have mapped would change signed pages under them.
On the first boot the watcher may be written after backboardd has started; the second boot has it from the start.

## v33 result (2026-11-12): the frame clock stops, and so does sleep()

- ca-watch loaded in backboardd and replaced all six virtuals (AppleCLCDDisplay slots +0xc0 is_ready, +0x148
  ignore_update_p, +0x188 begin_update, +0x190 finish_update; EAGLServer +0x1b0 renderer, +0x188 render_update).
  None of them was ever called, and its summary thread, `sleep(2)` then print, printed nothing in 230 s.
- The fade stopped part way this time: the centre at ff0a0a0a, the "very dim Apple logo" the user saw. Frames stop
  mid-animation, not after it.
- Kernel timer interrupts do fire (MONDRIAN TIMER samples: irqs rising), but those samples are one CPU's. XNU's
  timer deadlines are per CPU; if the secondary cores' timer interrupt never fires, a thread waiting with a deadline
  there (sleep, a semaphore with a timeout, CoreAnimation's own frame timer, which an iPhone X-class device runs
  instead of the display's vsync) wakes only when something else wakes that core. That is also the "tsleep/delay
  deadlines don't fire" seen in the kernel since the start.
v34: the v33 image with `cpus=1` instead of `cpus=8`. If the fade completes and the lock screen comes, the secondary
CPUs' timer interrupt is the bug to fix (then cpus=8 again).

## v34 result and v35 (2026-11-12)

v34 (cpus=1): far too slow to tell anything. At 105 s, 69 processes and no window server; the console stayed up.
v35 (kernel, diagnostic, cpus=8): every MONDRIAN TIMER sample also prints "MONDRIAN CPUS:", each CPU's own
cpu_stat.timer_cnt (timer interrupts taken) and irq_ex_cnt, from CpuDataEntries (sleh.c mondrian_cpu_counts). If
CPUs 1-7 show no timer interrupts while CPU 0 counts thousands, the secondary cores' timer PPI (27) is the bug
(pe_init_fiq per core: GICR mapping or enable).

## v35 result (2026-11-12): the timers are fine

    MONDRIAN CPUS: 8 with data; timer irqs per cpu 4028 2670 3530 2259 2563 2176 1951 2121; irqs 44223 44805 ...

Every core takes its timer interrupt, and all the counts rise; the secondary-timer guess is wrong. The fade froze at
ff7c7c7c this time (v31/v32 ff010101, v33 ff0a0a0a): a different point each boot, which looks like a race. ca-watch
again printed only its install lines, so its hooks are never called: this Metal path does not go through
AppleCLCDDisplay's or EAGLServer's vtable, or not through these slots. The UI hook's "hot" sampler covers backboardd's
main thread (idle in its run loop) and hakios's CommandQueue waiter (idle in its condition variable), not the
render thread.
Candidate: Metal's 64-slot command-buffer semaphore (HakiosMetalCA.m hakiosPaySlot); a slot never given back
would block -commandBuffer a few dozen frames in, at a point that varies. v36: boot-arg `metal_fps=timeline` (the
plug-in's per-command-buffer created/committed/completed/GPU times), kernel as v35.

## v36 result and v37 (2026-11-12)

v36 (metal_fps=timeline): the fade ran to ff010101 and stopped. The timeline prints only command buffers made 30 s
or more after the first, and it printed none: after the first 30 s no command buffer was made and completed.
v37 adds counters to the plug-in (HakiosMetalCA.m: asked = -commandBuffer calls, made = initialised, owing a slot;
committed; completed; slots paid). A thread prints them as "RM CB t+Ns" every 5 s while they change, and every 30 s
regardless, for 4 minutes. Reading it:
- asked > made with made - paid = 64: blocked in Metal's queue semaphore.
- asked stops growing with few owed: the window server stopped asking (its frame logic).
- committed > completed and stuck: a command buffer never completes.
Boot-args back to metal_fps=1. The bundle's bytes changed, so rm_blob rewrites it this boot.

## v37 result (2026-11-12): no leak; the window server asks for a frame a second, then stops asking

    RM CB t+10s: asked 5, made 5, committed 4, completed 4, slots paid 5 (owed 0)
    RM CB t+25s: asked 29, made 29, committed 28, completed 27, slots paid 28 (owed 1)
    RM CB t+45s: asked 32, made 32, committed 31, completed 31, slots paid 32 (owed 0)
    RM CB t+120s: asked 32 ... (unchanged)

At most 2 slots owed, and every committed buffer completed (the kernel's fence wait returns as soon as the fence
passes). The window server made about one frame per second through the fade, then stopped asking (32 in all; the
fade reached ff000000 this time). The pace is CoreAnimation's own frame clock. QuartzCore runs a timer of its own on
an iPhone X-class device and follows the display's refresh notifications only on A7-A10 phones or with
CA_FORCE_TIMER set (HackiOS inject.c, ca_vbl=1). MondrianFramebuffer sends 60 Hz notifications (type 5), and no
boot has ever turned them on.
v38: ca-watch (mondrian-inject.dylib, loaded by the UI hook while backboardd starts) sets CA_FORCE_TIMER=1 in
backboardd before QuartzCore reads it. If the dylib is rewritten after backboardd has loaded the old one, the
second boot is the test.

## v38 result (2026-11-12): the fade is smooth; the lock screen never arrives

With CA_FORCE_TIMER=1 the window server uses the framebuffer's refresh notifications (type 5 on/off, around each
animation). The fade itself (swaps 7-30) now runs back to back, with no 1 s SMP marker between swaps: it is smooth.
The slow part is before it: the first swaps around t=60-72 s, during startup. After the fade the server is idle,
with no more command buffers (asked 31) and no swaps. SpringBoard logs the same as in a software boot. The open
question is the boot-logo-to-lock-screen hand-off in Metal mode: whether SpringBoard's context updates reach the
render server and are dropped, or never come. Ideas: (1) metal_all=1, giving every process the device (HackiOS's
switch; poster/wallpaper processes may wait for Metal); (2) log CA::WindowServer::Server::render_display /
Display::render_display (symbols in qc-syms) for SpringBoard's context; (3) compare with HackiOS's boot-args
(metal_defer, wallpaper_cache).

## v40 result (2026-11-12): the window server stops on can_update "not yet", with vsync off

ca-watch now writes to stderr (its v38/v39 lines went to /dev/console, which the ring never got from backboardd) and
hooks every WindowServer vtable. AppleCLCDDisplay is the display, EAGLServer the server; is_ready always 1,
ignore_update_p 0, begin/render/finish_update run once a frame. IOMFBDisplay::can_update answers 0 about a third of
the time (the inner can_update_ polls IOMobileFramebufferSwapWait through swap_wait_timeout). The end of the log:

    panel after swap 11 ... backboardd notifications type 5 off ... can_update -> 0 (call 33), then nothing

CoreAnimation turned vsync off after the last frame, asked once more, got "not yet", and the retry waits for a vsync.
v41: kernel boot-arg mondrianfb_vsync_linger=120 keeps vsync going 2 s after it is turned off.

## v41 result (2026-11-12): vsync linger changes nothing

302 lingering vsyncs were sent after CoreAnimation turned vsync off; it never called can_update again and the screen
stayed black (11 swaps, the last at ~t+48 s). The final "not yet" is not what stalls the server: it has no more
updates to draw. SpringBoard is alive and building its UI (UICollectionView/CATransformLayer messages), but its commits
cause no render_update in backboardd. v42: the same kernel and userland without metal=1, as the control (does
software CoreAnimation still reach the lock screen on today's root, and how many render_updates does it make).

## v42 control (2026-11-12): software CoreAnimation reaches the lock screen

Same kernel and root without metal=1: lock screen and home screen (slow). ca-watch: both modes make the same ~9 frames
through t+28 s; then software draws ~8 frames a second (147 render_updates by t+48 s, ignore_update_p always 0,
renderer NULL), while Metal stops at 9-10 with backboardd's command buffers "made 10, committed 9": one made and never
committed. v43: ca-watch dumps every backboardd thread's name and frame chain at t+40 s and t+70 s (Metal on).

## v43 result (2026-11-12): no deadlock; the display thread was in Gpu::newBo, frames trickle

Plug-in base found from CommandQueue::waiter (0x1083e8000 this boot; symbolicated with
scratchpad symt.py: atos for the plug-in, qc-syms for QuartzCore). At t+40 s:

    display.primary: mach_msg <- Gpu::newBo <- newBuffer <- Texture::make <- hakios_metal_texture_new_levels
      <- CA::OGL::MetalContext::create_surface <- RenderSurface::set_dest <- ImagingNode::render
      <- ImagingNode/CacheNode::retain_surface <- prepare_layers_roi
    a plug-in thread: Gpu::wait <- CommandQueue::waiter (a fence)

At t+70 s every CoreAnimation thread is idle in its run loop, and backboardd still makes a command buffer now and then
(13 by t+120 s, 14 by t+130 s; 14 swaps). Not stuck: slow. Each CA cache surface is a new GPU buffer, and the kernel
allocates every buffer below 4 GB (inTaskWithPhysicalMask 0xfffff000) on a 12 GB phone.
v44: kernel prints alloc/map time of any bo over 5 ms; boot-arg mondrian_bo_anyphys=1 lifts the mask (the context
bank's PASize comes from SMMU ID2).

## v44 result (2026-11-12): buffer allocation is not slow

No bo took over 5 ms (alloc + map), with anyphys on; no GPU faults from pages above 4 GB either. Still black,
11 swaps. v45 (anyphys off again): ca-watch samples display.primary 50 times a second from t+20 s to t+80 s and prints
the commonest QuartzCore frame and plug-in offset every 10 s, to see whether that thread is busy or idle between frames.

## v45 result (2026-11-12): the display thread waits seconds in the kernel to make or free a buffer

Profile of display.primary (500 samples a round, ~10 s): t+20 s 198 in Gpu::newBo's IOKit call, 298 idle;
t+30 s 203 newBo, 92 in Bo::~Bo; t+40 s 107 ~Bo; then idle (IOMFBServer::thread_body) to the end. v44 showed the
kernel makes a buffer in under 5 ms, so the call waits before it gets there. The cause: MondrianGPUUserClient sets
none of the IOUserClientDefaultLocking keys and is not an IOSurface dependent, so IOKit turns default locking on --
externalMethod under the client's lock shared, IOConnectMapMemory64 / clientMemoryForType exclusive. The plug-in's
waiter thread sits in WAIT_FENCE (a spin until the GPU is done, up to 5 s) holding it shared, and every buffer the
window server maps or unmaps queues behind it. CPU_PREP also waited for the GPU holding fLock, which gem_new and
gem_close take.
v46: the three default-locking keys set false in initWithTask (fLock already guards the buffer table), CPU_PREP's wait
moved outside fLock, and WAIT_FENCE prints any fence over 50 ms.

## v46 result (2026-11-12): frames flow, then a CP translation fault stops the GPU

The Apple logo stayed over a minute. backboardd made 80 command buffers by t+110 s (14 on v43) and 40 swaps, but
fence 2007 came back with fsr 0x80000402 at 0x13eac200 (an IOVA no buffer was mapped at then; bo 26 got that range
later), and every WAIT_FENCE from 2008 on took the full 5000 ms (kIOReturnTimeout). The uc path's IOVA bump allocator,
page-table pages (mondrian_uc_table_next), TLB flush and ring have no lock of their own: each client's fLock only
separated that client's calls, and backboardd, SpringBoard and chronod mapped and submitted concurrently. IOKit's
default locking had been hiding some of it by slowing everything down. Also, buffers freed while the GPU still read them
had been safe only because their unmap queued behind a WAIT_FENCE.
v47: one IOLock shared by every MondrianGPUUserClient (fLock points at it), and DRM_GEM_CLOSE first waits (outside the
lock, at most 500 ms) for the last fence submitted.

## v47 result (2026-11-12): no fault, but the GPU hangs at the first big frame

The fade runs again, then black. No SMMU fault this time; WAIT_FENCE times out (5000 ms) from fence 2006, and
backboardd's display thread sits in CommandBuffer::waitUntilCompleted under MetalContext::finish_image for the whole
profile (500/500). Fences 2001-2005 are the fade's streams (750-1179 dwords); 2005 onwards are 5900-14400 dwords:
the lock screen's first frames, which hang the CP. v46's translation fault was also on such a frame, so this looks
like something those frames draw that the A7XX translation does not handle, not the locking.
v48: the first WAIT_FENCE timeout of a boot prints RBBM_STATUS/STATUS3, RB rptr/wptr, IB1/IB2 base and remaining
size, CP_HW_FAULT/INTERRUPT/PROTECT_STATUS, the fence word, and 56 dwords around the IB1 and IB2 fetch addresses.

## v48 result (2026-11-12): the 1994-dword stream hangs, every boot

Stuck on the logo this time (the hang came before the fade). HANG fence 2006 (submitted 2008): rbbm 0xed00a7,
status3 0x72000f, rb 5f7/618, IB1 0x12bb4000 rem 170, IB2 0, no HW fault/interrupt/protect. Fence 2006 is a
1994-dword stream -- the same size as v46's fence 2007 that faulted at an IOVA no buffer had yet (0x13eac200). So one
specific early frame, every boot: the CP stops about dword 1824 (1994 - 170, less its prefetch), not inside an IB2.
CP_IB1_BASE stays at the start of the IB (+0), so the window printed was only its first 8 dwords.
v49: the HANG dump prints the whole IB1 stream (up to 2560 dwords) to decode on the host.

## v49 result (2026-11-12): the sixth submission, whatever it holds

HANG fence 2006 again, RB rptr 0x5f7 again (offset 6 of the sixth 13-dword ring entry: right after its
INDIRECT_BUFFER), but this boot's 2006 is a 5931-dword stream in the reused slab 0x10d34000 (2005 used it too),
IB1 rem 3339 (about dword 2592). Decoded with gpu/pm4-decode.py: draws, then a pass tail at 2476-2498 --
WAIT_FOR_IDLE, CCU_CLEAN_COLOR, CACHE_CLEAN writing 1 to the pass table (0x10cd2000), CP_WAIT_REG_MEM for it == 1,
LRZ_FLUSH, CACHE_CLEAN writing 2 at +4, CP_WAIT_MEM_GTE for it -- just before the fetch point. Fences 2006 in v47,
v48 and v49 with different contents point at a wait, not at one frame's content.
v50: the HANG handler walks the IB's packets and, for every CP_WAIT_REG_MEM / CP_WAIT_MEM_GTE, reads the word it
polls now and prints the waits whose condition does not hold (address, value, reference).

## v50 result (2026-11-12): the framebuffer-fetch flush never writes its word

HANG fence 2007 (6947 dwords), IB1 rem 4739 (fetched ~2208). The first wait not met, at dword 1973:
CP_WAIT_REG_MEM == 8 at table + 0x38 (kTableFbReadFlushAt), the word is 0. That is the CCU flush render.cpp puts
before a draw whose fragment shader reads its own target (framebuffer fetch): PC_CCU_FLUSH_COLOR_TS writing
kFbReadFlushValue, then a wait for it. a7xx.cpp turned it into CCU_CLEAN_COLOR and CACHE_CLEAN with WRITE_ENABLED
writing the value; at a pass's end that write lands, inside a pass it never does. The boot fade has no framebuffer
fetch; the lock screen's first frame has.
v51 (plug-in only, same kernel): a CCU *_TS flush becomes the CCU_CLEAN_* event, CP_WAIT_FOR_IDLE, and CP_MEM_WRITE
of the value (the CP writes it itself), as turnip flushes inside a pass with plain events and a WFI. 99/99 host tests.

## v51 result (2026-11-12): still black; the same word still 0

The new plug-in was written (RM BLOB). HANG fence 2006 (16344 dwords), IB1 rem 6328 (fetched ~10016), RBBM_STATUS
0x800005 and STATUS3 0 (the GPU nearly idle: the CP polls). First wait not met: dword 9783, == 8 at a draw table's
+0x38 (the framebuffer-fetch flush), still 0 -- with CP_MEM_WRITE writing it right before, if the translation is
what ran. A wait not met can also be one the CP never reached.
v52: the walker prints the 40 packets before the first wait not met (offset, header, four words).

## v52 result (2026-11-12): an indexed draw right before it; the GPU busy

HANG fence 2006, RBBM_STATUS 0xe500a7 (busy this time; v51's 0x800005 was nearly idle). The packets before the first
wait not met (dword 2347):

    2264-2324  RB/GRAS state (viewport, blend, MRT 0x780 ...)
    2326       CP_DRAW_INDX_OFFSET [7] 00000404 00000001 00000006 ...   TRILIST, DI_SRC_SEL_DMA, 16-bit, 6 indices
    2336       the translation marker
    2339       CP_WAIT_FOR_IDLE
    2340       CP_EVENT_WRITE7 0x21 (CCU_CLEAN_COLOR)
    2342       CP_WAIT_FOR_IDLE
    2343       CP_MEM_WRITE 0x1364c038 <- 8        (v51's write: present)
    2347       CP_WAIT_REG_MEM 0x1364c038 == 8     (still 0)

So the CP never got past the draw or the WFI after it. The fade has only auto-index draws (3-word form); this is the
first DMA-indexed draw (7-word form, indices read from memory), and v46's translation fault was on such a frame. The
packet matches turnip's; PC_RESTART_INDEX / PC_PRIMITIVE_CNTL_0 / VFD_INDEX_OFFSET did not move on A7XX; the index
buffer is kept by the command buffer (render.cpp use()).
Next: on-device draw tests for what this frame adds -- a 16-bit indexed draw, and a framebuffer-fetch draw with its
barrier -- to see which one hangs alone.

v53: the probe's stage 11 (rm-draw-tests.inc): 11a tri_flat's pair through a 16-bit index buffer 0..5 (as 8a, all
4096 the colour), 11b tri_fb_fs (tools/kernels/tri_fb_fs.ll, bitcode header made with Homebrew llvm-as) over a target
filled 40 80 bf ff, wanting 80 bf 40 ff everywhere, sysmem with the barrier. Indexed first, since a hang wedges the
GPU. Probe boot-args as v26 (gltest=1, no metal=1), v51's plug-in translation and v52's kernel HANG walker.

## v53 result (2026-11-12): the indexed draw is fine

Every earlier stage still ok (8a-10d), and 11a, tri_flat through a 16-bit index buffer: 4096 of 4096 the colour.
11b did not run: the phone's AIR reader refused the bitcode ("unsupported module record 20"): Homebrew's llvm-as is
LLVM 23 now; the fixtures in tests/fixtures were made with LLVM 22 (llvm@22 regenerates tri_flat_fs_bc.hpp byte for
byte). Use PATH=/opt/homebrew/opt/llvm@22/bin for `make tests/fixtures/*_bc.hpp`.
v54: tri_fb_fs_bc.hpp from llvm@22, the same probe otherwise.

## v54 result (2026-11-12): framebuffer fetch alone is fine too

11a ok, 11b ok: tri_fb over 40 80 bf ff, 4096 of 4096 re-coloured, with the sysmem barrier (v51's CCU clean, WFI,
CP_MEM_WRITE, wait). 0 failures. Neither of the frame's two new kinds of draw hangs alone, each in a pass of its own.
In CoreAnimation's hung frame the barrier sits in the middle of a pass, after an indexed draw.
v55: 11c, the indexed T1 then the fetching pair in ONE sysmem pass (the barrier after a draw, mid-pass).

## v55 result (2026-11-12): 11c fine as well; the slab reuse is the suspect

11c, the indexed T1 then tri_fb in one sysmem pass: 4032 of 4032 right. 0 failures. No draw this frame adds hangs in
the probe. What the hangs share instead (v49, v51, v52): CoreAnimation's fifth and sixth submissions run from the same
command-buffer slab (0x10d34000): CommandQueue::giveSlabs returns a slab when its buffer completes and takeSlab hands
it to the next, which writes a different stream at the same address. Fences 2001-2004 reused it with near-identical
fade streams; the first frame that differs is the first to hang. The stream's own CACHE_INVALIDATE7 (at its pass
start) cannot cover the fetch of the stream itself.
v56 (kernel, Metal boot-args as v52): every ring entry gets CP_EVENT_WRITE CACHE_INVALIDATE7 (51) before its
INDIRECT_BUFFER (entry 15 dwords).

## v56 result (2026-11-12): not the slab

HANG fence 2006 again, its IB this time at 0x13d00000 (not the reused slab), rem 1058, RBBM busy (0xed00a7). First
wait not met again the framebuffer-fetch flush (dword 2465, == 8 at a draw table's +0x38). So: a draw right before
a fetching draw's barrier keeps the GPU busy, in CoreAnimation's frame, while the probe's indexed + fetch passes
(11a-11c) complete. Next suspect: that draw's own shaders. Mesa's host ir3-disasm built
(ref/mesa-host/build/src/freedreno/isa/ir3-disasm).
v57: the HANG walker tracks SP_VS_OBJ_START / SP_FS_OBJ_START up to the first wait not met and prints 384 dwords of
each program (the cache-invalidate per ring entry stays).

## v57 result (2026-11-12): the stuck draw is instanced

Same hang (fence 2006, the fetch flush's wait at dword 2465 unmet). The draw before it:

    CP_DRAW_INDX_OFFSET [7] 00000406 0000000d 00000004 ...   DI_PT_TRISTRIP, DMA 16-bit, 13 INSTANCES, 4 indices

Every fade draw and every probe test so far had one instance. Its programs (gpu/hang-shaders.py, then
ir3-disasm -c 07030001) are CoreAnimation's uber shaders: a VS with a preamble (stc into c[32..52], shpe) and a long
branch ladder on constants; an FS that opens with shps/getone (a preamble of 902 instructions' skip). Nothing in the
first 192 instructions of each stands out; the fade's specialised uber shaders ran.
v58 (probe build, gltest=1): 11d tri_flat with 13 instances, 11e the same indexed, 11f an indexed TRISTRIP of 4 with
13 instances (completion only).

## v58 result (2026-11-12): instancing is fine too

11d (13 instances), 11e (indexed, 13 instances), 11f (indexed TRISTRIP of 4, 13 instances) all ok; 0 failures. Every
feature of the stuck draw passes alone: indexed, instanced, strip, framebuffer fetch, a mid-pass barrier. What is left
is CoreAnimation's own pipeline and its state. The plug-in notes that an uber shader's loops run as many times as its
configuration buffer's bytes say -- a wrong or unread configuration could keep the GPU busy for good.
v59 (v57's kernel and plug-in): boot-args metal_trace=1 (each kind of draw once: program, constants, target,
textures) and metal_dump=cfg:uber (an uber draw's first 128 bytes of each fragment buffer).

## v59 result (2026-11-12): backboardd's pipelines; the hung draw is the fixed compositor

HANG fence 2006 in backboardd's slab again. The uber shaders are chronod's (widgets), not backboardd's. backboardd
links: fixed_vert_lph_gen+fixed_frag_lph_cpf (QuartzCore's compositor: VS 5 resources with a 152-byte constant
script, FS 24 resources) for pixel formats 81, 80, 10, 43, 115, 70, 30; downsample_blur_vert_lph with downsample_4/8,
narrow_blur_7..27, single_pass_blur_3 (format 80, one 115); std_vert1_lph+home_affordance_base_lph;
path_blit_vert_lph+attachment_clear_frag_lph. The stuck draw's VS (preamble with stc, a branch ladder on constants)
is fixed_vert_lph_gen's shape, the blur VS is small. backboardd's per-draw trace lines did not print (only chronod's
did), so metal_skip may not reach its draws either.
The probe has only drawn into RGBA8Unorm (80). backboardd's compositor draws into 10 (R8Unorm), 30 (RG8Unorm), 43 (?),
70/71 (RGBA8 sRGB), 81 (BGRA8 sRGB) and 115 (RGBA16Float). Next: probe draws into those formats.
(Correction: hakios-metal's 70 is RGBA8Unorm, which the probe uses, and 43 is BGR5A1Unorm.)

v60 (probe build, gltest=1): stage 12, tri_flat's pair into a 64x64 target of BGRA8Unorm (80), BGRA8Unorm_sRGB (81),
R8Unorm (10), RG8Unorm (30), RGBA16Float (115), BGR5A1Unorm (43), the program linked for each; completion and the
texel at (5,10). Run after 11a, before 11d-11f/11b/11c.

## v60 result (2026-11-12): every render-target format completes

    12 BGRA8Unorm (80)       80 40 20 ff        12 R8Unorm (10)          20
    12 BGRA8Unorm_sRGB (81)  bc 89 63 ff        12 RG8Unorm (30)         20 40
    12 RGBA16Float (115)     04 30 04 34 04 38 00 3c   (0.125 0.25 0.5 1.0 as halves)
    12 BGR5A1Unorm (43)      10 91
0 failures. Formats are not it either.

## v61 (built overnight 2026-11-12, not flashed): graphics device buffers as bindless descriptors on A7XX

The lead: v57's dump of the stuck draw's VS opens its preamble with `resinfo.b.untyped.1d.u32.1.imm r63.x, 0` and
`..., 1` -- sizes of device buffers in the 3D IBO table (non-bindless, SP_IBO / SB6_IBO). No probe draw had a device
buffer (8h's positions go through a vertex descriptor), and on A7XX compute's non-bindless IBO path was already broken
(0xa9f2, the A6XX SP_CS_IBO, is SP_CS_BINDLESS_BASE[5] there). A wrong IBO descriptor also explains v46's
translation fault at an address no buffer had.

Change (hakios-metal):
- compiler.cpp: lowerIboToBindless now also runs for graphics stages on gen >= 7 when gGfxBindlessIbo (default on;
  setGfxBindlessIbo(false) restores the A6XX way). The flag is in the stage cache name (layout[3]), so cached
  non-bindless stages are not reused. The descriptor index is the IBO table slot (the FS's already shifted by iboBase).
- render.cpp: if either stage has bindlessIbo: HLSQ_INVALIDATE_CMD GFX_BINDLESS (A6XX bits 14-18, a7xx.cpp widens them
  to 17-24), SP_BINDLESS_BASE[0] = A7XX 0xab0a (a7xx::REG_SP_BINDLESS_BASE_0, untouched by the translation) at the
  table | 64-byte descriptors, CP_LOAD_STATE6 ST6_SHADER SS6_BINDLESS SB6_IBO (address 0), SP_xS_CONFIG BINDLESS_IBO
  per stage; no SP_IBO. The A6XX path is unchanged.
- host test a_bindless_ibo_stage_gets_the_table_as_bindless_set_0 (encoding and the translated stream); 100/100.
- probe stage 13 (rm-draw-tests.inc, after 11a): 13a tri_dpos (VS positions from a device buffer, bindless), 13c
  tri_dbuf (FS colour from a device buffer, bindless; wants ff996633), 13b tri_dpos through the old A6XX SP_IBO -- last,
  as it may be what hangs. Bitcode headers tri_dpos_vs_bc / tri_dbuf_fs_bc made with llvm@22.

Images: payload/shim-boot-v61p.img (probe, gltest=1, Metal off) and payload/shim-boot-v61.img (Metal on, metal_all=1,
the new plug-in). Both kernels keep v56's per-submit CACHE_INVALIDATE7 and v57's HANG walker with the shader dump.
Order: v61p first (does 13a/13c complete, does 13b hang?), then v61.

## v61p / v61 results (2026-11-13): bindless IBO ruled out
- v61p (ring73): probe 0 failures; 13a, 13c (bindless) and 13b (old A6XX SP_IBO) all draw right. The old path works.
- v61 (ring74): the same hang as v59 (ring71), fence 2007, IB1 rem 3833. The walker's "first unmet wait" (dword 2860)
  is behind the CP: the CP had fetched to ~2976 and sat in a CP_WAIT_FOR_IDLE (2849/2852) behind a draw that never
  finished. Several draws were in flight, so the dump could not say which.

## v62 / v63: metal_drawseq, one draw at a time and numbered
- boot-arg metal_drawseq (plug-in, hakios_metal_set_draw_seq): after every draw a CP_WAIT_FOR_IDLE and a marker.
- v62 wrote the count to CP_SCRATCH_REG(7) (0x88a). On the phone the register kept its reset value 0xdeafbead:
  register writes to it from an IB are dropped. Do not use CP scratch registers from user IBs.
- v63: the marker is a CP_MEM_WRITE of 0xd5NNNNNN to a 4 KiB buffer the plug-in makes for it. The kernel walker finds
  the first marker the word has not reached: the draw before it is the one that never finished.
- v63 (ring76) named it: chronod's 3rd draw (indexed TRISTRIP of 4, 13 instances, RenderBox uber_vertex). Its VS
  program's first 16 bytes, and 16 bytes at +0x200 and +0x400, were floats (1,1,0.5,1), (-1,1,0.5,1), (1,-1,0.5,1):
  the corners of a quad, run as instructions. The program's bo began exactly where a 4 MiB bo ended.

## v64 / v65: two real kernel bugs, neither the cause
- v64: 64 KiB unmapped after every bo (mondrian_bo_gap; now off by default, =1 to turn it on) and SMMU FSR/FAR in the
  hang dump. The CP then faulted fetching its own IB (FSR 0x80000402 at the IB's address).
- Cause of that: the CP's SMMU table pool was 0x20000 (32 pages, ~60 MiB of iova) and smmu_map had no bound; iova is a
  bump allocator that never gives addresses back, and boots reach 0x15000000+. Tables past the pool were written
  into someone else's kernel memory, never flushed. v65: 4 MiB pool (the whole 32-bit window), map_page refuses
  past it ("cp tables full"), tables_sync flushes only the pages in use. Hang dump prints tables in use.
- v65 (ring78): 59 of 1024 table pages in use; the same hang, the same corruption at the same offsets -- deterministic,
  so not the tables.
- Open: iova is never reclaimed; a long session will exhaust 0x10000000-0x80000000.

## v66: the cause -- private memory sized for 2 SPs
- bo 11 (4 MiB) is made while uber_vertex+uber_fragment are linked: CommandQueue::privateMemory. Its size was
  perFiber (512) * fibers_per_sp (4096) * kSpCores, and kSpCores was 2 (the A635). The A730 (FD730) has num_ccu 4 =
  4 SP cores (freedreno_devices.py; turnip sizes num_sp_cores * per_sp_size). SPs 2 and 3 wrote their fibers'
  private memory (uber_vertex keeps its quad corners in a private array) past the end, into the program.
- Fix (command.cpp): spCores() = 4 on A7XX, 2 otherwise.
- v66 (ring79): the hang moved from fence 2006 to 2038 (32 more submits). New stall: SMMU fault FSR 0x402 at address 0
  (a read), no frames from chronod yet.

## v67 / v68: the read of address 0
- v67 (ring80, drawseq): chronod's draw 2166 never finished: CP_DRAW_INDX_OFFSET indexed TRILIST, 54 indices, 1
  instance, in IB1 (sysmem), viewport 64 wide. The 40-packet window did not reach the bindings.
- v68: the walker also prints the 640 raw dwords before the marker ("HANG raw N: 8 words").
  gpu/pm4-decode.py reads them (synced on the "drawseq before" packet starts); --zeros lists every 64-bit
  ADDR/BASE register written 0, VFD_FETCH sizes with base 0, CP_LOAD_STATE6 from 0 and the draw's index base.

## v68 result and v69: vertex fetch slot 0 at address 0
- v68 (ring81): chronod's draw 2120 never finished; same SMMU read fault at 0. pm4-decode --zeros found no address
  register written 0 that the draw uses (depth, LRZ, flag buffer and private memory are all off for it). The draw's
  two attributes both read Metal buffer 1 (VFD_DECODE[0..1].IDX 1); VFD_CONTROL_0 FETCH_CNT 2 (the attribute count,
  as turnip/fd6 write it) turns on fetch slots 0 and 1, and slot 0's BASE was never written: 0.
- v69 (render.cpp): every fetch slot up to the highest in use that no attribute reads gets the draw's table as BASE
  and SIZE 0. Counts unchanged (the fake GPU keeps turnip's rule). Host tests 100/100. Normal speed (no drawseq).

## v69 result and v71: more found per run
- v69 (ring82): the same SMMU read fault at 0 (fence 2037): the unused fetch slot was not it, or not alone (kept).
- v70 (built, not flashed): the walker also prints what each CP_LOAD_STATE6 of the window loads from memory.
- v71 boot-args add metal_validate=1 and mondrian_smmu_raz=1 (metal_drawseq=1 kept):
  - kernel: mondrian_smmu_raz clears SCTLR CFRE/CFIE on the CP's context bank: a fault reads 0 / drops the write
    and the GPU goes on; each fence completion prints FSR/FAR when it is not 0x400 (capped at 400 lines). A GPU
    reset after a hang was not built: the CP start loads the ZAP shader, which works once a boot.
  - plug-in/lib (command.cpp Validator, hakios_metal_set_validate): before each submission, every address the stream
    makes the GPU read is checked against the submission's buffers (uses + command bo), GMEM bodies followed:
    VFD fetch slots FETCH_CNT turns on, index buffer, VS/FS programs, private memory, colour/depth targets, and the
    data of every CP_LOAD_STATE6 SS6_INDIRECT (texture/IBO descriptor base and UBWC flag, UBO descriptors). Prints
    "validate: cb N draw K (vs .. fs ..): ..." (400 lines a process). Host tests with it on: 100/100 and no reports
    (a framebuffer-fetch descriptor in a GMEM body has a GMEM offset as base: skipped).

## v71 result (ring83) and v72
- SCTLR now 0x107 (CFRE/CFIE off) and still the same stall: FSR 0x402 FAR 0, drawseq marker 2121 not reached.
  Read-as-zero does not unstick it: whatever reads 0 keeps the draw from finishing (a CP read, or a shader that
  loops on what it read).
- The validator's 400 backboardd lines are all "texture descriptor N (block 4) is all zeros" (slots 0-2, some 4-8);
  no process reported a bad address. stageTextures fills only kTexSampled slots: slots below the highest sampled
  one that the shader does not declare stay zero by design.
- The stuck draw's CP_LOAD_STATE6 of 4 FS texture descriptors: slots 0-2 zero, slot 3 a real 2D texture (base
  0x2d888000). Whether its FS samples 0-2 is the question: the program dump had printed the program in force at
  the first unmet wait (behind the CP), not at the marker.
- v72 (kernel only): the walker tracks SP_VS/FS_OBJ_START up to the drawseq marker, so the dumped programs are the
  stuck draw's. Same boot-args as v71.

## v72 result (ring84) and v73: the UCHE trap base
- The stuck draw (marker 2110, IB1) now with its own programs: VS = an 11-instruction transform; FS = bary,
  alias.tex r40.x <- c0.x, `samb (f16)(xyzw)hr2.x, r0.z, r40.x, s#0, t#3`, (sy)end. It samples slot 3 only (a
  828x1792 RGBA8 texture at 0x2d888000, sampler 0 linear/repeat): the zero slots 0-2 are never read. Nothing in
  either program dereferences a pointer. So address 0 is read by fixed-function hardware.
- Suspect: the UCHE trap base. GPU init wrote KGSL's 0x1fffffffff000 to UCHE_TRAP_BASE / UCHE_WRITE_THRU_BASE --
  outside this SMMU context's 32-bit window -- and before the ZAP, which resets registers to the bootloader's values.
  Out-of-range accesses (VFD fetches past VFD_FETCH SIZE, robust reads) are sent to the trap base.
- v73 (kernel only): a 4 KiB trap page mapped at iova 0x303000 (rwx); UCHE_TRAP_BASE and UCHE_WRITE_THRU_BASE set to
  it at init and again after the ZAP (logged "cp uche trap ..."); the hang dump prints both registers as they are.

## v73 result (ring85): THE LOCK SCREEN DRAWS THROUGH METAL
- "cp uche trap 0x000303000 write-thru 0x000303000" after the ZAP; no WAIT_FENCE timeout, no HANG, no SMMU fault
  line; backboardd 38 render_updates, 39 command buffers committed and completed over 200 s (CA renders on change:
  the lock screen is still), SpringBoard 3, chronod's widgets drawn. The user saw the lock screen.
- Cause of the v66-v72 stall: UCHE_TRAP_BASE at KGSL's 0x1fffffffff000, outside this 32-bit SMMU context: an
  out-of-range access (the indexed draw's fetch past VFD_FETCH SIZE, most likely) faulted at the trap address.
- Validator: only the by-design zero texture slots (402 lines); no address in any process outside its buffers.
- v74: kernel v73, boot-args v57's + mondrian_smmu_raz=1 (no drawseq, no validate): full speed.

## v74 result (ring86): the stall is back without drawseq
- v74 = v73's kernel without metal_drawseq / metal_validate: chronod hangs again (fence 2029), FSR 0x402 FAR 0,
  with the trap base at 0x303000 (read back in the hang dump). So the trap base was not the cause: v73 worked
  because metal_drawseq puts a CP_WAIT_FOR_IDLE after every draw. Something reads address 0 only when draws overlap
  -- a state or register the next draw changes while the one before still runs.
- v75: v74 + metal_drawseq=1 alone (no validator), to confirm the serialisation is what matters and see its speed.

## v75 result (ring87): serialising draws is enough
- v74 + metal_drawseq=1 alone: no fence timeout, no hang, no SMMU fault line all boot. backboardd 62 command
  buffers committed and completed in 130 s, commit-to-completion 3-23 ms on average; SpringBoard and chronod finish.
  So a CP_WAIT_FOR_IDLE between draws is what keeps the address-0 read away: a hazard between consecutive draws.
- (the fps line's "GPU busy" goes negative over long windows: its accounting wraps -- cosmetic, to fix.)

## v75 second boot (ring88): drawseq does NOT fix it -- the hang is intermittent
- Same image as ring87, booted again: chronod hangs at the same draw (marker 2129, IB1 dword 13034 as on ring84),
  FSR 0x402 FAR 0. ring84 (v72) had drawseq too and hung; v73 and the first v75 boot did not. So the address-0 read
  is intermittent and serialisation only changes the odds. Not a between-draws hazard after all (or not only).
- The stuck draw: VS a transform; FS one `samb t#3, s#0` of a 828x1792 RGBA8 texture (screen-sized: likely the
  poster/wallpaper surface), bias from FS c0.x, which this draw does not load (stale). The sampler is linear/repeat,
  MAX_LOD 15.99, MIN_LOD 0 (a6xx.xml: MAX_LOD bits 8-19, MIN_LOD 20-31 -- correct).
- AegirPoster (the poster extension) is exec'd ~28 times a boot: SIGABRT crash loop. If the texture is one of its
  IOSurfaces, it may be torn down under chronod's draw -- to check.
- Next: name the faulting GPU block (FSYNR1 / the client the SMMU reports, as KGSL decodes it), whether t#3 is an
  IOSurface-backed bo and which, and AegirPoster's abort reason.

## Digging (2026-11-13 afternoon): the hang follows AegirPoster
- Every boot that hung (ring81-84, 86, 88) launched AegirPoster before the hang (7-76 times: a crash loop); the two
  that did not hang (ring85, 87) never launched it. AegirPoster dies a few hundred ms after making its Metal device:
  SIGABRT, `std::terminate` <- `_objc_terminate` <- `_dispatch_client_callout` <- FBSSerialQueue block (an uncaught
  exception; symbolised with `ipsw dyld a2s ref/ipsw-x/dsc/20H392__iPhone10,3_6/dyld_shared_cache_arm64 ADDR --slide S`).
- The stuck draw's texture is bo 348, 0x5b0000 = 3328 x 1792 (a 828x1792 BGRA poster image), alive across the hang.
- PridePosterExtension: "refused: a texture: 828x1792, pixel format 250" = Depth16Unorm, not served (texture.hpp has
  Depth32Float and Depth32Float_Stencil8 only) -- to add: format table, DEPTH6_16 in the depth setup, clears, GMEM.
- No SMMU fault line in good boots: the address-0 read comes with the hang only.
- v76: plug-in installs an uncaught-exception handler (objc_setUncaughtExceptionHandler, chained) at device creation:
  "uncaught exception NAME: REASON" + return addresses. Kernel hang dump adds FSYNR1 and UCHE_CLIENT_PF (the faulting
  GPU block, as KGSL's gen7_iommu_fault_block decodes it). Boot-args as v75 (drawseq on).

## v76 result (ring89): no hang, no AegirPoster; the unlock is slow on the CPU
- AegirPoster never launched this boot, and nothing hung (fits the correlation). The user saw the lock screen, then
  parts of frames seconds apart after swiping to unlock. GPU work was quick (commit to completion 0.2-2.6 ms on
  average). backboardd's display thread, sampled: mostly IOMFBServer::thread_body (waiting); the rest
  MetalContext::flush (up to ~20%) and MetalContext::load_pipeline_state (up to ~9%). 84 pipelines linked in
  backboardd, and fixed_vert_lph_gen+fixed_frag_lph_cpf specialisations at 175-212 ms each: first-use pipeline
  creation on the render thread.

## v77: is the stage cache working?
- Specialisations (hakios_metal_program_specialize) already go through compileStage's disk cache (the key has the
  known buffer's bytes), and they run on a background queue: the base program draws meanwhile. Pipeline creation
  (load_pipeline_state, 84 links in backboardd at the unlock) is synchronous on the render thread.
- The cache: HAKIOS_METAL_CACHE = /private/var/db/hakios-metal-cache (on the read-write root: should persist), keyed
  on kCacheBuild = HAKIOS_METAL_CACHE_BUILD (a hash of compiler.cpp, include/*.hpp and the Mesa libraries) -- so a
  build that changes compiler.cpp or a .hpp starts the cache over.
- v77 (c_api.cpp only, so the key is unchanged): every compileStage call timed; the "RM CB t+" line adds "stages N
  (M from the cache), X ms compiling; cache DIR". Boot it twice: the second boot shows whether the cache persists.

## v77 result (ring90): the stage cache works; the slow unlock is not the GPU and not compiles
- Cache: HAKIOS_METAL_CACHE fell back to /var/tmp/hakios-metal-cache (/var/db not writable to the window server) and
  still persists across boots: backboardd 130 of 133 stages from the cache, 543 ms compiling in all; SpringBoard 12/12.
- No hang, no AegirPoster this boot. The unlock animation crawled (2+ min, not 25% done). backboardd: ~10 command
  buffers a second, every gap between commits a "stall" (> 50 ms); GPU time per frame ~2 ms; commit to completion
  ~6 ms; surface write-backs 80-150 ms per 2 s. Display thread sampled ~87% idle in IOMFBServer::thread_body,
  finish_update 6-8 %, flush ~4 %. So the server is waiting to be asked for frames: the pace is set elsewhere
  (SpringBoard's own frame work, the clients' commits, or the vsync notifications that drive the server).
- SiriTTSSynthesizer, assetsd crash-loop (SIGABRT) as before -- unrelated to the GPU.

## v78 / v78b: what paces the frames
- Kernel (MondrianFramebuffer vsyncFired): every 300 vsyncs, "vsync N: 300 in X ms (Y Hz), C clients" -- the vsync
  timer's real rate (the server draws on vsync; delay() is known not to fire on this board).
- ca-watch: in SpringBoard (the UI hook dlopens mondrian-inject.dylib in every daemon it lands in), a sampler of the
  main thread, 50 Hz from t+15 s for 12 rounds: "prof main leaf" / "prof main caller+2", unslid (ipsw dyld a2s).
  In backboardd, boot-arg ca_timer=0 leaves CA_FORCE_TIMER out (v38 set it).
- v78 = timer as before; v78b = ca_timer=0. Both drawseq on.

## v78 result (ring91): vsync fine, SpringBoard idle, AegirPoster's exception named
- Vsync timer: 300 vsyncs in 5.30 s every time = 56.5 Hz (MONDRIAN_FB_VSYNC_US is a little long for 60), steady.
- SpringBoard main thread: idle (mach_msg2_trap / semaphore_wait / kevent_id) in every round. Not the pacer.
- This boot hung (fence 2033), with AegirPoster launched 14 times. The v76 handler caught its abort:
  `CAMetalLayerInvalid: unsupported extended range format`, from -[CAMetalLayer setPixelFormat:] ->
  layer_private_check_pixel_format (QuartzCore 0x1883712dc): for the XR formats (Metal 550-555: BGRA10_XR,
  BGRA10_XR_sRGB, BGR10_XR, BGR10_XR_sRGB) it asks the layer's device -supportsExtendedXR10Formats and raises when NO.
  The caller: -[NUNIAstronomyVistaView initWithFrame:configuration:] -> +[CLKUIQuadView quadViewWithFrame:...] ->
  -[CLKUIMetalQuadView initWithFrame:identifier:options:colorSpace:asyncRenderQueue:] (the Astronomy poster).
- The plug-in does not answer supportsExtendedXR10Formats, and hakios has no XR formats. To serve them: YES from the
  device, and the four formats in the table (Adreno has no XR encoding: BGR10_XR as 10:10:10:2 UNORM and BGRA10_XR as
  16:16:16:16 UNORM would draw, with XR's -0.75..1.25 range clipped/mis-scaled), plus their IOSurface fourccs.

## v79: XR formats served
- texture.hpp/.cpp: BGRA10_XR (552), BGRA10_XR_sRGB (553) as FMT6_16_16_16_16_UNORM, 8 bytes, WXYZ; BGR10_XR (554),
  BGR10_XR_sRGB (555) as FMT6_10_10_10_2_UNORM, 4 bytes, WXYZ, no alpha, RB format 10_10_10_2_UNORM_DEST. No sRGB,
  no XR scaling (the Adreno has neither for these): values outside 0..1 clip. Host test texture_xr_formats_*.
- Plug-in device: -supportsExtendedXR10Formats YES.
- texture.hpp is in the stage cache's key: the first v79 boot compiles every stage again (then cached).
- Image v79: v78's kernel and boot-args (drawseq on, CA_FORCE_TIMER on).

## v79 result (ring92): AegirPoster lives, the hang does not go; the CP reads 0
- AegirPoster: 1 launch, no crash, links aegir_star_vsh+aegir_star_fsh for pixel format 555 (BGR10_XR_sRGB) and
  compiles compute_convert_xrsrgb_to_p3. It refuses (as it should) R8Unorm over 'A008' surfaces (an alpha-only FourCC
  the plug-in's table lacks -- to add).
- Still the hang (fence 2040, marker 2167, IB1 dword 13034). The new fault-block line: FSYNR1 0x0 = master 0 = the
  CP itself (KGSL gen7_iommu_fault_block), UCHE_CLIENT_PF 0x84. A read (FSYNR0 WNR 0) of address 0 by the CP.
- The window before the stuck draw: CP_EVENT_WRITE7 0x26 (LRZ_FLUSH) at the pass start, with GRAS_LRZ_BUFFER_BASE
  and GRAS_LRZ_FAST_CLEAR_BUFFER_BASE 0 (LRZ off). turnip also flushes with LRZ off (tu_lrz_sysmem_end), so this is a
  suspect, not a finding.
- v80: boot-arg metal_nolrzflush drops every LRZ_FLUSH (render.cpp gNoLrzFlush). If the hang goes with AegirPoster
  running, the LRZ flush is it.

## v80 result (ring93): not the LRZ flush
- metal_nolrzflush on in 6 processes, AegirPoster up (1 launch, no crash), and the same hang: fence 2039, FSR 0x402
  FAR 0, FSYNR1 0 (CP), FSYNR0 0x90003 (its bits 16-23 vary between boots: 0x01, 0x08, 0x09). LRZ_FLUSH ruled out.

## v81: capture the stuck submission whole
- Kernel hang dump: the whole IB1 (walked to the end of its packets), every IB2 it calls, and the memory every
  CP_LOAD_STATE6 SS6_INDIRECT and every indexed draw's index buffer reads, as "HANG stream ADDR N dwords" and
  "HANG mem ADDR: 8 words" (deduplicated, at most 20000 lines so the 4 MiB ring keeps the rest of the boot).
- gpu/hang-capture.py LOG [--draw N]: puts the words back at their addresses, walks IB1 with the register state and
  the IB2 calls, lists every draw's reads (index buffer, VFD fetch slots on, programs, texture/sampler tables) and
  every packet that makes the CP read or write memory at 0 or outside the capture.
- Image v81: boot-args v79's (drawseq on, CA_FORCE_TIMER on; no metal_nolrzflush).

## v81 result (ring94) and v82: the binning thread
- The capture worked: 30 streams, 349 KB, 1903 draws walked (capped at 512 ranges / 1024 reads). Nothing reads 0:
  the stuck draw (#1866, ib1+13025, the marker after it at 13034 as every time; CP_IB1_REM 10983 every time) has 54
  16-bit indices 0..35 at 0x1b236900, its 36 vertices (stride 48) ending right where the indices start, VFD slot 0
  the table (size 0), textures and samplers in their tables. CP_BLITs (clears) have valid destinations/flags.
- So the stream is right and the CP still reads 0 there, intermittently. turnip starts every A7XX command buffer with
  CP_THREAD_CONTROL(THREAD = BR | CONCURRENT_BIN_DISABLE) (tu6_init_hw); hakios never does, and the kernel's ME_INIT
  sent CP_THREAD_CONTROL with THREAD 0 (bit 27 alone) and nothing per IB. The binning (BV) thread can then work
  through the stream too, with no visibility-stream state of its own (VSC addresses 0) -- a CP-side read of 0 whose
  timing varies, and FSYNR0 bits 16-23 that vary between boots (0x01, 0x08, 0x09).
- v82 (kernel): every submission's ring entry is now 17 dwords: SET_PROTECTED_MODE 0, CP_THREAD_CONTROL 0x08000001,
  CACHE_INVALIDATE7, INDIRECT_BUFFER, the fence EVENT_WRITE, SET_PROTECTED_MODE 1. Boot-args: v57's +
  mondrian_smmu_raz=1, no drawseq: full speed.

## v82 result (ring95): not the binning thread either
- CP_THREAD_CONTROL(BR | CONCURRENT_BIN_DISABLE) before every IB: still the hang (fence 2038, FSR 0x402 FAR 0,
  FSYNR1 0 = CP, FSYNR0 0x10003), AegirPoster up. 21 fence timeouts. Kept (it is what turnip does).
- The capture (ring95, 2671 draws): no CP_LOAD_STATE6 from a bindless base (SS6_BINDLESS) at all; 13 388 from
  memory (SS6_INDIRECT), 49 direct. So no bindless base read either.
- Ruled out so far for the CP's read of 0: the trap base, draw overlap, the LRZ flush, the BV thread, indirect loads
  (all captured, none at 0), index buffers, IB2 calls, blits. Still: only in boots where AegirPoster runs; never in
  boots where it does not (ring85, 87, 89, 90).

## Blue wallpaper (ring96 software, ring97 v82): AegirPoster is not the trigger
- The user set a plain blue wallpaper on a software boot (v82s). On the next v82 Metal boot AegirPoster never
  launched -- and the GPU still hung: fence 2126 after 297 backboardd command buffers had completed (the user had
  used the phone for a while), FSR 0x402 FAR 0, FSYNR1 0 (CP), FSYNR0 0x3. The stuck IB1 is 39 860 dwords.
- So the address-0 read by the CP is not tied to the Astronomy poster; it came sooner with it (more GPU work at the
  lock screen). It is load- or time-dependent.

## v83: GPU hang recovery
- mondrian_gpu.c mondrian_gpu_uc_recover(): SQE halt, RBBM_SW_RESET, the boot register programming (GBIF, UCHE with
  the trap page, protect, APRIV, HWCG, clock), ring/SQE/rptr bases, ME_INIT after CP_THREAD_CONTROL(BR | no concurrent
  binning), SQE start; ZAP resumed by SCM BOOT set_remote_state (fn 0x4200010a, args 0, 0); ring/SQE/trap published
  again, SET_SECURE_MODE 0; every fence handed out written as passed. At most 50 a boot. MSM_PARAM_FAULTS (0x09) =
  the count (hakios Gpu::faults gives the command buffers in flight up as errors).
- MondrianGPU.cpp WAIT_FENCE: on a timeout, under the global lock, if the fence is still not passed, recover (after the
  first timeout dump and capture). Boot-arg mondrian_recover=0 turns it off.
- Image v83: v57 boot-args + mondrian_smmu_raz=1, full speed.

## v83 result (ring99): hang recovery WORKS
- A hang came (fence 2040, the same CP read of 0). recover 1: ME_INIT drained (rptr 10, trust 0x0), the ZAP resume SCM
  call returned transport -3145801 (not needed: the CP came back non-secure), SET_SECURE_MODE drained (rptr 12 of 12),
  RBBM_STATUS 0x1, fences to 2043 written passed. One fence timeout in the whole boot; backboardd went on to 422
  committed and completed command buffers after it. The intermittent hang now costs ~5 s.
- Rendering itself is visibly broken (user: unlock draws parts of the screen, icons change, ~65% of the screen unused)
  with no refusal or error in the log: next.

## ring99 photos: the broken rendering is scale and placement
- Lock screen: the whole cover sheet drawn small, in a centred rectangle about 45% wide and 75% tall, black around it.
- Home screen: wallpaper and dock right; widgets drawn magnified 2-3x and clipped ("No eve...", a giant clock face in
  the corners); Calendar and Photos icons magnified; the left column of icons cut off at a vertical line.
- Safari favourites: mostly right (a stray blue block top-left, a grey band above the dock).
- So the GPU draws, but some layers come out at the wrong scale or offset -- textures sampled over the wrong extent, or
  positions transformed wrongly -- not missing work. Software CA drew the same screens right.

## v84: metal_sizes
- Plug-in, boot-arg metal_sizes: per draw (the first 600 a process, then every 40th, at most 1500 lines) "sizes: draw N
  PROGRAM target WxH [(surface)] vp X,Y WxH sc X,Y WxH count C inst I; tN WxH fFMT [(surface ID WxH)] ...". To find
  the draws behind the magnified widgets and the shrunk lock screen. Kernel = v83 (recovery on).

## v84 result (ring100): sizes agree; the scale is in the constants
- 1389 size lines. Every backboardd draw has viewport = target and full-target scissor (or a CA clip); the layer texture
  is always slot 3 with a plausible size; no IOSurface-backed fragment textures. A hang came and recover 1 restored the GPU.
- The pipelines take one Float4 attribute (format 31/29): placement and texture-coordinate transforms come from the
  constant buffers. Suspects: specialisation (uber programs folded per buffer-0 bytes: keyed on the exact bytes, looks
  sound) and the constants a draw reads being changed or stale by the time the GPU runs it.
- v84n = v84 + metal_nospec (every draw with the pipeline own program): if the layers come out right, specialisation is it.

## v84n result (ring101): specialisation was one cause; the rest is frames lost to the hang
- metal_nospec: no specialise line. User photo: the left half of the home screen now right (Weather widget, FaceTime,
  Calendar, Photos at their size; on v84 they were magnified too). Right of a vertical seam near x = 414: the same
  icons magnified, as at the start of the unlock zoom.
- Every backboardd pass to the display ran in sysmem (no tiles), so the seam is not a GMEM tile. Two hangs came right
  at the unlock (t+30 s, fences 2040 and 2042), both recovered; recovery gives the command buffers in flight up, so
  those frames never land, and CoreAnimation redraws only what changes after: the region the lost frames covered keeps
  an animation frame. 80+ command buffers completed after.
- So: (1) specialisation (fold buffer-0 bytes into the uber fragment program) draws some layers wrong -- likely the
  key is the bytes at encode time while the GPU (and CoreAnimation, which may write a buffer until commit) has the
  bytes at commit; (2) each recovered hang leaves stale screen regions until they are redrawn.

## v85: surfaces with unaligned rows get aligned rows
- User: v84n (no specialisation) still randomly broken, and not laggy -- just wrong. So not specialisation alone.
- backboardd logs 10-53 surface write-backs per 2 s (~20 ms each, display-sized). A surface whose rows are not a
  multiple of 64 bytes (828 x 4 = 3312, the size clients ask) is drawn through a copy (HakiosMetalCA.m
  -hakiosShadow:): refreshed from the surface at each pass (for a Load), written back at the command buffers
  completion. CoreAnimation queues frame N+1 before N completes: N+1 loads the surface before Ns write-back, then
  writes back N-1s contents over every region it did not redraw -> random old regions on screen.
- Fix (kernel MondrianSurface.cpp): a single-plane surface asked with IOSurfaceBytesPerRow not a multiple of 64 gets
  it rounded up (clients read it back with IOSurfaceGetBytesPerRow); the plug-in then wraps the surface directly, no
  copy. Boot-arg mondrian_surf_align=0 keeps the asked pitch. Image v85: v84 plug-in, specialisation on (default),
  v57 boot-args + mondrian_smmu_raz=1, recovery on.

## v86: the kernel canvas surface too
- ring101 swaps: CoreAnimation alternates surface 2 (backboardds, bpr 3328) and surface 1 (the kernels default canvas,
  bpr 3312 = 828 x 4): every other frame went through the plug-ins copy and its write-back race. v85 aligned only
  client-created surfaces (user: not fixed). v86: MondrianFramebuffer creates the canvas surface with rows rounded
  to 64 bytes (3328); swap() reads the pitch from the surface.
