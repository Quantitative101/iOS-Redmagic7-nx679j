// RedMagic 7: hakios-metal's probe stages 1-4 (copies, fills, ordering, copy speed) against the mondrian kernel's
// MondrianGPU service, as a dylib the UI hook loads with boot-arg gltest=1 (installed as
// /usr/libexec/mondrian-gpu-test.dylib). Output goes to /dev/console, i.e. the kernel's log ring, as "RM METAL:" lines.
// No shader compiler is involved: these stages are command-processor packets (CP_MEMCPY, CP_MEM_WRITE) and fences.
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <random>
#include <stdlib.h>
#include <string>
#include <unistd.h>
#include <vector>
#include "hakios-metal/command.hpp"
#include "hakios-metal/gpu.hpp"
#include "hakios-metal/compute.hpp"
#include "end_only.hpp"
#include "fill_id.hpp"
#include "fill_id_bl.hpp"
#include "hakios-metal/compiler.hpp"
#include "hakios-metal/tests/fixtures/fill_id_bc.hpp"
#include "hakios-metal/tests/fixtures/fill_id_air.hpp"
#include "hakios-metal/tests/fixtures/tri_flat_fs_bc.hpp"
#include "hakios-metal/tests/fixtures/tri_flat_vs_bc.hpp"
#include "hakios-metal/tests/fixtures/tri_fb_fs_bc.hpp"
#include "hakios-metal/tests/fixtures/tri_dpos_vs_bc.hpp"
#include "hakios-metal/tests/fixtures/tri_dbuf_fs_bc.hpp"
#include "hakios-metal/tests/fixtures/tri_cflat_fs_bc.hpp"
#include "hakios-metal/tests/fixtures/tri_color_vs_bc.hpp"
#include "hakios-metal/tests/fixtures/tri_color_fs_bc.hpp"
#include "hakios-metal/tests/fixtures/tri_attr_vs_bc.hpp"
#include "hakios-metal/tests/fixtures/tri_tex_fs_bc.hpp"
#include "hakios-metal/tests/fixtures/tri_pos_vs_bc.hpp"
#include <cmath>
#include <functional>
#include "hakios-metal/a7xx.hpp"
#include "hakios-metal/render.hpp"
#include "hakios-metal/texture.hpp"

using namespace hakios::metal;

