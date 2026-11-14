#include "hakios-metal/command.hpp"
#include "hakios-metal/texture.hpp"
#include "hakios-metal/a7xx.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <map>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

namespace hakios::metal {

namespace {
constexpr size_t kCommandBo = 64 * 1024;           // one command buffer's packets, at most (grown when needed)
constexpr int64_t kPatienceNs = 5000000000;        // a fence not passed in 5 s: the GPU is hung, the buffer is an error
constexpr uint32_t kFillHead = 64;                 // words written by CP_MEM_WRITE before the copies double them
// The most one CP_MEMCPY moves. On the phone (27 October) 125 000 words in one packet took 4.2 ms, and 4 Mi words (16 MB)
// in one packet hung the GPU ("no answer" from the kernel's driver, and the system after it). Cut into packets of this
// size, each waited for before the next, the same copy is a row of small ones.
constexpr uint32_t kMemcpyMax = 64 * 1024;
}  // namespace

void appendStream(pm4::Stream& to, const pm4::Stream& from);   // render.cpp's: a pass of its own onto the buffer's stream

namespace {
std::mutex gAddressLock;
std::map<uint64_t, std::weak_ptr<Buffer>> gByAddress;   // by gpuAddress, every Buffer newBuffer made and still alive

std::shared_ptr<Buffer> addressed(Buffer* raw) {
    const uint64_t at = raw->gpuAddress();
    std::shared_ptr<Buffer> b(raw, [at](Buffer* x) {
        {
            std::lock_guard<std::mutex> g(gAddressLock);
            auto it = gByAddress.find(at);
            if (it != gByAddress.end() && it->second.expired()) gByAddress.erase(it);
        }
        delete x;
    });
    std::lock_guard<std::mutex> g(gAddressLock);
    gByAddress[at] = b;
    return b;
}
}  // namespace

std::shared_ptr<Buffer> bufferAt(uint64_t address, uint64_t* offset) {
    std::lock_guard<std::mutex> g(gAddressLock);
    auto it = gByAddress.upper_bound(address);
    if (it == gByAddress.begin()) return nullptr;
    --it;
    std::shared_ptr<Buffer> b = it->second.lock();
    if (b == nullptr || address - it->first >= b->length()) return nullptr;
    *offset = address - it->first;
    return b;
}

std::atomic<uint32_t> gBuffersMade{0}, gBuffersReused{0};
std::atomic<uint64_t> gBytesMade{0};

std::shared_ptr<Buffer> newBuffer(Gpu& gpu, uint64_t length, std::string* why) {
    gBuffersMade++;
    gBytesMade += length;
    int error = 0;
    std::unique_ptr<Bo> bo = gpu.newBo(length, kBoWriteCombined, &error);
    if (bo == nullptr) {
        *why = "the driver gave no buffer of " + std::to_string(length) + " bytes: " + strerror(error);
        return nullptr;
    }
    return addressed(new Buffer(std::move(bo), length));
}

namespace {
// One slab is 32 draw tables. Pieces are cut at 256 bytes, which is more than anything in them wants (128 for a
// program's preload and the border colour, 64 for a texture descriptor), and a piece of a page or more starts on a
// page, so that a table lies in memory as it did in a buffer of its own.
constexpr uint64_t kSlabBytes = 256 * 1024;
constexpr uint64_t kPieceAlign = 256;
constexpr uint64_t kPageAlign = 4096;
}  // namespace

bool CommandBuffer::piece(uint64_t bytes, Piece* out, std::string* why) {
    bytes = (std::max<uint64_t>(bytes, 1) + kPieceAlign - 1) & ~(kPieceAlign - 1);
    const uint64_t align = bytes >= kPageAlign ? kPageAlign : kPieceAlign;
    if (slab_ != nullptr) {
        slabUsed_ = std::min(kSlabBytes, (slabUsed_ + align - 1) & ~(align - 1));
    }
    if (bytes > kSlabBytes / 4) {
        // (a piece this large in a slab would leave most of the slab unused when the next one did not fit)
        out->buffer = queue_.takeBuffer(bytes, why);
        out->offset = 0;
        if (out->buffer != nullptr) slabs_.push_back(out->buffer);
    } else {
        if (slab_ == nullptr || kSlabBytes - slabUsed_ < bytes) {
            slab_ = queue_.takeSlab(kSlabBytes, why);
            slabUsed_ = 0;
            if (slab_ != nullptr) slabs_.push_back(slab_);
        }
        out->buffer = slab_;
        out->offset = slabUsed_;
        if (slab_ != nullptr) {
            slabUsed_ += bytes;
        }
    }
    if (out->buffer == nullptr) {
        return false;
    }
    if (out->offset == 0) {
        use(*out->buffer, true);
        keep(out->buffer);
    }
    return true;
}

uint64_t CommandBuffer::programAddress(const Kernel& k, std::string* why) {
    const uint32_t n = std::max<uint32_t>(k.numWords, 1);
    auto same = [&](const Uploaded& u) {
        // (every word: a program freed and another of the same length made at its address within one command buffer
        // must not be taken for it -- the upstream rule, "only when the words are still the same")
        return u.words.size() == k.numWords && (k.numWords == 0 || std::memcmp(u.words.data(), k.words, 4ull * k.numWords) == 0);
    };
    const auto [first, last] = uploaded_.equal_range(k.words);
    for (auto it = first; it != last; ++it) {
        if (same(it->second)) return it->second.gpuAddress;
    }
    (void)n;
    std::shared_ptr<Buffer> b = queue_.programBuffer(k, why);
    if (b == nullptr) return 0;
    use(*b, false);
    keep(b);
    uploaded_.emplace(k.words, Uploaded{std::vector<uint32_t>(k.words, k.words + k.numWords), b->gpuAddress()});
    return b->gpuAddress();
}

namespace {
uint64_t sizeClass(uint64_t bytes) {
    uint64_t c = 4096;
    while (c < bytes) c <<= 1;
    return c;
}
}  // namespace

std::shared_ptr<Buffer> CommandQueue::takeBuffer(uint64_t bytes, std::string* why) {
    const uint64_t size = sizeClass(bytes);
    std::shared_ptr<Buffer> b;
    {
        std::lock_guard<std::mutex> g(poolLock_);
        auto it = freeBuffers_.find(size);
        if (it != freeBuffers_.end() && !it->second.empty()) {
            b = std::move(it->second.back());
            it->second.pop_back();
            freeBytes_ -= size;
        }
    }
    if (b == nullptr) return newBuffer(gpu_, size, why);
    gBuffersReused++;
    memset(b->contents(), 0, size);   // (as a new one comes from the kernel)
    return b;
}

void CommandQueue::giveBuffer(std::shared_ptr<Buffer> b) {
    // (only one no one else holds, of a size takeBuffer makes: a piece's buffer kept elsewhere is still someone's)
    if (b == nullptr || b.use_count() != 1 || b->contents() == nullptr || b->length() != sizeClass(b->length())) return;
    std::lock_guard<std::mutex> g(poolLock_);
    if (freeBytes_ + b->length() > (32ull << 20)) return;
    freeBytes_ += b->length();
    freeBuffers_[b->length()].push_back(std::move(b));
}

std::shared_ptr<Buffer> CommandQueue::takeSlab(uint64_t bytes, std::string* why) { return takeBuffer(bytes, why); }

void CommandQueue::giveSlabs(std::vector<std::shared_ptr<Buffer>>* slabs) {
    for (auto& b : *slabs) giveBuffer(std::move(b));
    slabs->clear();
}

std::shared_ptr<Buffer> CommandQueue::programBuffer(const Kernel& k, std::string* why) {
    std::lock_guard<std::mutex> g(lock_);
    const auto [first, last] = resident_.equal_range(k.words);
    for (auto it = first; it != last;) {
        const Resident& r = it->second;
        if (r.words.size() == k.numWords && std::memcmp(r.words.data(), k.words, 4ull * k.numWords) == 0) return r.buffer;
        residentBytes_ -= 4ull * r.words.size();
        it = resident_.erase(it);   // (the pointer's words have changed: a freed program's)
    }
    // (past 64 MB of programs, the ones kept are let go: a command buffer in flight keeps its own)
    if (residentBytes_ > (64ull << 20)) {
        resident_.clear();
        residentBytes_ = 0;
    }
    std::shared_ptr<Buffer> b = newBuffer(gpu_, 4ull * std::max<uint32_t>(k.numWords, 1), why);
    if (b == nullptr || b->contents() == nullptr) return nullptr;
    memcpy(b->contents(), k.words, 4ull * k.numWords);
    resident_.emplace(k.words, Resident{std::vector<uint32_t>(k.words, k.words + k.numWords), b});
    residentBytes_ += 4ull * k.numWords;
    return b;
}

namespace {
// the live surface buffers, by (GPU, kernel handle): the kernel gives this client one handle per surface, so two
// Buffers over it would close it twice. The deleter takes the lock too, so that a handle the kernel hands out again
// cannot be met by a Buffer still being destroyed.
std::mutex gSurfaceLock;
std::map<std::pair<const Gpu*, uint32_t>, std::weak_ptr<Buffer>> gSurfaceBuffers;
}  // namespace

std::shared_ptr<Buffer> surfaceBuffer(Gpu& gpu, uint32_t surfaceId, SurfaceInfo* info, std::string* why) {
    std::lock_guard<std::mutex> g(gSurfaceLock);
    int error = 0;
    std::unique_ptr<Bo> bo = gpu.surfaceBo(surfaceId, info, &error);
    if (bo == nullptr) {
        *why = "IOSurface " + std::to_string(surfaceId) + ": the driver gave no buffer for it: " +
               (error == ENOENT ? std::string("no such surface") : error == ENOTSUP ? std::string("a surface around a program's own memory is not one the GPU takes") : strerror(error));
        return nullptr;
    }
    const auto key = std::make_pair(static_cast<const Gpu*>(&gpu), bo->handle());
    auto live = gSurfaceBuffers.find(key);
    if (live != gSurfaceBuffers.end()) {
        if (std::shared_ptr<Buffer> b = live->second.lock()) {
            gpu.disown(std::move(bo));   // the same handle as the live Buffer's Bo: closed once, by that Bo
            return b;
        }
    }
    const uint64_t length = bo->size();
    std::shared_ptr<Buffer> b(new Buffer(std::move(bo), length), [key](Buffer* x) {
        std::lock_guard<std::mutex> g(gSurfaceLock);
        auto it = gSurfaceBuffers.find(key);
        if (it != gSurfaceBuffers.end() && it->second.expired()) gSurfaceBuffers.erase(it);
        delete x;
    });
    gSurfaceBuffers[key] = b;
    return b;
}

// --- CommandBuffer ----------------------------------------------------------------------------------------------

bool CommandBuffer::fail(const std::string& why) {
    if (error_.empty()) {
        error_ = why;
    }
    return false;
}

void CommandBuffer::stamp(uint32_t slot) {
    if (profileStamps_ == nullptr) {
        std::string why;
        profileStamps_ = queue_.takeBuffer(16 * kMaxProfiled, &why);
        if (profileStamps_ == nullptr) {
            profile_ = tuning_ = false;
            return;
        }
        memset(profileStamps_->contents(), 0, 16 * kMaxProfiled);
        keep(profileStamps_);
    }
    const uint64_t at = profileStamps_->gpuAddress() + 8ull * slot;
    const uint32_t w[3] = {a6xx::CP_REG_TO_MEM_0_REG(0x0980) | a6xx::CP_REG_TO_MEM_0_CNT(2) | a6xx::CP_REG_TO_MEM_0_64B, uint32_t(at),
                           uint32_t(at >> 32)};
    stream_.packet(a6xx::CP_REG_TO_MEM, w, 3);
    use(*profileStamps_, true);
}

std::shared_ptr<Buffer> CommandQueue::vscBuffer(std::string* why) {
    std::lock_guard<std::mutex> g(lock_);
    if (vsc_ == nullptr) vsc_ = newBuffer(gpu_, render::vscBytes(), why);
    return vsc_;
}

void CommandQueue::checkVsc() {
    std::lock_guard<std::mutex> g(lock_);
    if (vsc_ == nullptr || vsc_->contents() == nullptr) return;
    const uint32_t word = *reinterpret_cast<const volatile uint32_t*>(static_cast<const uint8_t*>(vsc_->contents()) + 32ull * render::kVscDrawPitch + render::kVscOverflowAt);
    if (word != 0) vscOverflowed_ = true;
}

bool CommandQueue::preferGmem(uint64_t key) {
    std::lock_guard<std::mutex> g(lock_);
    Tune& t = tune_[key];
    t.passes++;
    // (sysmem first: a kind of pass seen once may not come again, and an untried GMEM pass is the dearer gamble -- 5 ms
    // against 0.13 for a light one)
    if (t.runs[0] == 0) return false;
    if (t.runs[1] == 0) return true;
    const bool better = t.us[1] < t.us[0];
    // (the other mode again now and then, what a pass costs drifting: every 16th pass, or every 64th when the one is
    // dearer by half -- a light pass of CoreAnimation took 5 ms in GMEM against 0.13 in sysmem)
    const double ratio = better ? t.us[0] / t.us[1] : t.us[1] / t.us[0];
    return t.passes % (ratio > 1.5 ? 64 : 16) == 0 ? !better : better;
}

namespace {
uint64_t kindKey(uint64_t kind, uint32_t ordinal) { return kind * 0x100000001b3ull ^ ordinal; }
}  // namespace

uint32_t CommandQueue::lastDraws(uint64_t kind, uint32_t ordinal) {
    std::lock_guard<std::mutex> g(lock_);
    auto it = lastDraws_.find(kindKey(kind, ordinal));
    return it == lastDraws_.end() ? 0 : it->second;
}

void CommandQueue::noteDraws(uint64_t kind, uint32_t ordinal, uint32_t draws) {
    std::lock_guard<std::mutex> g(lock_);
    lastDraws_[kindKey(kind, ordinal)] = draws;
    Peak& p = peakDraws_[kindKey(kind, ordinal)];
    if (draws >= p.draws || ++p.below >= 16) p.draws = draws, p.below = 0;
}

uint32_t CommandQueue::peakDraws(uint64_t kind, uint32_t ordinal) {
    std::lock_guard<std::mutex> g(lock_);
    auto it = peakDraws_.find(kindKey(kind, ordinal));
    return it == peakDraws_.end() ? 0 : it->second.draws;
}

void CommandQueue::tuned(uint64_t key, bool gmem, double us) {
    if (!(us > 0)) return;
    std::lock_guard<std::mutex> g(lock_);
    Tune& t = tune_[key];
    double& avg = t.us[gmem ? 1 : 0];
    avg = t.runs[gmem ? 1 : 0]++ == 0 ? us : 0.75 * avg + 0.25 * us;
}

void CommandBuffer::stampStart(PassKind kind, uint32_t width, uint32_t height) {
    if (!(profile_ || tuning_) || profiled_.size() >= kMaxProfiled) return;
    { PassTime t{}; t.kind = kind, t.width = width, t.height = height; profiled_.push_back(std::move(t)); }
    stamp(uint32_t(2 * (profiled_.size() - 1)));
}

void CommandBuffer::stampEnd() {
    if (!(profile_ || tuning_) || profiled_.empty()) return;
    stamp(uint32_t(2 * (profiled_.size() - 1) + 1));
}

void CommandBuffer::drawStamp(uint32_t slot) {
    if (drawStamps_ == nullptr) {
        std::string why;
        drawStamps_ = queue_.takeBuffer(16 * kMaxProfiledDraws, &why);
        if (drawStamps_ == nullptr) {
            profileDraws_ = false;
            return;
        }
        memset(drawStamps_->contents(), 0, 16 * kMaxProfiledDraws);
        keep(drawStamps_);
    }
    const uint64_t at = drawStamps_->gpuAddress() + 8ull * slot;
    stream_.waitForIdle();
    const uint32_t w[3] = {a6xx::CP_REG_TO_MEM_0_REG(0x0980) | a6xx::CP_REG_TO_MEM_0_CNT(2) | a6xx::CP_REG_TO_MEM_0_64B, uint32_t(at),
                           uint32_t(at >> 32)};
    stream_.packet(a6xx::CP_REG_TO_MEM, w, 3);
    use(*drawStamps_, true);
}

std::vector<CommandBuffer::DrawTime> CommandBuffer::drawTimes() const {
    std::vector<DrawTime> out = drawsProfiled_;
    if (drawStamps_ == nullptr) return {};
    const auto* t = static_cast<const uint64_t*>(drawStamps_->contents());
    for (size_t i = 0; i < out.size(); i++) {
        out[i].us = t[2 * i + 1] > t[2 * i] ? double(t[2 * i + 1] - t[2 * i]) / 19.2 : 0.0;
    }
    return out;
}

std::vector<CommandBuffer::PassTime> CommandBuffer::passTimes() const {
    std::vector<PassTime> out = profiled_;
    if (profileStamps_ == nullptr) return {};
    const auto* t = static_cast<const uint64_t*>(profileStamps_->contents());
    for (size_t i = 0; i < out.size(); i++) {
        out[i].us = t[2 * i + 1] > t[2 * i] ? double(t[2 * i + 1] - t[2 * i]) / 19.2 : 0.0;
    }
    return out;
}

bool CommandBuffer::refuseWork(const std::string& why) {
    if (!skipRefused_) {
        return fail(why);
    }
    skipped_++;
    lastSkipped_ = why;
    return false;
}

void CommandBuffer::addCompletedHandler(std::function<void(CommandBuffer&)> handler) {
    if (status_ <= kEnqueued) {
        handlers_.push_back(std::move(handler));
    }
}

void CommandBuffer::enqueue() {
    Status expected = kNotEnqueued;
    status_.compare_exchange_strong(expected, kEnqueued);
}

void CommandBuffer::commit() {
    if (status_ > kEnqueued) {
        return;  // Metal asserts here; committing twice does nothing at all in this driver
    }
    queue_.submit(shared_from_this());
}

void CommandBuffer::waitUntilCompleted() {
    std::unique_lock<std::mutex> g(lock_);
    // (a buffer never committed is not waited for: Metal would wait forever, which helps nobody)
    done_.wait(g, [this] { return finished_ || status_ <= kEnqueued; });
}

CommandBuffer::~CommandBuffer() {
    // (a buffer never completed may be destroyed with the GPU still on it -- an error -- and its stamps are then not
    // given back; one that completed has had its readers)
    if (finished_) {
        queue_.giveBuffer(std::move(profileStamps_));
        queue_.giveBuffer(std::move(drawStamps_));
    }
}

void CommandBuffer::finish(Status s) {
    status_ = s;
    if (s == kCompleted && counts_.binnedPasses != 0) queue_.checkVsc();
    if (s == kCompleted && tuning_) {
        for (const PassTime& t : passTimes()) {
            if (t.tuneKey != 0) queue_.tuned(t.tuneKey, t.tiles != 0, t.us);
        }
    }
    for (auto& h : handlers_) {
        h(*this);
    }
    handlers_.clear();
    kept_.clear();
    slab_.reset();
    queue_.giveSlabs(&slabs_);   // (done with: the GPU has finished with this buffer, or never had it)
    keptAny_.clear();
    {
        std::lock_guard<std::mutex> g(lock_);
        finished_ = true;
    }
    done_.notify_all();
}

bool CommandBuffer::copy(const Buffer& from, uint64_t fromOffset, const Buffer& to, uint64_t toOffset, uint64_t length) {
    closePass();
    if (status_ > kEnqueued) {
        return fail("copy after commit");
    }
    if (fromOffset > from.length() || length > from.length() - fromOffset || toOffset > to.length() ||
        length > to.length() - toOffset) {
        return fail("copy outside a buffer");
    }
    if (length == 0) {
        return true;
    }
    if (fromOffset % 4 || toOffset % 4 || length % 4) {
        // the command processor moves words; a copy of bytes goes through the 2D engine instead (a pass of its own).
        // MapKit's snapshot service copies rows of 2 bytes into its textures.
        std::string why;
        Piece table;
        if (!piece(render::kTableBytes, &table, &why)) {
            return fail(std::string("no memory for the copy's table: ") + why);
        }
        pm4::Stream added;
        render::encodeBytes(added, from.gpuAddress() + fromOffset, to.gpuAddress() + toOffset, length, table.gpuAddress(), table.contents());
        appendStream(stream_, added);
        use(from, false);
        use(to, true);
        return true;
    }
    uint64_t words = length / 4, done = 0;
    while (done < words) {
        uint32_t n = uint32_t(std::min<uint64_t>(words - done, kMemcpyMax));
        stream_.memcpy(to.gpuAddress() + toOffset + 4 * done, from.gpuAddress() + fromOffset + 4 * done, n);
        // what comes after may read what this wrote: the copy lands before the next packet is taken
        stream_.waitMemWrites();
        stream_.waitForMe();
        done += n;
    }
    use(from, false);
    use(to, true);
    return true;
}

bool CommandBuffer::fill(const Buffer& b, uint64_t offset, uint64_t length, uint8_t value) {
    closePass();
    if (status_ > kEnqueued) {
        return fail("fill after commit");
    }
    if (offset % 4 || length % 4) {
        return fail("fill of " + std::to_string(length) + " bytes at " + std::to_string(offset) +
                    ": the command processor writes words, so both must be multiples of 4");
    }
    if (offset > b.length() || length > b.length() - offset) {
        return fail("fill outside the buffer");
    }
    if (length == 0) {
        return true;
    }
    uint32_t word = uint32_t(value) * 0x01010101u;
    uint64_t words = length / 4, base = b.gpuAddress() + offset;
    // the first words written as they are, ...
    uint32_t head = uint32_t(std::min<uint64_t>(words, kFillHead));
    std::vector<uint32_t> pattern(head, word);
    stream_.memWrite(base, pattern.data(), head);
    stream_.waitMemWrites();
    stream_.waitForMe();
    // ... and the rest copied from what is already filled, twice as much each time
    uint64_t have = head;
    while (have < words) {
        uint32_t n = uint32_t(std::min<uint64_t>(std::min<uint64_t>(have, words - have), kMemcpyMax));
        stream_.memcpy(base + 4 * have, base, n);
        stream_.waitMemWrites();
        stream_.waitForMe();
        have += n;
    }
    use(b, true);
    return true;
}

// --- CommandQueue -----------------------------------------------------------------------------------------------

std::unique_ptr<CommandQueue> CommandQueue::make(Gpu& gpu, std::string* why) {
    uint32_t id = 0;
    int e = gpu.newQueue(0, &id);
    if (e != 0) {
        *why = std::string("the driver gave no submission queue: ") + strerror(e);
        return nullptr;
    }
    std::unique_ptr<CommandQueue> q(new CommandQueue(gpu, id));
    q->thread_ = std::thread([p = q.get()] { p->waiter(); });
    return q;
}

CommandQueue::~CommandQueue() {
    {
        std::lock_guard<std::mutex> g(lock_);
        quit_ = true;
    }
    changed_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    idle_.clear();
    gpu_.closeQueue(id_);
}

std::shared_ptr<CommandBuffer> CommandQueue::commandBuffer() {
    return std::shared_ptr<CommandBuffer>(new CommandBuffer(*this));
}

std::shared_ptr<Texture> CommandQueue::nullTexture(std::string* why) {
    std::lock_guard<std::mutex> g(lock_);
    if (null_ == nullptr) {
        TextureDesc d;
        d.format = PixelFormat::kRGBA8Unorm;
        std::shared_ptr<Texture> t = Texture::make(gpu_, d, why);
        // (written, not taken as the kernel's fresh zeros: the zero is the claim, so it is put there in so many words)
        const uint32_t zero = 0;
        if (t == nullptr || !t->replace(0, 0, 1, 1, &zero, 4, why)) return nullptr;
        null_ = std::move(t);
    }
    return null_;
}

namespace {
// the SP cores and fibers an SP (freedreno_devices.py: num_sp_cores = num_ccu; the A635, 7c+ Gen 3, has 2, the A730
// (FD730) 4; fibers_per_sp 128 * 2 * 16 on both). RedMagic 7 (v66): this was 2 on the A730 too, so the private
// memory was half what its four SPs address: SPs 2 and 3 wrote past the 4 MiB buffer into the next allocation --
// chronod's uber_vertex program, its first words replaced by the corners of a quad (+-1, +-1, 0.5, 1) every 0x200
// bytes -- and the GPU hung running them.
uint32_t spCores() { return a7xx::isA7xx() ? 4u : 2u; }
constexpr uint32_t kFibersPerSp = 128 * 2 * 16;
}  // namespace

std::shared_ptr<Buffer> CommandQueue::privateMemory(uint32_t bytes, bool perWave, PrivateMemory* out, std::string* why) {
    std::lock_guard<std::mutex> g(lock_);
    const int l = perWave ? 1 : 0;
    if (privatePerFiber_[l] < bytes) {
        uint32_t perFiber = 512;
        while (perFiber < bytes) perFiber *= 2;
        const uint64_t perSp = (uint64_t(perFiber) * kFibersPerSp + 4095) & ~uint64_t(4095);
        std::shared_ptr<Buffer> b = newBuffer(gpu_, perSp * spCores(), why);
        if (b == nullptr) return nullptr;
        private_[l].push_back(std::move(b));
        privatePerFiber_[l] = perFiber;
    }
    const uint32_t perFiber = privatePerFiber_[l];
    out->address = private_[l].back()->gpuAddress();
    out->perFiber = perFiber;
    out->perSp = uint32_t((uint64_t(perFiber) * kFibersPerSp + 4095) & ~uint64_t(4095));
    out->perWave = perWave;
    return private_[l].back();
}

size_t CommandQueue::idleCommandBuffers() {
    std::lock_guard<std::mutex> g(lock_);
    return idle_.size();
}

std::unique_ptr<Bo> CommandQueue::takeCommandBo(size_t bytes, int* error) {
    {
        std::lock_guard<std::mutex> g(lock_);
        for (auto it = idle_.begin(); it != idle_.end(); ++it) {
            if ((*it)->size() >= bytes) {
                std::unique_ptr<Bo> bo = std::move(*it);
                idle_.erase(it);
                return bo;
            }
        }
    }
    return gpu_.newBo(std::max(bytes, kCommandBo), kBoGpuReadOnly | kBoWriteCombined, error);
}


// RedMagic 7 (v71): boot-arg metal_validate (hakios_metal_set_validate). Before a command buffer reaches the GPU, every
// GPU address its stream makes the GPU read is checked against the buffers the submission names (its uses and its
// command buffer): the vertex fetches each draw turns on, the index buffer, the VS/FS programs and their private memory,
// the colour and depth targets, and what each CP_LOAD_STATE6 loads from memory -- the texture descriptors' base and flag
// addresses, the UBO descriptors -- following the GMEM bodies it calls. What does not land in one is printed with
// the draw it belongs to, so a run names every bad address, not only the first one the GPU stalls on.
std::atomic<bool> gValidate{false};

namespace {

struct VRange {
    uint64_t at, size;
    const uint8_t* cpu;
};

class Validator {
public:
    Validator(std::vector<VRange> ranges, uint64_t cbSerial) : ranges_(std::move(ranges)), cb_(cbSerial) {}

