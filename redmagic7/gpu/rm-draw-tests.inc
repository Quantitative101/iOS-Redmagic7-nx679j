// RedMagic 7 probe, stage 8: the rest of hakios-metal's first-draw checks (its tools/probe.cpp stage 9) on the Adreno 730,
// every stage compiled here for FD730 and every stream rewritten for A7XX (a7xx::translateA7xx). Included by
// rm-metal-probe.cpp inside its run function, where gpu, queue, report and the compiler are in scope. No Clear: the
// colour targets are filled by the CPU, and the depth test's "clear" is a draw (Always + write at z 1), because a
// Clear goes through CP_BLIT and the 2D engine, not yet ported (ios-mondrian found the vendor SQE refusing CP_BLIT).
{
    const uint32_t W = 64, H = 64;
    const uint32_t flatColour = 0xff804020u;
    std::string why8;
    auto program = [&](const uint8_t* vsBc, size_t vsSize, const uint8_t* fsBc, size_t fsSize, const char* label)
        -> std::shared_ptr<CompiledProgram> {
        std::string w1, w2, w3;
        auto v = compileStage(vsBc, vsSize, &w1);
        auto f = compileStage(fsBc, fsSize, &w2);
        if (v == nullptr || f == nullptr) {
            report(false, std::string(label) + ": compile on the phone: " + (v ? w2 : w1));
            return nullptr;
        }
        auto p = linkProgram(std::shared_ptr<CompiledStage>(std::move(v)), std::shared_ptr<CompiledStage>(std::move(f)),
                             PixelFormat::kRGBA8Unorm, &w3);
        if (p == nullptr) report(false, std::string(label) + ": link: " + w3);
        return std::shared_ptr<CompiledProgram>(std::move(p));
    };
    TextureDesc td;
    td.format = PixelFormat::kRGBA8Unorm;
    td.width = W;
    td.height = H;
    td.usage = kTextureUsageRenderTarget | kTextureUsageRead;
    auto t = Texture::make(gpu, td, &why8);
    if (t == nullptr) report(false, "stage 8: a 64x64 target: " + why8);
    auto px = [&](uint32_t i, uint32_t j) { return static_cast<const uint32_t*>(t->contents())[j * (t->bytesPerRow() / 4) + i]; };
    auto fill = [&](uint32_t word) {
        auto* w = static_cast<uint32_t*>(t->contents());
        for (uint64_t i = 0; i < t->buffer()->length() / 4; i++) w[i] = word;
    };
    render::Options ro;
    ro.gmem = false;
    auto drawOf = [](uint32_t start, uint32_t count, Viewport vp = Viewport{0, 0, 64, 64, 0, 1}) {
        Draw d;
        d.vertexStart = start;
        d.vertexCount = count;
        d.viewport = vp;
        return d;
    };
    // one draw, committed and waited for; "" when it completed, else why not
    auto run = [&](const Program& p, const Draw& d, const Bindings& vsb, const Bindings& fsb, RenderPass* passIn = nullptr) {
        RenderPass pass;
        if (passIn) pass = *passIn; else pass.color0 = t.get();
        auto cb = queue->commandBuffer();
        if (!cb->draw(p, pass, d, vsb, fsb, ro)) return std::string("not recorded: ") + cb->error();
        cb->commit();
        cb->waitUntilCompleted();
        if (cb->status() != CommandBuffer::kCompleted) return std::string("did NOT complete: ") + cb->error();
        return std::string();
    };
    // count pixels against WANT(i, j) (0 = not judged)
    auto judge = [&](const std::function<uint32_t(uint32_t, uint32_t)>& want, std::string* first) {
        uint32_t right = 0, judged = 0;
        for (uint32_t j = 0; j < H; j++) {
            for (uint32_t i = 0; i < W; i++) {
                const uint32_t w = want(i, j);
                if (w == 0) continue;
                judged++;
                if (px(i, j) == w) {
                    right++;
                } else if (first->empty()) {
                    char b[96];
                    snprintf(b, sizeof b, "; first off (%u,%u): %08x, wanted %08x", i, j, px(i, j), w);
                    *first = b;
                }
            }
        }
        return std::make_pair(right, judged);
    };
    const uint32_t canary = 0xa5a5a5a5u;
    auto flat = t ? program(kernels::tri_flat_vs_bc, kernels::tri_flat_vs_bc_size, kernels::tri_flat_fs_bc, kernels::tri_flat_fs_bc_size, "tri_flat") : nullptr;
    if (t && flat) {
        // 8a. the pair: every pixel the colour (each diagonal centre painted by exactly one triangle)
        fill(canary);
        std::string e = run(flat->program, drawOf(0, 6), Bindings{}, Bindings{}), first;
        auto r = judge([&](uint32_t, uint32_t) { return flatColour; }, &first);
        report(e.empty() && r.first == r.second, "8a tri_flat, the pair: " + std::to_string(r.first) + " of 4096 the colour" + first + (e.empty() ? "" : " (" + e + ")"));
        // 8b. the second triangle alone (vertexStart 3): the upper-right half, i > j
        fill(canary);
        e = run(flat->program, drawOf(3, 3), Bindings{}, Bindings{});
        first.clear();
        r = judge([&](uint32_t i, uint32_t j) { return i > j ? flatColour : j > i ? canary : 0u; }, &first);
        report(e.empty() && r.first == r.second, "8b tri_flat from vertex 3 (upper-right alone): " + std::to_string(r.first) + " of " + std::to_string(r.second) + " right" + first + (e.empty() ? "" : " (" + e + ")"));
        // 8c. a half viewport (0, 0, 32, 64): inside iff i < 32 and j >= 2i + 1
        fill(canary);
        e = run(flat->program, drawOf(0, 3, Viewport{0, 0, 32, 64, 0, 1}), Bindings{}, Bindings{});
        first.clear();
        r = judge([&](uint32_t i, uint32_t j) { return i < 32 && j >= 2 * i + 1 ? flatColour : canary; }, &first);
        report(e.empty() && r.first == r.second, "8c tri_flat in a half viewport: " + std::to_string(r.first) + " of 4096 right" + first + (e.empty() ? "" : " (" + e + ")"));
    }
    // 8d. a constant buffer: tri_cflat_fs's colour from [[buffer(0)]], two draws with two colours
    auto cflat = t ? program(kernels::tri_flat_vs_bc, kernels::tri_flat_vs_bc_size, kernels::tri_cflat_fs_bc, kernels::tri_cflat_fs_bc_size, "tri_cflat") : nullptr;
    if (t && cflat) {
        const uint8_t colours[2][4] = {{0x11, 0x22, 0x33, 0xff}, {0xee, 0xdd, 0xcc, 0x80}};
        for (int k = 0; k < 2; k++) {
            const uint32_t word = uint32_t(colours[k][0]) | uint32_t(colours[k][1]) << 8 | uint32_t(colours[k][2]) << 16 | uint32_t(colours[k][3]) << 24;
            auto buf = newBuffer(gpu, 16, &why8);
            if (buf == nullptr) break;
            float* f = static_cast<float*>(buf->contents());
            for (int c = 0; c < 4; c++) f[c] = float(colours[k][c]) / 255.0f;
            fill(canary);
            Bindings fsb;
            fsb.buffers = {Binding{buf.get(), 0}};
            std::string e = run(cflat->program, drawOf(0, 3), Bindings{}, fsb), first;
            auto r = judge([&](uint32_t i, uint32_t j) { return j > i ? word : j < i ? canary : 0u; }, &first);
            char b[64];
            snprintf(b, sizeof b, "8d tri_cflat, colour %08x from a constant buffer: ", word);
            report(e.empty() && r.first == r.second, std::string(b) + std::to_string(r.first) + " of " + std::to_string(r.second) + " right" + first + (e.empty() ? "" : " (" + e + ")"));
        }
    }
    // 8e/8f. per-vertex colours (tri_color, from vertex_id) and the same through a vertex descriptor (tri_attr): every
    // off-diagonal pixel within one unit of its barycentric weights (hakios's 9d rule)
    auto colourPair = [&](const Program& p, const Bindings& vsb, const std::string& label) {
        fill(0x00a5a5a5u);
        std::string e = run(p, drawOf(0, 6), vsb, Bindings{});
        uint32_t within = 0, counted = 0, maxDev = 0;
        std::string first;
        for (uint32_t j = 0; j < H; j++) {
            for (uint32_t i = 0; i < W; i++) {
                if (i == j) continue;
                counted++;
                const double x = i + 0.5, y = j + 0.5;
                double l[3];
                if (j > i) {
                    l[1] = x / 64.0;
                    l[0] = (y - x) / 64.0;
                    l[2] = 1.0 - y / 64.0;
                } else {
                    l[0] = y / 64.0;
                    l[1] = (x - y) / 64.0;
                    l[2] = 1.0 - x / 64.0;
                }
                const uint32_t v = px(i, j);
                bool ok = true;
                for (uint32_t c = 0; c < 4; c++) {
                    const uint32_t got = (v >> (8 * c)) & 0xff, want = c == 3 ? 255 : uint32_t(std::lround(255.0 * l[c]));
                    const uint32_t dev = got > want ? got - want : want - got;
                    if (dev > maxDev) maxDev = dev;
                    if (c == 3 ? dev != 0 : dev > 1) ok = false;
                }
                within += ok;
                if (!ok && first.empty()) {
                    char b[64];
                    snprintf(b, sizeof b, "; first off (%u,%u): %08x", i, j, v);
                    first = b;
                }
            }
        }
        report(e.empty() && within == counted, label + ": " + std::to_string(within) + " of " + std::to_string(counted) +
                                                     " within one unit, max deviation " + std::to_string(maxDev) + first + (e.empty() ? "" : " (" + e + ")"));
    };
    auto color = t ? program(kernels::tri_color_vs_bc, kernels::tri_color_vs_bc_size, kernels::tri_color_fs_bc, kernels::tri_color_fs_bc_size, "tri_color") : nullptr;
    if (t && color) colourPair(color->program, Bindings{}, "8e tri_color, the pair, per-vertex colours interpolated");
    auto attr = t ? program(kernels::tri_attr_vs_bc, kernels::tri_attr_vs_bc_size, kernels::tri_color_fs_bc, kernels::tri_color_fs_bc_size, "tri_attr") : nullptr;
    if (t && attr) {
        attr->program.vertexDescriptor.attributes[0] = VertexAttribute{VertexFormat::kFloat2, 0, 0};
        attr->program.vertexDescriptor.attributes[1] = VertexAttribute{VertexFormat::kUChar4Normalized, 8, 0};
        attr->program.vertexDescriptor.layouts[0] = VertexLayout{12, VertexStep::kPerVertex, 1};
        auto vb = newBuffer(gpu, 6 * 12, &why8);
        if (vb != nullptr) {
            const float pos[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
            const uint32_t rgba[3] = {0xff0000ffu, 0xff00ff00u, 0xffff0000u};
            auto* bytes = static_cast<uint8_t*>(vb->contents());
            for (int v = 0; v < 6; v++) {
                memcpy(bytes + 12 * v, pos[v], 8);
                memcpy(bytes + 12 * v + 8, &rgba[v % 3], 4);
            }
            Bindings vsb;
            vsb.buffers = {Binding{vb.get(), 0}};
            colourPair(attr->program, vsb, "8f tri_attr, the pair through a vertex descriptor (Float2, UChar4Normalized, stride 12)");
        }
    }
    // 8g. a texture: tri_tex_fs samples a 4x4 RGBA8 texture at tri_color's (red, green) through the FS prefetch
    // (A7XX: the ids in SP_FS_BINDLESS_PREFETCH_CMD); every pixel not within 1/64 of a texel edge its texel exactly
    auto tex = t ? program(kernels::tri_color_vs_bc, kernels::tri_color_vs_bc_size, kernels::tri_tex_fs_bc, kernels::tri_tex_fs_bc_size, "tri_tex") : nullptr;
    if (t && tex) {
        TextureDesc sd;
        sd.format = PixelFormat::kRGBA8Unorm;
        sd.width = 4;
        sd.height = 4;
        sd.usage = kTextureUsageRead;
        auto src = Texture::make(gpu, sd, &why8);
        Sampler smp;
        std::string swhy;
        if (src == nullptr || !makeSampler(SamplerDesc{}, &smp, &swhy)) {
            report(false, "8g tri_tex: a 4x4 texture and a sampler: " + why8 + swhy);
        } else {
            auto texel = [](uint32_t x, uint32_t y) { return 0xff000000u | (0x10u + 0x30u * x) | (0x20u + 0x30u * y) << 8 | uint32_t(0x80 + 8 * (x + 4 * y)) << 16; };
            for (uint32_t y = 0; y < 4; y++)
                for (uint32_t x = 0; x < 4; x++) {
                    uint32_t v = texel(x, y);
                    memcpy(static_cast<uint8_t*>(const_cast<void*>(static_cast<const void*>(src->contents()))) + y * src->bytesPerRow() + x * 4, &v, 4);
                }
            fill(0x00a5a5a5u);
            Bindings fsb;
            fsb.textures = {src.get()};
            fsb.samplers = {&smp};
            std::string e = run(tex->program, drawOf(0, 6), Bindings{}, fsb), first;
            uint32_t judged = 0, exact = 0;
            for (uint32_t j = 0; j < H; j++)
                for (uint32_t i = 0; i < W; i++) {
                    if (i == j) continue;
                    const double x = i + 0.5, y = j + 0.5;
                    double u, v;
                    if (j > i) {
                        u = (y - x) / 64.0;
                        v = x / 64.0;
                    } else {
                        u = y / 64.0;
                        v = (x - y) / 64.0;
                    }
                    const double fu = 4 * u, fv = 4 * v;
                    if (std::fabs(fu - std::round(fu)) < 1.0 / 64 || std::fabs(fv - std::round(fv)) < 1.0 / 64) continue;
                    const uint32_t tx = std::min(3u, uint32_t(std::max(0.0, std::floor(fu)))), ty = std::min(3u, uint32_t(std::max(0.0, std::floor(fv))));
                    judged++;
                    if (px(i, j) == texel(tx, ty)) {
                        exact++;
                    } else if (first.empty()) {
                        char b[96];
                        snprintf(b, sizeof b, "; first off (%u,%u): %08x, texel (%u,%u) %08x", i, j, px(i, j), tx, ty, texel(tx, ty));
                        first = b;
                    }
                }
            report(e.empty() && exact == judged && judged > 3000, "8g tri_tex, a 4x4 texture through the FS prefetch: " + std::to_string(exact) + " of " +
                                                                       std::to_string(judged) + " their texel exactly" + first + (e.empty() ? "" : " (" + e + ")"));
        }
    }
    // 8h. depth: tri_pos (clip position Float4 at 0, colour Float4 at 16, stride 32) into a Depth32Float attachment,
    // each draw its own pass loading and storing the depth. The "clear" is the pair at z 1 under Always + write; then
    // NEAR (T1, z 0.25, red) and FAR (the pair, z 0.75, green) under Less + write: j > i red, the rest green.
    auto pos = t ? program(kernels::tri_pos_vs_bc, kernels::tri_pos_vs_bc_size, kernels::tri_color_fs_bc, kernels::tri_color_fs_bc_size, "tri_pos") : nullptr;
    if (t && pos) {
        pos->program.vertexDescriptor.attributes[0] = VertexAttribute{VertexFormat::kFloat4, 0, 0};
        pos->program.vertexDescriptor.attributes[1] = VertexAttribute{VertexFormat::kFloat4, 16, 0};
        pos->program.vertexDescriptor.layouts[0] = VertexLayout{32, VertexStep::kPerVertex, 1};
        TextureDesc dd;
        dd.format = PixelFormat::kDepth32Float;
        dd.width = W;
        dd.height = H;
        dd.usage = kTextureUsageRenderTarget;
        auto depth = Texture::make(gpu, dd, &why8);
        auto mk = [&](uint32_t count, float z, const float c[4]) {
            auto b = newBuffer(gpu, 6 * 32, &why8);
            if (b == nullptr) return b;
            static const float xy[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
            auto* f = static_cast<float*>(b->contents());
            for (uint32_t v = 0; v < count; v++) {
                float vert[8] = {xy[v][0], xy[v][1], z, 1.0f, c[0], c[1], c[2], c[3]};
                memcpy(f + 8 * v, vert, 32);
            }
            return b;
        };
        const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1};
        auto clearBuf = mk(6, 1.0f, blue), nearBuf = mk(3, 0.25f, red), farBuf = mk(6, 0.75f, green);
        if (depth == nullptr || clearBuf == nullptr || nearBuf == nullptr || farBuf == nullptr) {
            report(false, "8h depth: a Depth32Float texture and three vertex buffers: " + why8);
        } else {
            fill(canary);
            std::string errs;
            struct Step {
                Buffer* b;
                uint32_t count, compare;
            };
            const Step steps[3] = {{clearBuf.get(), 6, 7}, {nearBuf.get(), 3, 1}, {farBuf.get(), 6, 1}};
            for (int k = 0; k < 3; k++) {
                RenderPass rp;
                rp.color0 = t.get();
                rp.depth = depth.get();
                rp.depthLoad = LoadAction::kLoad;
                rp.depthStore = StoreAction::kStore;
                Draw d = drawOf(0, steps[k].count);
                d.depth = DepthState{steps[k].compare, true};
                Bindings vsb;
                vsb.buffers = {Binding{steps[k].b, 0}};
                std::string e = run(pos->program, d, vsb, Bindings{}, &rp);
                if (!e.empty()) errs += " [draw " + std::to_string(k) + ": " + e + "]";
            }
            std::string first;
            auto r = judge([&](uint32_t i, uint32_t j) { return j > i ? 0xff0000ffu : 0xff00ff00u; }, &first);
            report(errs.empty() && r.first == 4096, "8h depth, Less + write, NEAR then FAR over a drawn clear: " + std::to_string(r.first) + " of 4096 as wanted" + first + errs);
        }
    }
    // 9. the 2D engine on A7XX (v25): Clears and a blit go through CP_BLIT, which ios-mondrian once saw refused by this
    // vendor SQE (their later finding: stale cache, not the packet). Last in the probe, each announced first, so that a
    // wedge still leaves the lines before it.
    if (t) {
        say("RM METAL: 9 (CP_BLIT) next\n");
        const uint32_t clearWord = 0xffbf8040u;   // (0.25, 0.5, 0.75, 1) as RGBA8Unorm bytes
        // 9a. a Clear alone
        fill(canary);
        {
            RenderPass rp;
            rp.color0 = t.get();
            rp.load = LoadAction::kClear;
            rp.clearColor[0] = 0.25;
            rp.clearColor[1] = 0.5;
            rp.clearColor[2] = 0.75;
            rp.clearColor[3] = 1.0;
            auto cb = queue->commandBuffer();
            std::string e;
            if (!cb->clear(rp, ro)) {
                e = "not recorded: " + cb->error();
            } else {
                cb->commit();
                cb->waitUntilCompleted();
                if (cb->status() != CommandBuffer::kCompleted) e = "did NOT complete: " + cb->error();
            }
            std::string first;
            auto r = judge([&](uint32_t, uint32_t) { return clearWord; }, &first);
            report(e.empty() && r.first == 4096, "9a Clear of 64x64 RGBA8Unorm through CP_BLIT: " + std::to_string(r.first) + " of 4096 the clear colour" + first + (e.empty() ? "" : " (" + e + ")"));
        }
        // 9b. a Clear and a draw in one pass: T1 the colour, the rest (the diagonal too) the clear colour
        if (flat) {
            fill(canary);
            RenderPass rp;
            rp.color0 = t.get();
            rp.load = LoadAction::kClear;
            rp.clearColor[0] = 0.25;
            rp.clearColor[1] = 0.5;
            rp.clearColor[2] = 0.75;
            rp.clearColor[3] = 1.0;
            std::string e = run(flat->program, drawOf(0, 3), Bindings{}, Bindings{}, &rp), first;
            auto r = judge([&](uint32_t i, uint32_t j) { return j > i ? flatColour : clearWord; }, &first);
            report(e.empty() && r.first == 4096, "9b Clear then tri_flat in one pass: " + std::to_string(r.first) + " of 4096 right" + first + (e.empty() ? "" : " (" + e + ")"));
        }
        // 9c. depth with a real Clear: NEAR (Less + write) into a depth cleared to 1, then FAR loading it
        if (pos) {
            TextureDesc dd;
            dd.format = PixelFormat::kDepth32Float;
            dd.width = W;
            dd.height = H;
            dd.usage = kTextureUsageRenderTarget;
            auto depth = Texture::make(gpu, dd, &why8);
            auto mk = [&](uint32_t count, float z, uint32_t rgba) {
                auto b = newBuffer(gpu, 6 * 32, &why8);
                if (b == nullptr) return b;
                static const float xy[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
                auto* f = static_cast<float*>(b->contents());
                for (uint32_t v = 0; v < count; v++) {
                    float vert[8] = {xy[v][0], xy[v][1], z, 1.0f, float(rgba & 0xff) / 255, float((rgba >> 8) & 0xff) / 255, float((rgba >> 16) & 0xff) / 255, 1.0f};
                    memcpy(f + 8 * v, vert, 32);
                }
                return b;
            };
            auto nearBuf = mk(3, 0.25f, 0xff0000ffu), farBuf = mk(6, 0.75f, 0xff00ff00u);
            if (depth && nearBuf && farBuf) {
                fill(canary);
                std::string errs;
                for (int k = 0; k < 2; k++) {
                    RenderPass rp;
                    rp.color0 = t.get();
                    rp.depth = depth.get();
                    rp.depthLoad = k == 0 ? LoadAction::kClear : LoadAction::kLoad;
                    rp.depthStore = StoreAction::kStore;
                    rp.clearDepth = 1.0;
                    Draw d = drawOf(0, k == 0 ? 3 : 6);
                    d.depth = DepthState{1, true};
                    Bindings vsb;
                    vsb.buffers = {Binding{k == 0 ? nearBuf.get() : farBuf.get(), 0}};
                    std::string e = run(pos->program, d, vsb, Bindings{}, &rp);
                    if (!e.empty()) errs += " [draw " + std::to_string(k) + ": " + e + "]";
                }
                std::string first;
                auto r = judge([&](uint32_t i, uint32_t j) { return j > i ? 0xff0000ffu : 0xff00ff00u; }, &first);
                report(errs.empty() && r.first == 4096, "9c depth cleared to 1 through CP_BLIT, NEAR then FAR: " + std::to_string(r.first) + " of 4096 as wanted" + first + errs);
            }
        }
        // 9d. a blit: a 64x64 RGBA8 texture of distinct words copied whole into the target by the 2D engine
        {
            TextureDesc sd;
            sd.format = PixelFormat::kRGBA8Unorm;
            sd.width = W;
            sd.height = H;
            sd.usage = kTextureUsageRead;
            auto src = Texture::make(gpu, sd, &why8);
            if (src != nullptr) {
                auto word = [](uint32_t i, uint32_t j) { return 0xff000000u | (i * 4) | (j * 4) << 8 | ((i ^ j) * 4) << 16; };
                for (uint32_t j = 0; j < H; j++)
                    for (uint32_t i = 0; i < W; i++) {
                        uint32_t v = word(i, j);
                        memcpy(static_cast<uint8_t*>(const_cast<void*>(static_cast<const void*>(src->contents()))) + j * src->bytesPerRow() + i * 4, &v, 4);
                    }
                fill(canary);
                auto cb = queue->commandBuffer();
                std::string e;
                if (!cb->blit(*src, 0, render::BlitRect{0, 0, W, H}, *t, 0, render::BlitRect{0, 0, W, H}, false, ro)) {
                    e = "not recorded: " + cb->error();
                } else {
                    cb->commit();
                    cb->waitUntilCompleted();
                    if (cb->status() != CommandBuffer::kCompleted) e = "did NOT complete: " + cb->error();
                }
                std::string first;
                auto r = judge(word, &first);
                report(e.empty() && r.first == 4096, "9d blit of a 64x64 RGBA8 texture through the 2D engine: " + std::to_string(r.first) + " of 4096 right" + first + (e.empty() ? "" : " (" + e + ")"));
            }
        }
    }
    // 10. GMEM on A7XX (v26): passes drawn tile by tile in the 2 MiB GMEM, with the tile loads, clears and stores as BLIT
    // events (render::Options::gmem on; the queue picks GMEM for any pass that can have it). Each line says whether the
    // pass really went to GMEM (counts().gmemPasses). Last, announced first.
    if (t && flat) {
        say("RM METAL: 10 (GMEM) next\n");
        render::Options go;
        go.gmem = true;
        const uint32_t clearWord = 0xffbf8040u;
        auto passRun = [&](Texture& target, bool clearIt, const std::vector<Draw>& draws, uint32_t* gmemPasses) {
            RenderPass rp;
            rp.color0 = &target;
            rp.load = clearIt ? LoadAction::kClear : LoadAction::kLoad;
            rp.store = StoreAction::kStore;
            rp.clearColor[0] = 0.25;
            rp.clearColor[1] = 0.5;
            rp.clearColor[2] = 0.75;
            rp.clearColor[3] = 1.0;
            auto cb = queue->commandBuffer();
            for (size_t k = 0; k < draws.size(); k++) {
                if (k == 1) rp.load = LoadAction::kLoad;
                if (!cb->draw(flat->program, rp, draws[k], Bindings{}, Bindings{}, go)) return std::string("not recorded: ") + cb->error();
            }
            cb->commit();
            cb->waitUntilCompleted();
            *gmemPasses = cb->counts().gmemPasses;
            if (cb->status() != CommandBuffer::kCompleted) return std::string("did NOT complete: ") + cb->error();
            return std::string();
        };
        auto gm = [](uint32_t n) { return n ? std::string(" (GMEM)") : std::string(" (went to SYSMEM)"); };
        // 10a. the pair over a loaded target: every tile loaded, drawn, stored
        {
            fill(canary);
            uint32_t g = 0;
            std::string e = passRun(*t, false, {drawOf(0, 6)}, &g), first;
            auto r = judge([&](uint32_t, uint32_t) { return flatColour; }, &first);
            report(e.empty() && r.first == 4096 && g == 1, "10a GMEM, tri_flat's pair over a loaded 64x64: " + std::to_string(r.first) + " of 4096" + gm(g) + first + (e.empty() ? "" : " (" + e + ")"));
        }
        // 10b. a Clear in the tiles and T1
        {
            fill(canary);
            uint32_t g = 0;
            std::string e = passRun(*t, true, {drawOf(0, 3)}, &g), first;
            auto r = judge([&](uint32_t i, uint32_t j) { return j > i ? flatColour : clearWord; }, &first);
            report(e.empty() && r.first == 4096 && g == 1, "10b GMEM, a Clear and T1: " + std::to_string(r.first) + " of 4096" + gm(g) + first + (e.empty() ? "" : " (" + e + ")"));
        }
        // 10c. a Clear, T1 and T2 as two draws of one pass
        {
            fill(canary);
            uint32_t g = 0;
            std::string e = passRun(*t, true, {drawOf(0, 3), drawOf(3, 3)}, &g), first;
            auto r = judge([&](uint32_t i, uint32_t j) { (void)i; (void)j; return flatColour; }, &first);   // the pair covers the diagonal too (8a)
            report(e.empty() && r.first == 4096 && g == 1, "10c GMEM, a Clear, T1 and T2 in one pass: " + std::to_string(r.first) + " of 4096" + gm(g) + first + (e.empty() ? "" : " (" + e + ")"));
        }
        // 10d. many tiles: a 512x512 target (1 MiB, twice hakios's 512 KiB GMEM plan), a Clear and T1; inside iff j > i
        {
            TextureDesc bd;
            bd.format = PixelFormat::kRGBA8Unorm;
            bd.width = 512;
            bd.height = 512;
            bd.usage = kTextureUsageRenderTarget | kTextureUsageRead;
            auto big = Texture::make(gpu, bd, &why8);
            if (big == nullptr) {
                report(false, "10d: a 512x512 target: " + why8);
            } else {
                auto* w = static_cast<uint32_t*>(big->contents());
                for (uint64_t i = 0; i < big->buffer()->length() / 4; i++) w[i] = canary;
                uint32_t g = 0;
                Draw d = drawOf(0, 3, Viewport{0, 0, 512, 512, 0, 1});
                std::string e = passRun(*big, true, {d}, &g);
                const uint64_t pitch = big->bytesPerRow() / 4;
                uint32_t right = 0;
                std::string first;
                for (uint32_t j = 0; j < 512; j++)
                    for (uint32_t i = 0; i < 512; i++) {
                        const uint32_t want = j > i ? flatColour : clearWord, got = w[j * pitch + i];
                        if (got == want) {
                            right++;
                        } else if (first.empty()) {
                            char b[96];
                            snprintf(b, sizeof b, "; first off (%u,%u): %08x, wanted %08x", i, j, got, want);
                            first = b;
                        }
                    }
                report(e.empty() && right == 512 * 512 && g == 1, "10d GMEM, a Clear and T1 in 512x512 (several tiles): " + std::to_string(right) + " of 262144" + gm(g) + first +
                                                                      (e.empty() ? "" : " (" + e + ")"));
            }
        }
    }
    // Stage 11 (v53): what CoreAnimation's first lock-screen frame adds over the boot fade, which hung the GPU
    // (METAL-PORT.md, v52): a DMA-indexed draw (the 7-word CP_DRAW_INDX_OFFSET, 16-bit indices) and a draw whose
    // fragment shader reads its own target (framebuffer fetch, with the sysmem barrier before it). Indexed first: a
    // hang leaves the GPU wedged, and the line says which one did it.
    if (t && flat) {
        // 11a. tri_flat's pair through an index buffer 0..5: as 8a, every pixel the colour
        auto ib = newBuffer(gpu, 16, &why8);
        if (ib == nullptr) {
            report(false, "11a: an index buffer: " + why8);
        } else {
            auto* ix = static_cast<uint16_t*>(ib->contents());
            for (uint16_t k = 0; k < 8; k++) ix[k] = k < 6 ? k : 0;
            Draw d = drawOf(0, 6);
            d.indexBuffer = ib.get();
            d.indexType = 0;
            fill(canary);
            std::string e = run(flat->program, d, Bindings{}, Bindings{}), first;
            auto r = judge([&](uint32_t, uint32_t) { return flatColour; }, &first);
            report(e.empty() && r.first == r.second, "11a tri_flat indexed (16-bit, 6 indices): " + std::to_string(r.first) + " of 4096 the colour" + first + (e.empty() ? "" : " (" + e + ")"));
        }
    }
    // Stage 13 (v61): a device buffer in a draw. CoreAnimation's compositor VS reads its buffers through the 3D IBO table
    // (resinfo on IBO 0 and 1 in its preamble, v57), and no probe draw had done that. 13a and 13c through the new A7XX
    // bindless path (SP_BINDLESS_BASE[0] at the table), 13b through the old A6XX one (SP_IBO) -- last, since if it is
    // what hung CoreAnimation it wedges the GPU.
    {
        auto pairProgram = [&](const uint8_t* vsBc, size_t vsSize, const uint8_t* fsBc, size_t fsSize, const char* label) {
            return program(vsBc, vsSize, fsBc, fsSize, label);
        };
        const float xy[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
        auto pos = newBuffer(gpu, sizeof xy, &why8);
        auto col = newBuffer(gpu, 16, &why8);
        if (t && pos && col) {
            memcpy(pos->contents(), xy, sizeof xy);
            const float c4[4] = {0x33 / 255.0f, 0x66 / 255.0f, 0x99 / 255.0f, 1.0f};
            memcpy(col->contents(), c4, sizeof c4);
            const uint32_t colWord = 0xff996633u;
            Bindings vsb;
            vsb.buffers = {Binding{pos.get(), 0}};
            Bindings fsb;
            fsb.buffers = {Binding{col.get(), 0}};
            auto both = [&](const char* name, bool bindless, bool fsBuffer) {
                setGfxBindlessIbo(bindless);
                auto p = fsBuffer ? pairProgram(kernels::tri_flat_vs_bc, kernels::tri_flat_vs_bc_size, kernels::tri_dbuf_fs_bc, kernels::tri_dbuf_fs_bc_size, name)
                                  : pairProgram(kernels::tri_dpos_vs_bc, kernels::tri_dpos_vs_bc_size, kernels::tri_flat_fs_bc, kernels::tri_flat_fs_bc_size, name);
                if (p == nullptr) return;
                fill(canary);
                const uint32_t want = fsBuffer ? colWord : flatColour;
                std::string e = run(p->program, drawOf(0, fsBuffer ? 6 : 6), fsBuffer ? Bindings{} : vsb, fsBuffer ? fsb : Bindings{}), first;
                auto r = judge([&](uint32_t, uint32_t) { return want; }, &first);
                report(e.empty() && r.first == r.second, std::string(name) + ": " + std::to_string(r.first) + " of 4096 right" + first + (e.empty() ? "" : " (" + e + ")"));
            };
            both("13a tri_dpos, VS positions from a device buffer, bindless IBO", true, false);
            both("13c tri_dbuf, FS colour from a device buffer, bindless IBO", true, true);
            both("13b tri_dpos, VS positions from a device buffer, A6XX SP_IBO", false, false);
            setGfxBindlessIbo(true);
        } else {
            report(false, "13: buffers: " + why8);
        }
    }
    // Stage 12 (v60): the render-target formats backboardd's compositor draws into (v59: 80, 81, 10, 30, 115, 43 beside
    // the probe's 70). tri_flat's pair into a 64x64 target of each, the program linked for it: did it complete, and the
    // first texel's bits (the colour 20 40 80 ff in that format). Commonest first: a hang wedges the GPU.
    {
        struct Fmt { PixelFormat f; const char* name; uint32_t bytes; };
        const Fmt fmts[] = {
            {PixelFormat::kBGRA8Unorm, "BGRA8Unorm (80)", 4}, {PixelFormat::kBGRA8UnormSrgb, "BGRA8Unorm_sRGB (81)", 4},
            {PixelFormat::kR8Unorm, "R8Unorm (10)", 1},       {PixelFormat::kRG8Unorm, "RG8Unorm (30)", 2},
            {PixelFormat::kRGBA16Float, "RGBA16Float (115)", 8}, {PixelFormat::kBGR5A1Unorm, "BGR5A1Unorm (43)", 2},
        };
        for (const Fmt& f : fmts) {
            std::string w1, w2, w3;
            auto v = compileStage(kernels::tri_flat_vs_bc, kernels::tri_flat_vs_bc_size, &w1);
            auto fr = compileStage(kernels::tri_flat_fs_bc, kernels::tri_flat_fs_bc_size, &w2);
            if (v == nullptr || fr == nullptr) { report(false, std::string("12 ") + f.name + ": compile: " + (v ? w2 : w1)); continue; }
            auto p = linkProgram(std::shared_ptr<CompiledStage>(std::move(v)), std::shared_ptr<CompiledStage>(std::move(fr)), f.f, &w3);
            if (p == nullptr) { report(false, std::string("12 ") + f.name + ": link: " + w3); continue; }
            TextureDesc fd;
            fd.format = f.f;
            fd.width = W;
            fd.height = H;
            fd.usage = kTextureUsageRenderTarget | kTextureUsageRead;
            std::string wt;
            auto tf = Texture::make(gpu, fd, &wt);
            if (tf == nullptr) { report(false, std::string("12 ") + f.name + ": target: " + wt); continue; }
            memset(tf->contents(), 0xa5, tf->buffer()->length());
            RenderPass rp;
            rp.color0 = tf.get();
            auto cb = queue->commandBuffer();
            std::string e;
            if (!cb->draw(p->program, rp, drawOf(0, 6), Bindings{}, Bindings{}, ro)) {
                e = std::string("not recorded: ") + cb->error();
            } else {
                cb->commit();
                cb->waitUntilCompleted();
                if (cb->status() != CommandBuffer::kCompleted) e = std::string("did NOT complete: ") + cb->error();
            }
            char b[96];
            const uint8_t* c = static_cast<const uint8_t*>(tf->contents()) + 10 * tf->bytesPerRow() + 5 * f.bytes;
            int n = snprintf(b, sizeof b, "; texel (5,10):");
            for (uint32_t k = 0; k < f.bytes && n < (int)sizeof b - 4; k++) n += snprintf(b + n, sizeof b - n, " %02x", c[k]);
            report(e.empty(), std::string("12 tri_flat into ") + f.name + ": " + (e.empty() ? "completed" : e) + b);
        }
    }
    // 11d/11e (v58): instancing. CoreAnimation's hung frame (v57) stops at an indexed TRISTRIP of 4 indices and 13
    // INSTANCES; every fade draw and every test before had one instance. tri_flat ignores the instance id, so 13
    // instances paint the same pair: as 8a. Non-indexed first, then indexed as CoreAnimation draws it.
    if (t && flat) {
        Draw d = drawOf(0, 6);
        d.instanceCount = 13;
        fill(canary);
        std::string e = run(flat->program, d, Bindings{}, Bindings{}), first;
        auto r = judge([&](uint32_t, uint32_t) { return flatColour; }, &first);
        report(e.empty() && r.first == r.second, "11d tri_flat, 13 instances: " + std::to_string(r.first) + " of 4096 the colour" + first + (e.empty() ? "" : " (" + e + ")"));
        auto ib4 = newBuffer(gpu, 16, &why8);
        if (ib4 != nullptr) {
            auto* ix = static_cast<uint16_t*>(ib4->contents());
            for (uint16_t k = 0; k < 8; k++) ix[k] = k < 6 ? k : 0;
            Draw di = drawOf(0, 6);
            di.indexBuffer = ib4.get();
            di.indexType = 0;
            di.instanceCount = 13;
            fill(canary);
            e = run(flat->program, di, Bindings{}, Bindings{});
            first.clear();
            r = judge([&](uint32_t, uint32_t) { return flatColour; }, &first);
            report(e.empty() && r.first == r.second, "11e tri_flat indexed, 13 instances: " + std::to_string(r.first) + " of 4096 the colour" + first + (e.empty() ? "" : " (" + e + ")"));
            // ... and as a 4-index strip (0 1 2 3: the first two of the pair's corners and their neighbours), 13 instances:
            // judged only for completing
            Draw ds = drawOf(0, 4);
            ds.primitive = Primitive::kTriangleStrip;
            ds.indexBuffer = ib4.get();
            ds.indexType = 0;
            ds.instanceCount = 13;
            e = run(flat->program, ds, Bindings{}, Bindings{});
            report(e.empty(), "11f tri_flat indexed TRISTRIP of 4, 13 instances: " + (e.empty() ? std::string("completed") : e));
        }
    }
    auto fbp = t ? program(kernels::tri_flat_vs_bc, kernels::tri_flat_vs_bc_size, kernels::tri_fb_fs_bc, kernels::tri_fb_fs_bc_size, "tri_fb") : nullptr;
    if (t && fbp) {
        // 11b. tri_fb_fs over a target filled 40 80 bf ff: one pair of draws leaves 80 bf 40 ff (g, b, r, 1)
        const uint32_t before = 0xffbf8040u, after = 0xff40bf80u;
        fill(before);
        std::string e = run(fbp->program, drawOf(0, 6), Bindings{}, Bindings{}), first;
        auto r = judge([&](uint32_t, uint32_t) { return after; }, &first);
        report(e.empty() && r.first == r.second, "11b tri_fb (framebuffer fetch, sysmem barrier): " + std::to_string(r.first) + " of 4096 re-coloured" + first + (e.empty() ? "" : " (" + e + ")"));
        // 11c (v55). as CoreAnimation's hung frame has it: in ONE sysmem pass, an indexed tri_flat T1 (j > i), then the
        // fetching pair -- its barrier now in the middle of a pass, after a draw. Over 40 80 bf ff: below the diagonal
        // the flat colour re-coloured (20 40 80 -> 40 80 20), above it the fill re-coloured
        auto ib = newBuffer(gpu, 16, &why8);
        if (ib != nullptr && flat) {
            auto* ix = static_cast<uint16_t*>(ib->contents());
            for (uint16_t k = 0; k < 8; k++) ix[k] = k < 3 ? k : 0;
            Draw d1 = drawOf(0, 3);
            d1.indexBuffer = ib.get();
            d1.indexType = 0;
            fill(before);
            RenderPass rp;
            rp.color0 = t.get();
            auto cb = queue->commandBuffer();
            std::string e2;
            if (!cb->draw(flat->program, rp, d1, Bindings{}, Bindings{}, ro) ||
                !cb->draw(fbp->program, rp, drawOf(0, 6), Bindings{}, Bindings{}, ro)) {
                e2 = std::string("not recorded: ") + cb->error();
            } else {
                cb->commit();
                cb->waitUntilCompleted();
                if (cb->status() != CommandBuffer::kCompleted) e2 = std::string("did NOT complete: ") + cb->error();
            }
            std::string first2;
            auto r2 = judge([&](uint32_t i, uint32_t j) { return j > i ? 0xff208040u : i > j ? after : 0u; }, &first2);
            report(e2.empty() && r2.first == r2.second, "11c indexed tri_flat then tri_fb in one sysmem pass: " + std::to_string(r2.first) + " of " +
                                                            std::to_string(r2.second) + " right" + first2 + (e2.empty() ? "" : " (" + e2 + ")"));
        }
    }
}