namespace {

int console = -1;
int failures = 0;

void say(const char* format, ...) {
    char line[512];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    if (n > 0 && console >= 0) {
        (void)write(console, line, size_t(n) < sizeof(line) ? size_t(n) : sizeof(line) - 1);
    }
}

void report(bool ok, const std::string& what) {
    if (!ok) failures++;
    say("RM METAL: %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
}

void* probe(void*) {
    std::string why;
    std::unique_ptr<Transport> transport;
    // the GPU service comes up during the kernel's boot; give it a while in case the UI is early
    for (int i = 0; i < 60 && transport == nullptr; i++) {
        transport = openKernelGpu(&why);
        if (transport == nullptr) sleep(2);
    }
    if (transport == nullptr) {
        say("RM METAL: no GPU: %s\n", why.c_str());
        return nullptr;
    }
    Gpu gpu(std::move(transport));
    setCompilerChip(0x07030001);   // the Adreno 730 (Mesa FD730), before anything is compiled
    uint64_t id = 0, chip = 0, freq = 0;
    gpu.param(kParamGpuId, &id);
    gpu.param(kParamChipId, &chip);
    gpu.param(kParamMaxFreq, &freq);
    say("RM METAL: GPU %llu, chip id 0x%llx, %llu MHz\n", (unsigned long long)id, (unsigned long long)chip,
        (unsigned long long)(freq / 1000000));
    auto queue = CommandQueue::make(gpu, &why);
    if (queue == nullptr) {
        report(false, "a queue: " + why);
        return nullptr;
    }
    // v20: where each A7XX dispatch writes its CS registers (compute::Options::dumpTo), printed after it
    auto dumpBuf = newBuffer(gpu, 4096, &why);
    auto dumpReset = [&] {
        if (dumpBuf == nullptr || dumpBuf->contents() == nullptr) return;
        auto* d = static_cast<uint32_t*>(dumpBuf->contents());
        for (int i = 0; i < 64; i++) d[i] = 0xdddddddd;
    };
    auto dumpSay = [&](const char* label) {
        if (dumpBuf == nullptr || dumpBuf->contents() == nullptr) return;
        const auto* d = static_cast<const uint32_t*>(dumpBuf->contents());
        say("RM METAL: regs %s: a9b0 %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n",
            label, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
        say("RM METAL: regs %s: a9f0 %08x %08x ibo %08x%08x count %08x hlsq_cs_cntl %08x | after: ibo %08x%08x a9b0 %08x %08x %08x %08x\n",
            label, d[16], d[17], d[19], d[18], d[20], d[21], d[23], d[22], d[24], d[25], d[26], d[27]);
    };
    // v21: the hand-written fill_id with its store through bindless base 0 (tools/kernels/fill_id_bl.asm)
    Kernel fillBl = kernels::fill_id_bl;
    fillBl.bindlessIbo = true;
    const uint64_t mb = 1 << 20;
    auto a = newBuffer(gpu, mb, &why), b = newBuffer(gpu, mb, &why);
    if (a == nullptr || b == nullptr || a->contents() == nullptr || b->contents() == nullptr) {
        report(false, "two buffers of a megabyte: " + why);
        return nullptr;
    }
    // 1. a copy of 500000 bytes at an offset, checked byte for byte and around its edges
    {
        std::vector<uint32_t> pattern(mb / 4);
        std::mt19937 rng(1);
        for (auto& w : pattern) w = rng();
        memcpy(a->contents(), pattern.data(), mb);
        memset(b->contents(), 0, mb);
        auto cb = queue->commandBuffer();
        bool recorded = cb->copy(*a, 4096, *b, 8192, 500000);
        cb->commit();
        cb->waitUntilCompleted();
        std::vector<uint8_t> back(mb);
        memcpy(back.data(), b->contents(), mb);
        bool same = memcmp(back.data() + 8192, reinterpret_cast<uint8_t*>(pattern.data()) + 4096, 500000) == 0;
        bool edges = true;
        for (size_t i = 0; i < 8192; i++) edges = edges && back[i] == 0;
        for (size_t i = 8192 + 500000; i < mb; i++) edges = edges && back[i] == 0;
        report(recorded && cb->status() == CommandBuffer::kCompleted && same && edges,
               "CP_MEMCPY of 500000 bytes: " + std::string(same ? "same bytes" : "bytes DIFFER") +
                   (edges ? ", edges untouched" : ", edges CHANGED") +
                   (cb->error().empty() ? "" : " (" + cb->error() + ")") + ", " +
                   std::to_string(cb->gpuNanoseconds() / 1000) + " us");
    }
    // 2. fills of awkward lengths at an offset
    for (uint64_t len : {4ull, 256ull, 260ull, 65540ull, 999996ull}) {
        memset(b->contents(), 0x11, mb);
        auto cb = queue->commandBuffer();
        cb->fill(*b, 16, len, 0xa5);
        cb->commit();
        cb->waitUntilCompleted();
        std::vector<uint8_t> back(mb);
        memcpy(back.data(), b->contents(), mb);
        bool ok = back[15] == 0x11 && (16 + len >= mb || back[16 + len] == 0x11);
        for (uint64_t i = 16; i < 16 + len; i++) ok = ok && back[i] == 0xa5;
        report(ok && cb->status() == CommandBuffer::kCompleted,
               "fill of " + std::to_string(len) + " bytes at 16" + (cb->error().empty() ? "" : " (" + cb->error() + ")"));
    }
    // 3. forty command buffers in flight: handlers in commit order, memory as the last left it
    {
        std::vector<int> order;
        std::mutex m;
        std::vector<std::shared_ptr<CommandBuffer>> all;
        for (int i = 0; i < 40; i++) {
            auto cb = queue->commandBuffer();
            cb->fill(*b, 4 * uint64_t(i), 4, uint8_t(i + 1));
            cb->addCompletedHandler([&, i](CommandBuffer&) {
                std::lock_guard<std::mutex> g(m);
                order.push_back(i);
            });
            cb->commit();
            all.push_back(cb);
        }
        for (auto& cb : all) cb->waitUntilCompleted();
        bool inOrder = order.size() == 40;
        for (int i = 0; inOrder && i < 40; i++) inOrder = order[i] == i;
        std::vector<uint8_t> back(160);
        memcpy(back.data(), b->contents(), 160);
        bool mem = true;
        for (int i = 0; i < 40; i++) mem = mem && back[4 * i] == i + 1;
        report(inOrder && mem, std::string("40 command buffers in flight: handlers ") +
                                   (inOrder ? "in order" : "OUT OF ORDER") + ", memory " + (mem ? "as written" : "WRONG"));
    }
    // 4. copy speed: 16 MB five times, best
    {
        auto big1 = newBuffer(gpu, 16 * mb, &why), big2 = newBuffer(gpu, 16 * mb, &why);
        if (big1 && big2) {
            int64_t best = 0;
            bool ok = true;
            std::string err;
            for (int i = 0; i < 5 && ok; i++) {
                auto cb = queue->commandBuffer();
                cb->copy(*big1, 0, *big2, 0, 16 * mb);
                cb->commit();
                cb->waitUntilCompleted();
                ok = cb->status() == CommandBuffer::kCompleted;
                err = cb->error();
                if (ok && (best == 0 || cb->gpuNanoseconds() < best)) best = cb->gpuNanoseconds();
            }
            report(ok, ok && best > 0 ? "16 MB by CP_MEMCPY in " + std::to_string(best / 1000) + " us at best (" +
                                            std::to_string(int(16.0 * mb / best * 1000)) + " MB/s)"
                                      : "16 MB by CP_MEMCPY: " + err);
        } else {
            report(false, "two buffers of 16 MB: " + why);
        }
    }
    // 5. compute on the A7XX path: the first shader instructions on this GPU. end_only must complete and write nothing;
    // fill_id (16 groups of 64) must write gid * 0x9E3779B1 + c into every one of 1024 words. Kernels assembled for
    // FD730 (tests/fixtures-a730, identical words to the a6xx build). No tail events of the driver's own: the kernel's
    // fence closes the submission (hakios-metal's --notail), one A7XX difference fewer for the first try.
    {
        compute::Options a7;
        a7.a7xx = true;
        a7.tail = false;
        const uint32_t words = 1024, c = 0x00001234, canary = 0xc0ffee00;
        auto out = newBuffer(gpu, 4 * words, &why);
        if (out == nullptr || out->contents() == nullptr) {
            report(false, "a buffer for the dispatches: " + why);
        } else {
            auto prefill = [&] {
                auto* w = static_cast<uint32_t*>(out->contents());
                for (uint32_t i = 0; i < words; i++) w[i] = canary;
            };
            prefill();
            {
                const uint32_t g[3] = {1, 1, 1};
                auto cb = queue->commandBuffer();
                dumpReset();
                compute::Options d7 = a7;
                d7.dumpTo = dumpBuf ? dumpBuf->gpuAddress() : 0;
                bool rec = cb->dispatch(kernels::end_only, g, nullptr, std::vector<Binding>{Binding{dumpBuf.get(), 0}}, nullptr, d7);
                cb->commit();
                cb->waitUntilCompleted();
                dumpSay("end_only");
                const uint32_t* got = static_cast<const uint32_t*>(out->contents());
                uint32_t untouched = 0;
                for (uint32_t i = 0; i < words; i++) untouched += got[i] == canary;
                report(rec && cb->status() == CommandBuffer::kCompleted && untouched == words,
                       std::string("A7XX dispatch of end_only: ") +
                           (cb->status() == CommandBuffer::kCompleted ? "completed" : "did NOT complete") + ", " +
                           std::to_string(untouched) + " of 1024 words untouched" +
                           (cb->error().empty() ? "" : " (" + cb->error() + ")") + ", " +
                           std::to_string(cb->gpuNanoseconds() / 1000) + " us");
            }
            prefill();
            {
                const uint32_t g[3] = {16, 1, 1};
                uint32_t imm[4] = {c, 0, 0, 0};
                auto cb = queue->commandBuffer();
                dumpReset();
                compute::Options d7 = a7;
                d7.dumpTo = dumpBuf ? dumpBuf->gpuAddress() : 0;
                say("RM METAL: stage 5 fill_id out at 0x%llx\n", (unsigned long long)out->gpuAddress());
                bool rec = cb->dispatch(fillBl, g, nullptr, std::vector<Binding>{Binding{out.get(), 0}, Binding{dumpBuf.get(), 0}}, imm, d7);
                cb->commit();
                cb->waitUntilCompleted();
                dumpSay("stage 5 fill_id");
                const uint32_t* got = static_cast<const uint32_t*>(out->contents());
                uint32_t right = 0, untouched = 0;
                for (uint32_t i = 0; i < words; i++) {
                    right += got[i] == i * 0x9E3779B1u + c;
                    untouched += got[i] == canary;
                }
                char text[128];
                snprintf(text, sizeof text, " (word 0 %08x want %08x, word 1023 %08x want %08x)", got[0], c,
                         got[1023], 1023u * 0x9E3779B1u + c);
                report(rec && cb->status() == CommandBuffer::kCompleted && right == words,
                       std::string("A7XX dispatch of fill_id (bindless IBO), 16x64: ") + std::to_string(right) + " of 1024 right, " +
                           std::to_string(untouched) + " untouched" + text +
                           (cb->error().empty() ? "" : " (" + cb->error() + ")") + ", " +
                           std::to_string(cb->gpuNanoseconds() / 1000) + " us");
            }
        }
    }
    // v18: on a given queue; handWritten = the stage-5 fixture (c from its immediates, its own local size)
    // fix: bit 0 = also invalidate the CS bindless bases, bit 1 = IBO descriptors in the packet (v19)
    auto dispatchOn = [&](decltype(queue)& q, const Kernel& k, const std::string& label, bool handWritten, int fix) {
        compute::Options a7;
        a7.a7xx = true;
        a7.tail = false;
        a7.a7xxInvalidateBindless = (fix & 1) != 0;
        a7.iboDirect = (fix & 2) != 0;
        a7.dumpTo = dumpBuf ? dumpBuf->gpuAddress() : 0;
        dumpReset();
        const uint32_t words = 1024, c = 0x00004321, canary = 0xc0ffee00;
        auto out = newBuffer(gpu, 4 * words, &why);
        auto cbuf = newBuffer(gpu, 64, &why);
        if (out == nullptr || cbuf == nullptr) {
            report(false, "buffers for " + label + ": " + why);
            return;
        }
        auto* w = static_cast<uint32_t*>(out->contents());
        for (uint32_t i = 0; i < words; i++) w[i] = canary;
        static_cast<uint32_t*>(cbuf->contents())[0] = c;
        const uint32_t g[3] = {16, 1, 1}, local[3] = {64, 1, 1};
        auto cb = q->commandBuffer();
        uint32_t imm[4] = {c, 0, 0, 0};
        bool rec = handWritten
                       ? cb->dispatch(k, g, nullptr, std::vector<Binding>{Binding{out.get(), 0}, Binding{dumpBuf.get(), 0}}, imm, a7)
                       : cb->dispatch(k, g, local, std::vector<Binding>{Binding{out.get(), 0}, Binding{cbuf.get(), 0}, Binding{dumpBuf.get(), 0}}, nullptr, a7);
        cb->commit();
        cb->waitUntilCompleted();
        dumpSay(label.c_str());
        const uint32_t* got = static_cast<const uint32_t*>(out->contents());
        uint32_t right = 0, untouched = 0;
        for (uint32_t i = 0; i < words; i++) {
            right += got[i] == i * 0x9E3779B1u + c;
            untouched += got[i] == canary;
        }
        char text[192];
        snprintf(text, sizeof text, ", %u untouched (word 0 %08x want %08x, word 1 %08x want %08x; out at 0x%llx, c at 0x%llx)",
                 untouched, got[0], c, got[1], 0x9E3779B1u + c, (unsigned long long)out->gpuAddress(),
                 (unsigned long long)cbuf->gpuAddress());
        report(rec && cb->status() == CommandBuffer::kCompleted && right == words,
               "A7XX dispatch of " + label + ": " + std::to_string(right) + " of 1024 right" + text +
                   (rec ? "" : " (not recorded)") + (cb->error().empty() ? "" : " (" + cb->error() + ")") + ", " +
                   std::to_string(cb->gpuNanoseconds() / 1000) + " us");
    };
    // 6. the compiler on the phone: fill_id.ll (AIR bitcode, as a .metallib function would be) compiled here for the
    // Adreno 730 by HackiOS's AIR front end and Mesa's ir3, then dispatched on the A7XX path: c from buffer 1.
    if (!hasCompiler()) {
        report(false, "compiler: this build has none (MESA=1)");
    } else {
        std::string cwhy;
        auto t0 = std::chrono::steady_clock::now();
        auto compiled = compileAir(kernels::fill_id_bc, kernels::fill_id_bc_size, &cwhy);
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        if (compiled == nullptr) {
            report(false, "compile fill_id.ll on the phone for FD730: " + cwhy);
        } else {
            const Kernel& k = compiled->kernel;
            report(true, "compile fill_id.ll on the phone for FD730: " + std::to_string(k.numWords) + " words, instrlen " +
                             std::to_string(k.instrlen) + ", constlen " + std::to_string(k.constlen) + ", in " +
                             std::to_string(us / 1000) + " ms");
            // v15: the same dispatch three ways. (a) as compiled (FD730: an early preamble with a descriptor
            // prefetch); (b) the same words with SP_CS_CTRL_REG0.EARLYPREAMBLE clear, so the preamble runs as an
            // ordinary one; (c) HackiOS's A635 build of the same kernel (no preamble at all), whose plumbing --
            // driver params, the constant range from buffer 1, IBO 0 -- is the same as (a)'s.
            Kernel late = k;
            late.earlyPreamble = false;
            // v18: v17 still wrote through stage 5's freed output (a stale stream or stale state at a reused
            // address?). (a) the hand-written fill_id again on the same queue, fresh buffers; (b) the compiled
            // one on the same queue; (c) the compiled one on a NEW queue (new slabs, new addresses); (d) the
            // hand-written one on that new queue; (e) the A635 build on the new queue.
            (void)late;
            // v19: v18 showed it is not the compiled kernels: after stage 5's fill_id, EVERY dispatch (the same
            // hand-written one included, on a new queue too) stores through stage 5's output. In order, so each
            // result reads against the state the one before it left: plain (expect the stale write), then each fix
            // candidate, then both, then the compiled kernels with both, then plain again (does a fix have to be
            // sent every time?).
            // v21: bindless IBOs. The hand-written one again, the phone-compiled one (lowered to bindless on A7XX:
            // say so), with the early preamble off, and last the A635 build, which is not bindless
            say("RM METAL: phone-compiled fill_id: bindlessIbo %d, earlyPreamble %d\n", int(k.bindlessIbo), int(k.earlyPreamble));
            dispatchOn(queue, fillBl, "hand-written fill_id (bindless) again", true, 0);
            dispatchOn(queue, k, "the phone-compiled fill_id", false, 0);
            dispatchOn(queue, late, "the phone-compiled fill_id, early preamble off", false, 0);
            dispatchOn(queue, fillBl, "hand-written fill_id (bindless), third time", true, 0);
            dispatchOn(queue, kernels::fill_id_air, "the A635-compiled fill_id (not bindless)", false, 0);
        }
    }
    // 7. the first draw on the Adreno 730: tri_flat (VS and FS compiled here for FD730), one triangle into a 64x64
    // RGBA8Unorm target in sysmem, through render.cpp's A6XX encoder with every stream rewritten for A7XX
    // (a7xx::translateA7xx, on from here: stages 1-6 ran without it). Metal's orientation puts the triangle in the
    // lower-left half (pixel (i, j) inside iff j > i); the 64 diagonal pixels are left to the fill rule.
    if (hasCompiler()) {
        a7xx::setA7xx(true);
        std::string vwhy, fwhy, lwhy;
        auto vs = compileStage(kernels::tri_flat_vs_bc, kernels::tri_flat_vs_bc_size, &vwhy);
        auto fs = compileStage(kernels::tri_flat_fs_bc, kernels::tri_flat_fs_bc_size, &fwhy);
        std::unique_ptr<CompiledProgram> prog;
        if (vs == nullptr || fs == nullptr) {
            report(false, "compile tri_flat on the phone: " + (vs ? fwhy : vwhy));
        } else {
            prog = linkProgram(std::shared_ptr<CompiledStage>(std::move(vs)), std::shared_ptr<CompiledStage>(std::move(fs)),
                               PixelFormat::kRGBA8Unorm, &lwhy);
            report(prog != nullptr, "compile and link tri_flat on the phone for FD730" + (prog ? std::string() : ": " + lwhy));
        }
        TextureDesc d;
        d.format = PixelFormat::kRGBA8Unorm;
        d.width = 64;
        d.height = 64;
        d.usage = kTextureUsageRenderTarget | kTextureUsageRead;
        std::string twhy;
        auto target = prog ? Texture::make(gpu, d, &twhy) : nullptr;
        if (prog != nullptr && target == nullptr) report(false, "a 64x64 RGBA8Unorm target: " + twhy);
        if (prog != nullptr && target != nullptr) {
            const uint32_t colour = 0xff804020u, canary = 0xa5a5a5a5u;
            auto* px = static_cast<uint32_t*>(target->contents());
            for (uint64_t i = 0; i < target->buffer()->length() / 4; i++) px[i] = canary;
            RenderPass pass;
            pass.color0 = target.get();
            Draw dr;
            dr.vertexStart = 0;
            dr.vertexCount = 3;
            dr.viewport = Viewport{0, 0, 64, 64, 0, 1};
            render::Options ro;
            ro.gmem = false;
            auto cb = queue->commandBuffer();
            bool rec = cb->draw(prog->program, pass, dr, Bindings{}, Bindings{}, ro);
            if (rec) {
                cb->commit();
                cb->waitUntilCompleted();
            }
            uint32_t inside = 0, insideRight = 0, outside = 0, outsideCanary = 0, edgeColour = 0, other = 0, firstOther = 0;
            const uint64_t pitch = target->bytesPerRow() / 4;
            for (uint32_t j = 0; j < 64; j++) {
                for (uint32_t i = 0; i < 64; i++) {
                    const uint32_t v = px[j * pitch + i];
                    if (j > i) {
                        inside++;
                        insideRight += v == colour;
                    } else if (j < i) {
                        outside++;
                        outsideCanary += v == canary;
                    } else {
                        edgeColour += v == colour;
                    }
                    if (v != colour && v != canary && other++ == 0) firstOther = v;
                }
            }
            char text[256];
            snprintf(text, sizeof text,
                     ": %u of %u inside the colour, %u of %u outside canary, diagonal %u of 64 the colour, %u other (first %08x), "
                     "pixel (0,63) %08x, (63,0) %08x",
                     insideRight, inside, outsideCanary, outside, edgeColour, other, firstOther, px[63 * pitch], px[63]);
            report(rec && cb->status() == CommandBuffer::kCompleted && insideRight == inside && outsideCanary == outside && other == 0,
                   std::string("A7XX draw of tri_flat, one triangle into 64x64 RGBA8Unorm") + (rec ? "" : " (not recorded)") +
                       (cb->status() == CommandBuffer::kCompleted ? "" : " (did NOT complete)") + text +
                       (cb->error().empty() ? "" : " (" + cb->error() + ")") + ", " +
                       std::to_string(cb->gpuNanoseconds() / 1000) + " us");
        }
#include "rm-draw-tests.inc"
        a7xx::setA7xx(false);
    }
    say("RM METAL: done, %d failure(s)\n", failures);
    return nullptr;
}

__attribute__((constructor)) void start() {
    // the UI hook loads this into every daemon it lands in: run once, in backboardd
    if (strcmp(getprogname(), "backboardd") != 0) return;
    console = open("/dev/console", O_WRONLY | O_NOCTTY);
    say("RM METAL: probe loaded in %s[%d]\n", getprogname(), getpid());
    pthread_t thread;
    if (pthread_create(&thread, nullptr, probe, nullptr) == 0) pthread_detach(thread);
}

}  // namespace