    void walk(const uint32_t* w, size_t n, int depth) {
        for (size_t i = 0; i < n;) {
            const uint32_t h = w[i];
            const uint32_t kind = h >> 28;
            if (kind == 4) {
                const uint32_t cnt = h & 0x7f, reg = (h >> 8) & 0x7ffff;
                for (uint32_t k = 0; k < cnt && i + 1 + k < n; k++) regs_[reg + k] = w[i + 1 + k];
                i += 1 + cnt;
                continue;
            }
            if (kind != 7) {
                i++;
                continue;
            }
            const uint32_t cnt = h & 0x3fff, op = (h >> 16) & 0x7f;
            const uint32_t* b = w + i + 1;
            const size_t have = std::min<size_t>(cnt, n - i - 1);
            if (op == 0x3f && have >= 3 && depth == 0) {   // CP_INDIRECT_BUFFER: a GMEM body
                const uint64_t at = uint64_t(b[0]) | uint64_t(b[1]) << 32;
                const VRange* r = find(at, 4ull * b[2]);
                if (r == nullptr) {
                    report("IB at " + hex(at) + " of " + std::to_string(b[2]) + " dwords is in no buffer of the submission");
                } else if (r->cpu != nullptr) {
                    inBody_ = true;
                    walk(reinterpret_cast<const uint32_t*>(r->cpu + (at - r->at)), b[2], 1);
                    inBody_ = false;
                }
            } else if ((op == 0x30 || op == 0x32 || op == 0x34 || op == 0x36) && have >= 3) {
                load(b[0], uint64_t(b[1] & ~3u) | uint64_t(b[2]) << 32);
            } else if (op == 0x38 || op == 0x2a) {   // CP_DRAW_INDX_OFFSET / CP_DRAW_INDIRECT_MULTI
                draw(b, have, op);
            }
            i += 1 + cnt;
        }
    }

    unsigned problems() const { return problems_; }

private:
    static std::string hex(uint64_t v) {
        char s[24];
        snprintf(s, sizeof(s), "0x%llx", (unsigned long long)v);
        return s;
    }
    uint32_t reg(uint32_t r) const {
        auto it = regs_.find(r);
        return it == regs_.end() ? 0 : it->second;
    }
    bool has(uint32_t r) const { return regs_.count(r) != 0; }
    uint64_t reg64(uint32_t r) const { return uint64_t(reg(r)) | uint64_t(reg(r + 1)) << 32; }
    const VRange* find(uint64_t at, uint64_t bytes) const {
        for (const VRange& r : ranges_) {
            if (at >= r.at && at + std::max<uint64_t>(bytes, 1) <= r.at + r.size) return &r;
        }
        return nullptr;
    }
    void check(const char* what, uint64_t at, uint64_t bytes) {
        if (find(at, bytes) == nullptr) report(std::string(what) + " at " + hex(at) + " (" + std::to_string(bytes) + " bytes) is in no buffer of the submission");
    }
    void report(const std::string& s) {
        problems_++;
        static std::atomic<unsigned> printed{0};
        if (printed.fetch_add(1) < 400) {
            fprintf(stderr, "HakiosMetal[%s %d]: validate: cb %llu draw %u (vs %s fs %s): %s\n", getprogname(), getpid(),
                    (unsigned long long)cb_, draws_, hex(reg64(0xa81c)).c_str(), hex(reg64(0xa983)).c_str(), s.c_str());
        }
    }
    // what a CP_LOAD_STATE6 loads from memory: the descriptors in it
    void load(uint32_t w0, uint64_t at) {
        const uint32_t type = (w0 >> 14) & 3, src = (w0 >> 16) & 3, block = (w0 >> 18) & 0xf, units = (w0 >> 22) & 0x3ff;
        if (src != 2) return;   // SS6_INDIRECT only: the data is in memory
        uint32_t unitBytes = 0;
        const bool texBlock = block <= 6 || block == 14 || block == 13;   // SB6_xS_TEX, SB6_IBO, SB6_CS_SHADER (+ST6_IBO)
        if (type == 1 && texBlock && block != 13) unitBytes = 64;         // texture / IBO descriptors
        else if (type == 3) unitBytes = 64;                               // ST6_IBO
        else if (type == 2) unitBytes = 8;                                // ST6_UBO
        else if (type == 0 && block <= 6) unitBytes = 16;                 // samplers
        else if (type == 0) unitBytes = 128;                              // a program: instruction groups
        else unitBytes = 16;                                              // constants: vec4s
        const VRange* r = find(at, uint64_t(units) * unitBytes);
        if (r == nullptr) {
            report("CP_LOAD_STATE6 (type " + std::to_string(type) + " block " + std::to_string(block) + ", " + std::to_string(units) +
                   " units) from " + hex(at) + ": not in a buffer of the submission");
            return;
        }
        if (r->cpu == nullptr) return;
        const uint32_t* d = reinterpret_cast<const uint32_t*>(r->cpu + (at - r->at));
        for (uint32_t u = 0; u < units; u++) {
            if (unitBytes == 64 && (type == 1 || type == 3)) {
                const uint32_t* t = d + 16 * u;
                bool zero = true;
                for (int k = 0; k < 16; k++) zero = zero && t[k] == 0;
                const std::string slot = "texture/IBO descriptor " + std::to_string(u) + " (block " + std::to_string(block) + ")";
                if (zero) {
                    report(slot + " is all zeros");
                    continue;
                }
                const uint64_t base = uint64_t(t[4] & 0xffffffe0u) | uint64_t(t[5] & 0x1ffffu) << 32;
                // (in a GMEM body a framebuffer-fetch descriptor's base is an offset in GMEM, not an address)
                const bool gmemOffset = inBody_ && base < 0x200000;
                if (!gmemOffset && (base == 0 || find(base, 1) == nullptr)) report(slot + ": base " + hex(base) + " is in no buffer of the submission");
                if (t[3] & 0x10000000u) {   // TEX_CONST_3 FLAG: UBWC, with a flag buffer
                    const uint64_t flag = uint64_t(t[7] & 0xffffffe0u) | uint64_t(t[8] & 0x1ffffu) << 32;
                    if (flag == 0 || find(flag, 1) == nullptr) report(slot + ": UBWC flag buffer " + hex(flag) + " is in no buffer of the submission");
                }
            } else if (type == 2) {
                const uint64_t a = uint64_t(d[2 * u]) | uint64_t(d[2 * u + 1] & 0x1ffffu) << 32;
                const uint32_t vec4s = d[2 * u + 1] >> 17;
                if (vec4s != 0 && find(a, 16ull * vec4s) == nullptr)
                    report("UBO descriptor " + std::to_string(u) + " (block " + std::to_string(block) + "): " + hex(a) + " for " +
                           std::to_string(vec4s) + " vec4s is in no buffer of the submission");
            }
        }
    }
    void draw(const uint32_t* b, size_t have, uint32_t op) {
        draws_++;
        // the fetch slots VFD_CONTROL_0 turns on
        const uint32_t fetches = reg(0xa000) & 0x3f;
        for (uint32_t s = 0; s < fetches; s++) {
            const uint32_t r = 0xa010 + 4 * s;
            if (!has(r)) {
                report("vertex fetch slot " + std::to_string(s) + " is on (FETCH_CNT " + std::to_string(fetches) + ") and its BASE was never written");
                continue;
            }
            const uint32_t size = reg(r + 2);
            if (find(reg64(r), size) == nullptr) report("vertex fetch slot " + std::to_string(s) + ": " + hex(reg64(r)) + " size " + std::to_string(size));
        }
        if (op == 0x38 && have >= 7 && ((b[0] >> 6) & 3) == 0) {   // DI_SRC_SEL_DMA: an index buffer
            const uint32_t bytes = (((b[0] >> 10) & 3) == 0 ? 1u : ((b[0] >> 10) & 3) == 1 ? 2u : 4u) * b[2];
            check("index buffer", uint64_t(b[4]) | uint64_t(b[5]) << 32, bytes);
        }
        check("VS program", reg64(0xa81c), 1);
        check("FS program", reg64(0xa983), 1);
        if ((reg(0xa821) & 0x3ffff) != 0) check("VS private memory", reg64(0xa81f), 1);
        if ((reg(0xa988) & 0x3ffff) != 0) check("FS private memory", reg64(0xa986), 1);
        for (uint32_t m = 0; m < 8; m++) {
            const uint32_t r = 0x8820 + 8 * m;
            if (((reg(r) >> 7) & 0xf) != 0 && has(r + 5)) check(("colour target " + std::to_string(m)).c_str(), reg64(r + 5), 1);
        }
        if ((reg(0x8872) & 0x7) != 0) check("depth buffer", reg64(0x8875), 1);
    }

    std::vector<VRange> ranges_;
    std::map<uint32_t, uint32_t> regs_;
    uint64_t cb_;
    uint32_t draws_ = 0;
    unsigned problems_ = 0;
    bool inBody_ = false;
};

}  // namespace

void CommandQueue::submit(std::shared_ptr<CommandBuffer> cb) {
    cb->committedAt_ = gpu_.transport().nowNs();
    if (!cb->error_.empty()) {
        cb->finish(CommandBuffer::kError);   // a blit that was refused: nothing of it reaches the GPU
        return;
    }
    cb->closePass();   // the last pass's tail
    // every command buffer ends with the GPU idle: what Metal's completion promises, that the work is done
    cb->stream_.waitMemWrites();
    cb->stream_.waitForIdle();
    const std::vector<uint32_t>& w = cb->stream_.words();
    int error = 0;
    cb->commands_ = takeCommandBo(4 * w.size(), &error);
    if (cb->commands_ == nullptr) {
        cb->error_ = std::string("no memory for the command stream: ") + strerror(error);
        cb->finish(CommandBuffer::kError);
        return;
    }
    // the stream is written in order, as write-combined memory wants it, and read back by nobody
    memcpy(cb->commands_->cpu(), w.data(), 4 * w.size());
    if (gValidate.load(std::memory_order_relaxed)) {
        static std::atomic<uint64_t> serial{0};
        std::vector<VRange> ranges;
        ranges.push_back(VRange{cb->commands_->gpuAddress(), cb->commands_->size(), static_cast<const uint8_t*>(cb->commands_->cpu())});
        for (const BoUse& u : cb->uses_) {
            if (u.bo != nullptr) ranges.push_back(VRange{u.bo->gpuAddress(), u.bo->size(), static_cast<const uint8_t*>(u.bo->cpu())});
        }
        Validator v(std::move(ranges), serial.fetch_add(1) + 1);
        v.walk(w.data(), w.size(), 0);
    }
    std::unique_lock<std::mutex> g(lock_);  // the fences of one queue are handed out in commit order
    cb->status_ = CommandBuffer::kCommitted;
    cb->faultsAtCommit_ = gpu_.faults();
    int e = gpu_.submit(id_, *cb->commands_, 0, uint32_t(w.size()), cb->uses_, &cb->fence_);
    if (e != 0) {
        cb->error_ = std::string("the driver refused the submission: ") + strerror(e);
        idle_.push_back(std::move(cb->commands_));
        g.unlock();                         // (the handlers may commit more: not under the queue's lock)
        cb->finish(CommandBuffer::kError);
        return;
    }
    cb->status_ = CommandBuffer::kScheduled;
    inFlight_.push_back(std::move(cb));
    changed_.notify_all();
}

void CommandQueue::waiter() {
    std::unique_lock<std::mutex> g(lock_);
    for (;;) {
        changed_.wait(g, [this] { return quit_ || !inFlight_.empty(); });
        if (inFlight_.empty()) {
            return;  // quitting, and nothing left in flight
        }
        std::shared_ptr<CommandBuffer> cb = inFlight_.front();
        g.unlock();
        int e = gpu_.wait(id_, cb->fence_, kPatienceNs);
        cb->completedAt_ = gpu_.transport().nowNs();
        // A fence that passed is not yet a submission that ran. When the kernel resets a hung GPU (gpu_recover=1) it
        // gives up everything the GPU had been given, the hung submission and those behind it in the ring, by setting
        // the fence word to the last submission's number -- so their waits end as if they had run (hakios_gpu_recover:
        // "submissions up to N are given up as done"). FAULTS counts the resets; one more since the commit says so.
        uint64_t faults = gpu_.faults();
        bool givenUp = faults > cb->faultsAtCommit_;
        if (e == 0 && givenUp) {
            cb->error_ = "given up by a reset of the GPU (it hung on this submission or on one before it): not run";
            e = EIO;
        } else if (e == ETIMEDOUT) {
            cb->error_ = givenUp ? "the GPU did not finish it in 5 s: it hung, and the kernel has reset it (reset " +
                                       std::to_string(faults) + " of this run)"
                                 : "the GPU did not finish it in 5 s and is not answering; without gpu_recover=1 the "
                                   "kernel waits for nothing again in this run";
        } else if (e != 0) {
            cb->error_ = std::string("waiting for it failed: ") + strerror(e);
        }
        cb->finish(e == 0 ? CommandBuffer::kCompleted : CommandBuffer::kError);
        g.lock();
        inFlight_.pop_front();
        if (cb->commands_ != nullptr) {
            idle_.push_back(std::move(cb->commands_));
        }
    }
}

}  // namespace hakios::metal
