#include "runtime/gs/gs_threaded_backend.h"
#include "runtime/gs/gs_recording.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_pcsx2_backend.h"
#include "runtime/gs/gs_shared_present.h"
#include "ThreadPriority.h"
#include "ThreadNaming.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>
#include <xmmintrin.h>

namespace
{
    // Asynchronous readback of tiny local->host transfers (docs/PROGRESS.md §50). Urban Reign reads an 8x8 block of the
    // depth buffer about once a frame (its light-flare visibility test, sub_003155F8). Synchronously, each one waits for
    // the GS thread to finish everything queued and for the GPU (~9.5% of the game thread in the heaviest map). For such
    // transfers the game gets the data of the previous transfer with the same buffer, format and size (one frame
    // late; the position is not part of the key since the flare moves with the camera), and the GS thread reads the
    // new block in the background. The first time, it is read synchronously as before. Other downloads stay exact.
    // PS2X_GS_ASYNC_READBACK=0 turns it off; =all also covers 8x8 color blocks.
    int asyncReadbackMode()
    {
        static const int mode = []
        {
            const char *v = std::getenv("PS2X_GS_ASYNC_READBACK");
            if (v && *v == '0')
                return 0;
            if (v && std::strcmp(v, "all") == 0)
                return 2;
            return 1;
        }();
        return mode;
    }

    bool isDepthFormat(uint8_t psm)
    {
        return psm == 0x30u || psm == 0x31u || psm == 0x32u || psm == 0x3Au;
    }

    uint32_t bitsPerPixel(uint8_t psm)
    {
        switch (psm)
        {
        case 0x00u: case 0x30u: return 32u;
        case 0x01u: case 0x31u: return 24u;
        case 0x02u: case 0x0Au: case 0x32u: case 0x3Au: return 16u;
        case 0x13u: return 8u;
        case 0x14u: return 4u;
        default: return 0u;
        }
    }

    using ReadbackKey = std::tuple<uint32_t, uint8_t, uint8_t, uint16_t, uint16_t>; // sbp, sbw, spsm, w, h

    struct AsyncReadback
    {
        std::mutex mutex;
        std::map<ReadbackKey, std::vector<uint8_t>> cache;
        // EE side: bytes of a cache hit being consumed instead of the GS thread's transfer.
        std::vector<uint8_t> serve;
        size_t servePos = 0u;
        bool serving = false;
        // EE side: a first (synchronous) transfer whose bytes go into the cache as they are consumed.
        bool capturing = false;
        ReadbackKey captureKey{};
        size_t captureExpected = 0u;
        std::vector<uint8_t> capture;
        // GS thread side: refreshes queued by cache hits.
        std::deque<std::pair<ReadbackKey, GSTransferCommand>> refresh;
        GSRasterBackend *inner = nullptr; // the backend whose GS thread runs the refreshes
        uint64_t hits = 0u, misses = 0u;
    };
    AsyncReadback g_readback;

    constexpr size_t kPublishBytes = 64u * 1024u;
    constexpr size_t kMaxPendingChunks = 256u;
    std::atomic<bool> s_enabledByDefault{false};
    std::atomic<bool> s_gpuByDefault{false};

    bool discardDraws()
    {
        static const bool discard = []
        {
            const char *value = std::getenv("PS2X_GS_DISCARD_DRAWS");
            return value && *value && *value != '0';
        }();
        return discard;
    }

    struct LoadClutArgs
    {
        GSTex0Reg tex0;
        GSTexClutReg texclut;
    };

    struct WriteVramArgs
    {
        uint32_t psm;
        uint32_t base;
        uint32_t bw;
        uint32_t x;
        uint32_t y;
        uint32_t value;
    };

    template <typename T>
    T readAt(const uint8_t *data)
    {
        T value;
        std::memcpy(&value, data, sizeof(T));
        return value;
    }
}

void ps2xGsNoteHostShownMidClear(bool shown); // present at the vblank block below

GSThreadedBackend::GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner)
    : m_inner(std::move(inner))
{
    m_rawGif = m_inner && m_inner->WantsRawGif();
    m_gpuInner = dynamic_cast<GSGpuBackend *>(m_inner.get());
    const char *compact = std::getenv("PS2X_GS_COMPACT_QUEUE");
    m_compactQueue = !compact || *compact != '0';
    // 64 chunks (4 MB) in flight by default instead of 8: the game thread produces draws in bursts (each VU1 job's
    // XGKICK output) and waited for room ~8.6% of the time in Urban Reign's heaviest 4-fighter map while this thread
    // sat idle the rest of the frame (37.6/40.2 -> 44.6 FPS; 256 gives the same as 64).
    m_queueChunkLimit = 64u;
    if (const char *limit = std::getenv("PS2X_GS_QUEUE_CHUNKS"))
        m_queueChunkLimit = static_cast<size_t>(std::clamp(std::atoi(limit), 1, static_cast<int>(kMaxPendingChunks)));
    m_open.reserve(kPublishBytes + 4096u);
    m_worker = std::thread([this]
                           { WorkerLoop(); });
}

GSThreadedBackend::~GSThreadedBackend()
{
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_stop = true;
        m_pending.clear();
    }
    m_workCv.notify_all();
    if (m_worker.joinable())
        m_worker.join();
}

std::unique_ptr<GSRasterBackend> GSThreadedBackend::MakeDefault()
{
    const char *value = std::getenv("PS2X_GS_THREAD");
    const bool enabled = (value && *value) ? *value != '0' : s_enabledByDefault.load();
    const char *gpu = std::getenv("PS2X_GS_GPU");
    const bool useGpu = (gpu && *gpu) ? *gpu != '0' : s_gpuByDefault.load();
    if (GSPcsx2Backend::Requested())
        return std::make_unique<GSThreadedBackend>(std::make_unique<GSPcsx2Backend>());
    if (useGpu)
        return std::make_unique<GSThreadedBackend>(std::make_unique<GSGpuBackend>());
    if (!enabled)
        return std::make_unique<GSCpuBackend>();
    return std::make_unique<GSThreadedBackend>(std::make_unique<GSCpuBackend>());
}

void GSThreadedBackend::SetEnabledByDefault(bool enabled)
{
    s_enabledByDefault.store(enabled);
}

void GSThreadedBackend::SetGpuByDefault(bool enabled)
{
    s_gpuByDefault.store(enabled);
}

void GSThreadedBackend::Enqueue(Op op, const void *payload, uint32_t size)
{
    std::lock_guard<std::mutex> lock(m_producerMutex);
    const uint32_t csr = _mm_getcsr();
    if (csr != m_lastMxcsr)
    {
        m_lastMxcsr = csr;
        const uint8_t header = static_cast<uint8_t>(Op::Mxcsr);
        const uint32_t csrSize = sizeof(csr);
        m_open.push_back(header);
        m_open.insert(m_open.end(), reinterpret_cast<const uint8_t *>(&csrSize), reinterpret_cast<const uint8_t *>(&csrSize) + 4);
        m_open.insert(m_open.end(), reinterpret_cast<const uint8_t *>(&csr), reinterpret_cast<const uint8_t *>(&csr) + 4);
    }
    const auto append = [&](Op kind, const void *bytes, uint32_t length)
    {
        uint8_t header[5];
        header[0] = static_cast<uint8_t>(kind);
        std::memcpy(header + 1u, &length, sizeof(length));
        m_open.insert(m_open.end(), header, header + sizeof(header));
        if (length)
            m_open.insert(m_open.end(), static_cast<const uint8_t *>(bytes), static_cast<const uint8_t *>(bytes) + length);
    };
    if (op == Op::Submit && m_compactQueue)
    {
        // One primitive per vertex kick, so this runs ~a million times a second on the GIF thread. The records are
        // built in a fixed-size local buffer (copies of known size, inlined) and appended with one insert, instead of
        // 2-3 vector inserts of variable size, each an out-of-line memmove of a few dozen bytes (PROGRESS §70).
        const auto &draw = *static_cast<const GSPrimitiveBatch *>(payload);
        constexpr uint32_t kStateBytes = sizeof(draw.state);
        constexpr uint32_t kVertexBytes = static_cast<uint32_t>(offsetof(GSPrimitiveBatch, state));
        alignas(16) uint8_t record[2u * 5u + kStateBytes + kVertexBytes];
        size_t used = 0u;
        if (!m_drawStateValid || std::memcmp(&m_lastDrawState, &draw.state, kStateBytes) != 0)
        {
            std::memcpy(&m_lastDrawState, &draw.state, kStateBytes);
            m_drawStateValid = true;
            record[used] = static_cast<uint8_t>(Op::DrawState);
            std::memcpy(record + used + 1u, &kStateBytes, sizeof(kStateBytes));
            std::memcpy(record + used + 5u, &draw.state, kStateBytes);
            used += 5u + kStateBytes;
        }
        record[used] = static_cast<uint8_t>(Op::Vertices);
        std::memcpy(record + used + 1u, &kVertexBytes, sizeof(kVertexBytes));
        std::memcpy(record + used + 5u, &draw, kVertexBytes);
        used += 5u + kVertexBytes;
        m_open.insert(m_open.end(), record, record + used);
    }
    else
        append(op, payload, size);
    if (m_open.size() >= kPublishBytes)
        PublishLocked();
}

namespace
{
    // PS2X_GS_WAIT_STATS=1 (round 10 diagnostic): every 2 s, how long producers waited for the GS worker: in
    // PublishLocked for a free chunk slot (queue full) and in Drain (synchronous calls: readbacks, RunOnWorker).
    const bool g_gsWaitStats = [] { const char *v = std::getenv("PS2X_GS_WAIT_STATS"); return v && *v == '1'; }();
    std::atomic<uint64_t> g_publishWaits{0}, g_publishWaitNs{0}, g_publishes{0}, g_drains{0}, g_drainWaitNs{0};
    std::atomic<int64_t> g_gsWaitWindowNs{0};
    std::atomic<int64_t> g_gsWaitEpochNs{0};

    int64_t gsWaitNowNs()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void gsWaitStatsMaybePrint()
    {
        const int64_t now = gsWaitNowNs();
        int64_t start = g_gsWaitWindowNs.load(std::memory_order_relaxed);
        if (start == 0)
        {
            g_gsWaitWindowNs.compare_exchange_strong(start, now);
            int64_t zero = 0;
            g_gsWaitEpochNs.compare_exchange_strong(zero, now);
            return;
        }
        if (now - start < 2000000000 || !g_gsWaitWindowNs.compare_exchange_strong(start, now))
            return;
        std::fprintf(stderr, "[gs-wait] t=%.2f publishes=%llu queue-full waits=%llu %.1f ms, drains=%llu %.1f ms\n",
                     static_cast<double>(now - g_gsWaitEpochNs.load()) / 1e9,
                     static_cast<unsigned long long>(g_publishes.exchange(0)),
                     static_cast<unsigned long long>(g_publishWaits.exchange(0)), static_cast<double>(g_publishWaitNs.exchange(0)) / 1e6,
                     static_cast<unsigned long long>(g_drains.exchange(0)), static_cast<double>(g_drainWaitNs.exchange(0)) / 1e6);
    }
}

void GSThreadedBackend::PublishLocked()
{
    if (m_open.empty())
        return;
    std::vector<uint8_t> next;
    {
        std::unique_lock<std::mutex> lock(m_queueMutex);
        if (g_gsWaitStats)
        {
            g_publishes.fetch_add(1, std::memory_order_relaxed);
            if (!(m_stop || m_pending.size() < m_queueChunkLimit))
            {
                const int64_t t0 = gsWaitNowNs();
                m_doneCv.wait(lock, [&]
                              { return m_stop || m_pending.size() < m_queueChunkLimit; });
                g_publishWaits.fetch_add(1, std::memory_order_relaxed);
                g_publishWaitNs.fetch_add(static_cast<uint64_t>(gsWaitNowNs() - t0), std::memory_order_relaxed);
            }
        }
        m_doneCv.wait(lock, [&]
                      { return m_stop || m_pending.size() < m_queueChunkLimit; });
        m_pending.push_back(std::move(m_open));
        ++m_published;
        if (!m_spare.empty())
        {
            next = std::move(m_spare.back());
            m_spare.pop_back();
        }
    }
    m_workCv.notify_one();
    if (g_gsWaitStats)
        gsWaitStatsMaybePrint();
    next.clear();
    m_open = std::move(next);
    if (m_open.capacity() < kPublishBytes)
        m_open.reserve(kPublishBytes + 4096u);
}

// Present at the vblank: the latch should reach the GS thread at once, but the EE thread must not wait at its vblank. With
// the queue full (the GS thread 4 MB behind) the chunk stays open and goes with the next publish.
void GSThreadedBackend::TryPublishLocked()
{
    if (m_open.empty())
        return;
    std::vector<uint8_t> next;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_stop || m_pending.size() >= m_queueChunkLimit)
            return;
        m_pending.push_back(std::move(m_open));
        ++m_published;
        if (!m_spare.empty())
        {
            next = std::move(m_spare.back());
            m_spare.pop_back();
        }
    }
    m_workCv.notify_one();
    next.clear();
    m_open = std::move(next);
    if (m_open.capacity() < kPublishBytes)
        m_open.reserve(kPublishBytes + 4096u);
}

void GSThreadedBackend::Drain() const
{
    auto *self = const_cast<GSThreadedBackend *>(this);
    uint64_t target = 0u;
    {
        std::lock_guard<std::mutex> lock(m_producerMutex);
        self->PublishLocked();
        std::lock_guard<std::mutex> queueLock(m_queueMutex);
        target = m_published;
    }
    std::unique_lock<std::mutex> lock(m_queueMutex);
    if (g_gsWaitStats)
    {
        const int64_t t0 = gsWaitNowNs();
        m_doneCv.wait(lock, [&]
                      { return m_completed >= target || m_stop; });
        g_drains.fetch_add(1, std::memory_order_relaxed);
        g_drainWaitNs.fetch_add(static_cast<uint64_t>(gsWaitNowNs() - t0), std::memory_order_relaxed);
        lock.unlock();
        gsWaitStatsMaybePrint();
        return;
    }
    m_doneCv.wait(lock, [&]
                  { return m_completed >= target || m_stop; });
}

void GSThreadedBackend::WorkerLoop()
{
    ThreadNaming::SetCurrentThreadName("GSThread");
    ThreadPriority::RaiseCurrentThread(ThreadPriority::Level::AboveNormal);
    GSPrimitiveBatch draw;
    for (;;)
    {
        std::vector<uint8_t> chunk;
        bool idle = false;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            // Present at the vblank: while a present readback is in flight, look for it about every 1 ms even when idle
            // (a timed wait, not a spin; busy, it is looked for between chunks every 0.2 ms).
            if (m_latchPolling)
                m_workCv.wait_for(lock, std::chrono::milliseconds(1), [&]
                                  { return m_stop || !m_pending.empty(); });
            else
                m_workCv.wait(lock, [&]
                              { return m_stop || !m_pending.empty(); });
            if (m_stop)
            {
                m_inner.reset();
                return;
            }
            if (m_pending.empty())
                idle = true;
            else
            {
                chunk = std::move(m_pending.front());
                m_pending.pop_front();
            }
        }
        if (idle)
        {
            std::lock_guard<std::mutex> exec(m_execMutex);
            PollLatched();
            continue;
        }
        {
            std::lock_guard<std::mutex> exec(m_execMutex);
            Execute(chunk, draw);
            if (m_latchPolling && std::chrono::steady_clock::now() - m_lastPoll >= std::chrono::microseconds(200))
                PollLatched();
        }
        // Bug recorder: a copy of the executed chunk goes into the ring (GS thread only; the chunk itself is recycled as
        // before).
        if (m_ringEnabled.load(std::memory_order_acquire))
            RingCapture(chunk);
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            ++m_completed;
            if (m_spare.size() < 64u)
                m_spare.push_back(std::move(chunk));
        }
        m_doneCv.notify_all();
    }
}

void GSThreadedBackend::Execute(const std::vector<uint8_t> &chunk, GSPrimitiveBatch &draw)
{
    const uint8_t *data = chunk.data();
    const size_t size = chunk.size();
    size_t offset = 0u;
    while (offset + 5u <= size)
    {
        const Op op = static_cast<Op>(data[offset]);
        const uint32_t length = readAt<uint32_t>(data + offset + 1u);
        const uint8_t *payload = data + offset + 5u;
        offset += 5u + length;
        switch (op)
        {
        case Op::Mxcsr:
            _mm_setcsr(readAt<uint32_t>(payload));
            break;
        case Op::Submit:
        {
            const GSPrimitiveBatch batch = readAt<GSPrimitiveBatch>(payload);
            if (!discardDraws())
                m_inner->Submit(batch);
            if (m_watchRect.valid && batch.state.context.frame.fbp == m_watchRect.fbp)
                WatchDisplayDraw(batch);
            break;
        }
        case Op::LoadClut:
        {
            const LoadClutArgs args = readAt<LoadClutArgs>(payload);
            m_inner->LoadClut(args.tex0, args.texclut);
            break;
        }
        case Op::BeginTransfer:
            m_inner->BeginTransfer(readAt<GSTransferCommand>(payload));
            break;
        case Op::Upload:
            m_inner->UploadImage(payload, length);
            break;
        case Op::TextureFlush:
            m_inner->TextureFlush();
            break;
        case Op::WriteVram:
        {
            const WriteVramArgs a = readAt<WriteVramArgs>(payload);
            m_inner->WriteVram(a.psm, a.base, a.bw, a.x, a.y, a.value);
            break;
        }
        case Op::Sync:
            m_inner->Sync(readAt<GSSyncReason>(payload));
            break;
        case Op::DrawState:
            std::memcpy(&draw.state, payload, sizeof(draw.state));
            break;
        case Op::Vertices:
            std::memcpy(&draw, payload, offsetof(GSPrimitiveBatch, state));
            if (!discardDraws())
                m_inner->Submit(draw);
            if (m_watchRect.valid && draw.state.context.frame.fbp == m_watchRect.fbp)
                WatchDisplayDraw(draw);
            break;
        case Op::Present:
            m_inner->Present(readAt<GSPresentationRequest>(payload));
            break;
        case Op::Call:
            (*readAt<const std::function<void()> *>(payload))();
            break;
        case Op::RawGif:
            // PCSX2's GIF parser loads whole qwords with aligned vector loads; the queue packs records at any byte.
            if ((reinterpret_cast<uintptr_t>(payload) & 15u) != 0u)
            {
                static thread_local std::vector<__m128> aligned;
                aligned.resize((length + 15u) / 16u);
                std::memcpy(aligned.data(), payload, length);
                m_inner->RawGif(reinterpret_cast<const uint8_t *>(aligned.data()), length);
            }
            else
                m_inner->RawGif(payload, length);
            break;
        case Op::VSync:
            m_inner->VSync(readAt<GSVSyncRegs>(payload));
            break;
        case Op::GsReset:
            m_inner->GsReset();
            break;
        case Op::RingMark:
            RingAtMark(payload, static_cast<uint32_t>(offset - 5u - length), draw);
            break;
        case Op::RingRecord:
            break;
        case Op::LatchPresent:
            ExecuteLatch(payload, length);
            break;
        }
    }
}

void GSThreadedBackend::RunOnWorker(const std::function<void()> &function) const
{
    auto *self = const_cast<GSThreadedBackend *>(this);
    const std::function<void()> *pointer = &function;
    self->Enqueue(Op::Call, &pointer, sizeof(pointer));
    Drain();
}

// ---- Bug recorder GS ring (gs_recording.h GSRing) ---------------------------------------------------------------------
// The GS thread keeps the chunks it has executed (this queue's own format) instead of recycling them at once. Field
// marks (Op::RingMark, from GSRingRecorderBackend on the game thread) split them into fields; every 15 fields a mark
// starts a segment with a State checkpoint taken right there in stream order (GPU: asynchronous copy). Dropped chunks go
// back to the spare pool, so the steady state allocates nothing.

namespace GSRing
{
    struct Checkpoint
    {
        uint64_t tick = 0u;
        std::array<uint64_t, 7> display{};
        std::chrono::steady_clock::time_point requested{};
        std::mutex mutex;
        std::condition_variable cv;
        int status = 0; // 0 pending, 1 ready, -1 failed
        std::vector<uint8_t> vram;
        GSBackendState state;

        void Finish(std::vector<uint8_t> &&bytes, GSBackendState &&backendState, bool ok);
        bool Wait(int waitMs);
    };

    struct Collected
    {
        struct Start
        {
            uint64_t seq = 0u;
            uint32_t offset = 0u; // of the start mark in that chunk
            std::shared_ptr<Checkpoint> checkpoint;
            GSDrawState drawState{};
            uint32_t mxcsr = 0u;
        };
        std::vector<Start> starts;
        struct Span
        {
            uint64_t seq = 0u;
            std::shared_ptr<const void> block; // keeps the ring block alive
            size_t offset = 0u;
            size_t size = 0u;
            const uint8_t *data = nullptr;
        };
        std::vector<Span> chunks;
        bool hasEnd = false;
        uint64_t endSeq = 0u;
        uint32_t endOffset = 0u;
        std::shared_ptr<Checkpoint> end;
    };
}

namespace
{
    constexpr uint32_t kRingSegmentFields = 15u; // a State checkpoint every 15 fields (~0.25 s)
    constexpr size_t kRingKeepFields = 60u;      // the oldest segment goes once the newer ones hold >= 60 fields
    constexpr size_t kRingCheckpointBytes = 4u << 20u;

    struct RingMarkPayload
    {
        uint64_t tick;
        std::array<uint64_t, 7> display;
        uint32_t restart;
        uint32_t reserved;
    };

    constexpr size_t kRingBlockBytes = 4u << 20u; // chunks are copied into 4 MB blocks
    constexpr size_t kRingFreeBlocks = 4u;

    struct RingBlock
    {
        std::vector<uint8_t> bytes; // fixed size, never reallocated (readers keep spans of it)
        size_t used = 0u;
        uint64_t lastSeq = 0u;
    };

    struct RingChunk
    {
        uint64_t seq;
        std::shared_ptr<RingBlock> block;
        size_t offset;
        size_t size;
    };

    struct RingSegment
    {
        uint64_t seq;
        uint32_t offset;
        std::shared_ptr<GSRing::Checkpoint> checkpoint;
        GSDrawState drawState;
        uint32_t mxcsr;
        size_t fields;
    };

    struct RingEvent
    {
        uint32_t offset = 0u;
        bool start = false;
        bool restart = false;
        uint64_t ticket = 0u;
        std::shared_ptr<GSRing::Checkpoint> checkpoint;
        GSDrawState drawState{};
        uint32_t mxcsr = 0u;
    };

    struct RingStore
    {
        std::mutex mutex;
        std::condition_variable cv;
        std::deque<RingChunk> chunks;
        std::deque<RingSegment> segments;
        size_t bytes = 0u;
        size_t budget = size_t(256) << 20u;
        bool active = false;
        std::atomic<uint64_t> requested{0u};
        uint64_t served = 0u;
        std::shared_ptr<GSRing::Collected> handed;
        uint64_t checkpoints = 0u;
        uint64_t failed = 0u;
        uint64_t dropped = 0u;
        uint64_t restarts = 0u;
        uint64_t delayCount = 0u;
        double delayMsSum = 0.0;
        double delayMsMax = 0.0;
        // GS thread only.
        uint64_t nextSeq = 0u;
        uint32_t fieldsInSegment = 0u;
        bool needSegment = true;
        uint64_t claimedTicket = 0u;
        std::vector<RingEvent> events;
        std::deque<std::shared_ptr<RingBlock>> blocks; // in use, oldest first
        std::vector<std::shared_ptr<RingBlock>> freeBlocks;
    };

    RingStore &ringStore()
    {
        static RingStore *store = new RingStore(); // never destroyed: a checkpoint may finish on the GS thread at exit
        return *store;
    }

    // Removes chunks older than seq, then the blocks only they used (back to the free list unless a writer still holds
    // them). Store locked; GS thread.
    void dropChunksBefore(RingStore &s, uint64_t seq)
    {
        while (!s.chunks.empty() && s.chunks.front().seq < seq)
            s.chunks.pop_front();
        while (s.blocks.size() > 1u && s.blocks.front()->lastSeq < seq)
        {
            std::shared_ptr<RingBlock> block = std::move(s.blocks.front());
            s.blocks.pop_front();
            s.bytes -= block->bytes.size();
            if (block.use_count() == 1 && block->bytes.size() == kRingBlockBytes && s.freeBlocks.size() < kRingFreeBlocks)
            {
                block->used = 0u;
                block->lastSeq = 0u;
                s.freeBlocks.push_back(std::move(block));
            }
        }
    }
}

namespace GSRing
{
    void Checkpoint::Finish(std::vector<uint8_t> &&bytes, GSBackendState &&backendState, bool ok)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            vram = std::move(bytes);
            state = std::move(backendState);
            status = ok ? 1 : -1;
        }
        cv.notify_all();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - requested).count();
        RingStore &store = ringStore();
        std::lock_guard<std::mutex> lock(store.mutex);
        if (!ok)
            ++store.failed;
        ++store.delayCount;
        store.delayMsSum += ms;
        store.delayMsMax = std::max(store.delayMsMax, ms);
    }

    bool Checkpoint::Wait(int waitMs)
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait_for(lock, std::chrono::milliseconds(waitMs), [&] { return status != 0; });
        return status == 1;
    }

    bool Active() { return GSThreadedBackend::RingActive(); }
    std::shared_ptr<Collected> Collect(int waitMs, size_t keepSegments)
    {
        return GSThreadedBackend::RingCollect(waitMs, keepSegments);
    }
    bool Write(const Collected &collected, const std::string &path, std::string &summary)
    {
        return GSThreadedBackend::RingWrite(collected, path, summary);
    }
    std::string Stats() { return GSThreadedBackend::RingStats(); }
}

void GSThreadedBackend::EnableRing()
{
    RingStore &s = ringStore();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.active = true;
        if (const char *mb = std::getenv("PS2X_BUG_RECORDER_GS_MB"); mb && *mb)
            s.budget = static_cast<size_t>(std::clamp(std::atoi(mb), 16, 4096)) << 20u;
    }
    m_ringEnabled.store(true, std::memory_order_release);
}

void GSThreadedBackend::RingMark(uint64_t tick, const std::array<uint64_t, 7> &display, bool restart)
{
    const RingMarkPayload mark{tick, display, restart ? 1u : 0u, 0u};
    Enqueue(Op::RingMark, &mark, sizeof(mark));
}

void GSThreadedBackend::RingRecord(uint8_t op, const void *payload, uint32_t size)
{
    std::vector<uint8_t> record(1u + size);
    record[0] = op;
    if (size != 0u)
        std::memcpy(record.data() + 1u, payload, size);
    Enqueue(Op::RingRecord, record.data(), static_cast<uint32_t>(record.size()));
}

// GS thread, inside Execute: a field mark at opOffset of the chunk being executed (stream order).
void GSThreadedBackend::RingAtMark(const uint8_t *payload, uint32_t opOffset, const GSPrimitiveBatch &draw)
{
    if (!m_ringEnabled.load(std::memory_order_relaxed))
        return;
    RingStore &s = ringStore();
    RingMarkPayload mark{};
    std::memcpy(&mark, payload, sizeof(mark));
    RingEvent event;
    event.offset = opOffset;
    if (mark.restart)
    {
        event.restart = true;
        s.needSegment = true;
        s.events.clear(); // marks earlier in this chunk belong to the stream before the restart
    }
    const uint64_t requested = s.requested.load(std::memory_order_acquire);
    uint64_t served = 0u;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        served = s.served;
    }
    const bool dump = requested > served && requested != s.claimedTicket;
    if (s.needSegment || s.fieldsInSegment >= kRingSegmentFields || dump)
    {
        auto checkpoint = std::make_shared<GSRing::Checkpoint>();
        checkpoint->tick = mark.tick;
        checkpoint->display = mark.display;
        checkpoint->requested = std::chrono::steady_clock::now();
        if (auto *gpu = dynamic_cast<GSGpuBackend *>(m_inner.get()))
        {
            const bool started = gpu->BeginSnapshotAsync([checkpoint](std::vector<uint8_t> &&vram, GSBackendState &&state)
                                                         {
                                                             const bool ok = !vram.empty();
                                                             checkpoint->Finish(std::move(vram), std::move(state), ok);
                                                         });
            if (!started)
                checkpoint->Finish({}, GSBackendState{}, false);
        }
        else
        {
            // CPU back end: its VRAM is host memory, copied here (on the GS thread).
            m_inner->Flush();
            std::vector<uint8_t> vram;
            m_inner->SnapshotVram(vram);
            GSBackendState state;
            bool ok = !vram.empty();
            if (auto *access = dynamic_cast<GSBackendStateAccess *>(m_inner.get()))
                ok = access->ExportState(state) && ok;
            checkpoint->Finish(std::move(vram), std::move(state), ok);
        }
        event.start = true;
        event.checkpoint = std::move(checkpoint);
        event.drawState = draw.state;
        event.mxcsr = _mm_getcsr();
        if (dump)
        {
            event.ticket = requested;
            s.claimedTicket = requested;
        }
        s.fieldsInSegment = 0u;
        s.needSegment = false;
    }
    ++s.fieldsInSegment;
    s.events.push_back(std::move(event));
}

// GS thread, after Execute: a copy of the chunk joins the ring; segments/handover/trim.
void GSThreadedBackend::RingCapture(const std::vector<uint8_t> &chunk)
{
    RingStore &s = ringStore();
    const uint64_t seq = s.nextSeq++;
    const size_t size = chunk.size();
    // The copy is made outside the lock: blocks are only appended here, and readers only read the spans they were given.
    std::shared_ptr<RingBlock> block = s.blocks.empty() ? nullptr : s.blocks.back();
    bool newBlock = false;
    if (!block || block->used + size > block->bytes.size())
    {
        if (size <= kRingBlockBytes && !s.freeBlocks.empty())
        {
            block = std::move(s.freeBlocks.back());
            s.freeBlocks.pop_back();
        }
        else
        {
            block = std::make_shared<RingBlock>();
            block->bytes.resize(std::max(size, kRingBlockBytes));
        }
        newBlock = true;
    }
    const size_t offset = block->used;
    if (size != 0u)
        std::memcpy(block->bytes.data() + offset, chunk.data(), size);
    block->used += size;
    block->lastSeq = seq;
    bool handed = false;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (newBlock)
        {
            s.blocks.push_back(block);
            s.bytes += block->bytes.size();
        }
        s.chunks.push_back({seq, block, offset, size});
        for (RingEvent &event : s.events)
        {
            if (event.restart)
            {
                for (const RingSegment &segment : s.segments)
                {
                    (void)segment;
                    s.bytes -= kRingCheckpointBytes;
                }
                s.segments.clear();
                dropChunksBefore(s, seq);
                ++s.restarts;
            }
            if (event.start)
            {
                if (event.ticket != 0u && event.ticket > s.served)
                {
                    auto collected = std::make_shared<GSRing::Collected>();
                    for (const RingSegment &segment : s.segments)
                        collected->starts.push_back({segment.seq, segment.offset, segment.checkpoint, segment.drawState, segment.mxcsr});
                    if (!s.segments.empty())
                        for (const RingChunk &ringChunk : s.chunks)
                            if (ringChunk.seq >= s.segments.front().seq)
                                collected->chunks.push_back({ringChunk.seq, ringChunk.block, ringChunk.offset, ringChunk.size,
                                                             ringChunk.block->bytes.data() + ringChunk.offset});
                    collected->hasEnd = true;
                    collected->endSeq = seq;
                    collected->endOffset = event.offset;
                    collected->end = event.checkpoint;
                    s.handed = std::move(collected);
                    s.served = event.ticket;
                    handed = true;
                }
                s.segments.push_back({seq, event.offset, event.checkpoint, event.drawState, event.mxcsr, 1u});
                s.bytes += kRingCheckpointBytes;
                ++s.checkpoints;
            }
            else if (!s.segments.empty())
                ++s.segments.back().fields;
        }
        s.events.clear();
        if (s.segments.empty())
            dropChunksBefore(s, seq + 1u); // nothing replayable before the first checkpoint
        while (s.segments.size() > 1u)
        {
            size_t newer = 0u;
            for (size_t i = 1u; i < s.segments.size(); ++i)
                newer += s.segments[i].fields;
            if (newer < kRingKeepFields && s.bytes <= s.budget)
                break;
            s.bytes -= kRingCheckpointBytes;
            s.segments.pop_front();
            ++s.dropped;
            dropChunksBefore(s, s.segments.front().seq);
        }
    }
    if (handed)
        s.cv.notify_all();
}

bool GSThreadedBackend::RingActive()
{
    RingStore &s = ringStore();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.active;
}

std::shared_ptr<GSRing::Collected> GSThreadedBackend::RingCollect(int waitMs, size_t keepSegments)
{
    RingStore &s = ringStore();
    std::unique_lock<std::mutex> lock(s.mutex);
    if (!s.active)
        return nullptr;
    const uint64_t ticket = s.requested.fetch_add(1u, std::memory_order_acq_rel) + 1u;
    std::shared_ptr<GSRing::Collected> result;
    if (s.cv.wait_for(lock, std::chrono::milliseconds(waitMs), [&] { return s.served >= ticket; }) && s.handed)
        result = s.handed;
    else
    {
        // No field mark came (no GS activity): the ring as it is, without an End checkpoint.
        result = std::make_shared<GSRing::Collected>();
        for (const RingSegment &segment : s.segments)
            result->starts.push_back({segment.seq, segment.offset, segment.checkpoint, segment.drawState, segment.mxcsr});
        if (!s.segments.empty())
            for (const RingChunk &chunk : s.chunks)
                if (chunk.seq >= s.segments.front().seq)
                    result->chunks.push_back({chunk.seq, chunk.block, chunk.offset, chunk.size, chunk.block->bytes.data() + chunk.offset});
        s.served = std::max(s.served, s.requested.load(std::memory_order_acquire)); // withdraw the request
    }
    s.handed.reset();
    lock.unlock();
    // A shorter stream (automatic bundles): only the newest keepSegments segments; RingWrite starts at the first one
    // left and skips the chunks before it.
    if (keepSegments != 0u && result->starts.size() > keepSegments)
        result->starts.erase(result->starts.begin(),
                             result->starts.end() - static_cast<std::ptrdiff_t>(keepSegments));
    while (!result->starts.empty() && !result->starts.front().checkpoint->Wait(waitMs))
        result->starts.erase(result->starts.begin()); // its checkpoint failed: the stream starts at the next one
    if (result->end && !result->end->Wait(waitMs))
        result->end.reset(); // the stream still ends there; End then has no VRAM
    return result;
}

bool GSThreadedBackend::RingWrite(const GSRing::Collected &collected, const std::string &path, std::string &summary)
{
    if (collected.starts.empty() || collected.chunks.empty())
    {
        summary = "no GS commands in the ring";
        return false;
    }
    const GSRing::Collected::Start &start = collected.starts.front();
    size_t first = 0u;
    while (first < collected.chunks.size() && collected.chunks[first].seq != start.seq)
        ++first;
    if (first == collected.chunks.size())
    {
        summary = "ring start chunk missing";
        return false;
    }
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (!file)
    {
        summary = "cannot write " + path;
        return false;
    }
    std::setvbuf(file, nullptr, _IOFBF, 4u << 20u);
    static constexpr char kGsrMagic[8] = {'P', 'S', '2', 'X', 'G', 'S', 'R', '1'};
    std::fwrite(kGsrMagic, 1, sizeof(kGsrMagic), file);
    uint64_t written = sizeof(kGsrMagic);
    const auto record = [&](GSRecording::Op op, const void *payload, uint32_t size)
    {
        const uint8_t opByte = static_cast<uint8_t>(op);
        std::fwrite(&opByte, 1, 1, file);
        std::fwrite(&size, sizeof(size), 1, file);
        if (size != 0u)
            std::fwrite(payload, 1, size, file);
        written += 5u + size;
    };
    std::vector<uint8_t> scratch;
    const auto put = [&](const auto &value)
    {
        const auto *p = reinterpret_cast<const uint8_t *>(&value);
        scratch.insert(scratch.end(), p, p + sizeof(value));
    };
    const auto checkpointRecord = [&](GSRecording::Op op, uint64_t tick, const std::array<uint64_t, 7> &display,
                                      const GSRing::Checkpoint *full)
    {
        scratch.clear();
        put(tick);
        put(display);
        if (full)
        {
            put(static_cast<uint32_t>(full->vram.size()));
            scratch.insert(scratch.end(), full->vram.begin(), full->vram.end());
            GSRecording::AppendState(scratch, full->state);
        }
        else
        {
            put(uint32_t{0u});
            GSRecording::AppendState(scratch, GSBackendState{});
        }
        record(op, scratch.data(), static_cast<uint32_t>(scratch.size()));
    };

    checkpointRecord(GSRecording::Op::State, start.checkpoint->tick, start.checkpoint->display, start.checkpoint.get());
    record(GSRecording::Op::Mxcsr, &start.mxcsr, sizeof(start.mxcsr));
    GSPrimitiveBatch batch{};
    batch.state = start.drawState;
    constexpr uint32_t kVertexBytes = static_cast<uint32_t>(offsetof(GSPrimitiveBatch, state));
    bool firstMark = true;
    uint64_t lastTick = start.checkpoint->tick;
    std::array<uint64_t, 7> lastDisplay = start.checkpoint->display;
    size_t fields = 0u;
    size_t draws = 0u;
    for (size_t index = first; index < collected.chunks.size(); ++index)
    {
        const uint64_t seq = collected.chunks[index].seq;
        const uint8_t *data = collected.chunks[index].data;
        const size_t chunkSize = collected.chunks[index].size;
        size_t offset = index == first ? start.offset : 0u;
        const bool last = collected.hasEnd && seq == collected.endSeq;
        const size_t end = last ? std::min<size_t>(collected.endOffset, chunkSize) : chunkSize;
        while (offset + 5u <= end)
        {
            const Op op = static_cast<Op>(data[offset]);
            uint32_t length = 0u;
            std::memcpy(&length, data + offset + 1u, sizeof(length));
            const uint8_t *payload = data + offset + 5u;
            offset += 5u + length;
            switch (op)
            {
            case Op::Mxcsr:
                record(GSRecording::Op::Mxcsr, payload, length);
                break;
            case Op::Submit:
                record(GSRecording::Op::Submit, payload, length);
                ++draws;
                break;
            case Op::LoadClut:
                record(GSRecording::Op::LoadClut, payload, length);
                break;
            case Op::BeginTransfer:
                record(GSRecording::Op::BeginTransfer, payload, length);
                break;
            case Op::Upload:
                record(GSRecording::Op::Upload, payload, length);
                break;
            case Op::TextureFlush:
                record(GSRecording::Op::TextureFlush, nullptr, 0u);
                break;
            case Op::WriteVram:
                record(GSRecording::Op::WriteVram, payload, length);
                break;
            case Op::DrawState:
                std::memcpy(static_cast<void *>(&batch.state), payload, std::min<size_t>(length, sizeof(batch.state)));
                break;
            case Op::Vertices:
                std::memcpy(static_cast<void *>(&batch), payload, std::min<size_t>(length, kVertexBytes));
                record(GSRecording::Op::Submit, &batch, sizeof(batch));
                ++draws;
                break;
            case Op::RingMark:
            {
                RingMarkPayload mark{};
                std::memcpy(&mark, payload, std::min<size_t>(length, sizeof(mark)));
                if (firstMark)
                    firstMark = false; // the State record stands for it
                else
                    checkpointRecord(GSRecording::Op::Field, mark.tick, mark.display, nullptr);
                lastTick = mark.tick;
                lastDisplay = mark.display;
                ++fields;
                break;
            }
            case Op::RingRecord:
                if (length >= 1u)
                    record(static_cast<GSRecording::Op>(payload[0]), payload + 1u, length - 1u);
                break;
            default: // Sync, Present, Call (synchronous calls, async readbacks), raw GIF: nothing for the replay
                break;
            }
        }
        if (last)
            break;
    }
    const bool fullEnd = collected.end && collected.end->status == 1;
    if (fullEnd)
        checkpointRecord(GSRecording::Op::End, collected.end->tick, collected.end->display, collected.end.get());
    else
        checkpointRecord(GSRecording::Op::End, lastTick + 1u, lastDisplay, nullptr);
    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    const std::string endText = fullEnd ? "at " + std::to_string(collected.end->tick) + " with VRAM" : std::string("without VRAM");
    char text[256];
    std::snprintf(text, sizeof(text), "%zu fields (ticks %llu-%llu), %zu draws, State at %llu, End %s, %.1f MB", fields,
                  static_cast<unsigned long long>(start.checkpoint->tick), static_cast<unsigned long long>(lastTick), draws,
                  static_cast<unsigned long long>(start.checkpoint->tick), endText.c_str(),
                  static_cast<double>(written) / (1024.0 * 1024.0));
    summary = text;
    return ok;
}

std::string GSThreadedBackend::RingStats()
{
    RingStore &s = ringStore();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.active)
        return "GS ring off";
    size_t fields = 0u;
    for (const RingSegment &segment : s.segments)
        fields += segment.fields;
    const uint64_t firstTick = s.segments.empty() || !s.segments.front().checkpoint ? 0u : s.segments.front().checkpoint->tick;
    char text[320];
    std::snprintf(text, sizeof(text),
                  "GS ring %zu fields from tick %llu in %zu segments, %.1f MB (%zu chunks); checkpoints %llu (failed %llu, "
                  "delay avg %.1f max %.1f ms), segments dropped %llu, restarts %llu",
                  fields, static_cast<unsigned long long>(firstTick), s.segments.size(),
                  static_cast<double>(s.bytes) / (1024.0 * 1024.0), s.chunks.size(), static_cast<unsigned long long>(s.checkpoints),
                  static_cast<unsigned long long>(s.failed), s.delayCount ? s.delayMsSum / s.delayCount : 0.0, s.delayMsMax,
                  static_cast<unsigned long long>(s.dropped), static_cast<unsigned long long>(s.restarts));
    s.delayMsMax = 0.0;
    return text;
}

void GSThreadedBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    RunOnWorker([&]
                {
        m_clearPending = false; // VRAM replaced (start-up, save state): the display-clear watch starts again
        m_inner->Initialize(vram, vramSize); });
}

void GSThreadedBackend::Reset()
{
    RunOnWorker([&]
                {
        m_clearPending = false;
        m_inner->Reset(); });
}

void GSThreadedBackend::Submit(const GSPrimitiveBatch &batch)
{
    Enqueue(Op::Submit, &batch, sizeof(batch));
}

void GSThreadedBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    const LoadClutArgs args{tex0, texclut};
    Enqueue(Op::LoadClut, &args, sizeof(args));
}

void GSThreadedBackend::BeginTransfer(const GSTransferCommand &command)
{
    const int mode = asyncReadbackMode();
    const GSBitBltBuf &b = command.bitbltbuf;
    const uint32_t bpp = bitsPerPixel(b.spsm);
    // "all" takes blocks up to 16x16: Urban Reign's flare color read becomes 1x16..7x16 when the sun nears the screen
    // edge, and those stayed synchronous (~3% of the game thread with 8 fighters; docs/PROGRESS.md §64).
    const uint32_t tinyLimit = mode == 2 ? 16u : 8u;
    const bool tiny = command.direction == 1u && command.trxreg.rrw != 0u && command.trxreg.rrh != 0u &&
                      command.trxreg.rrw <= tinyLimit && command.trxreg.rrh <= tinyLimit && bpp != 0u;
    if (mode != 0 && tiny && (mode == 2 || isDepthFormat(b.spsm)))
    {
        const ReadbackKey key{b.sbp, b.sbw, b.spsm, static_cast<uint16_t>(command.trxreg.rrw), static_cast<uint16_t>(command.trxreg.rrh)};
        bool hit = false;
        {
            std::lock_guard<std::mutex> lock(g_readback.mutex);
            const auto it = g_readback.cache.find(key);
            hit = it != g_readback.cache.end();
            if (hit)
            {
                g_readback.serve = it->second;
                g_readback.servePos = 0u;
                g_readback.serving = true;
                g_readback.capturing = false;
                g_readback.refresh.emplace_back(key, command);
                g_readback.inner = m_inner.get();
                ++g_readback.hits;
            }
            else
            {
                g_readback.serving = false;
                g_readback.capturing = true;
                g_readback.captureKey = key;
                g_readback.captureExpected = static_cast<size_t>(command.trxreg.rrw) * command.trxreg.rrh * bpp / 8u;
                g_readback.capture.clear();
                ++g_readback.misses;
            }
        }
        if (hit)
        {
            // The GS thread reads the new block when it gets there, in order with the draws before it.
            static const std::function<void()> refresher = []
            {
                for (;;)
                {
                    std::pair<ReadbackKey, GSTransferCommand> job;
                    GSRasterBackend *inner = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(g_readback.mutex);
                        if (g_readback.refresh.empty() || !g_readback.inner)
                            return;
                        job = g_readback.refresh.front();
                        g_readback.refresh.pop_front();
                        inner = g_readback.inner;
                    }
                    // On the GPU back end the read itself does not wait either (a synchronous one waited for
                    // everything queued before it: ~half the GS thread at 2x internal resolution with 8 fighters).
                    if (auto *gpu = dynamic_cast<GSGpuBackend *>(inner))
                    {
                        const ReadbackKey key = job.first;
                        if (gpu->BeginDownloadAsync(job.second, [key](std::vector<uint8_t> &&bytes)
                                                    {
                                                        std::lock_guard<std::mutex> lock(g_readback.mutex);
                                                        g_readback.cache[key] = std::move(bytes);
                                                    }))
                            continue;
                    }
                    // PCSX2's GS: the stream's own transfer is the game's (TRXDIR=1 went through the GIF); the
                    // refresh reads the area beside it (PROGRESS §83).
                    if (auto *pcsx2 = dynamic_cast<GSPcsx2Backend *>(inner))
                    {
                        std::vector<uint8_t> bytes;
                        if (pcsx2->ReadTransfer(job.second, bytes) && !bytes.empty())
                        {
                            std::lock_guard<std::mutex> lock(g_readback.mutex);
                            g_readback.cache[job.first] = std::move(bytes);
                        }
                        continue;
                    }
                    inner->BeginTransfer(job.second);
                    std::vector<uint8_t> bytes(static_cast<size_t>(job.second.trxreg.rrw) * job.second.trxreg.rrh *
                                               bitsPerPixel(job.second.bitbltbuf.spsm) / 8u);
                    const uint32_t got = inner->ConsumeLocalToHostBytes(bytes.data(), static_cast<uint32_t>(bytes.size()));
                    if (got == bytes.size())
                    {
                        std::lock_guard<std::mutex> lock(g_readback.mutex);
                        g_readback.cache[job.first] = std::move(bytes);
                    }
                }
            };
            const std::function<void()> *pointer = &refresher;
            Enqueue(Op::Call, &pointer, sizeof(pointer));
            return;
        }
    }
    else if (command.direction == 1u || command.direction == 2u)
    {
        std::lock_guard<std::mutex> lock(g_readback.mutex);
        g_readback.serving = false;
        g_readback.capturing = false;
    }
    Enqueue(Op::BeginTransfer, &command, sizeof(command));
}

void GSThreadedBackend::RawGif(const uint8_t *data, uint32_t sizeBytes)
{
    if (data && sizeBytes >= 16u)
        Enqueue(Op::RawGif, data, sizeBytes & ~15u);
}

// The field ends: published at once, so the GS thread presents it without waiting for more GIF data.
void GSThreadedBackend::VSync(const GSVSyncRegs &regs)
{
    Enqueue(Op::VSync, &regs, sizeof(regs));
    std::lock_guard<std::mutex> lock(m_producerMutex);
    PublishLocked();
}

void GSThreadedBackend::GsReset()
{
    Enqueue(Op::GsReset, nullptr, 0u);
}

void GSThreadedBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    Enqueue(Op::Upload, data, data ? sizeBytes : 0u);
}

void GSThreadedBackend::Flush()
{
}

void GSThreadedBackend::TextureFlush()
{
    Enqueue(Op::TextureFlush, nullptr, 0u);
}

void GSThreadedBackend::Sync(GSSyncReason reason)
{
    if (reason == GSSyncReason::Presentation && GSSharedPresent::Active())
        return;
    if (reason == GSSyncReason::Finish)
    {
        Enqueue(Op::Sync, &reason, sizeof(reason));
        return;
    }
    RunOnWorker([&] { m_inner->Sync(reason); });
}

PresentationFrame GSThreadedBackend::Present(const GSPresentationRequest &request)
{
    if (GSSharedPresent::Active())
    {
        Enqueue(Op::Present, &request, sizeof(request));
        std::lock_guard<std::mutex> lock(m_producerMutex);
        PublishLocked();
        return {};
    }
    // The old path (PS2X_PRESENT_AT_VBLANK=0, or before the first vblank latch): composed where the stream is now.
    PresentationFrame frame;
    RunOnWorker([&]
                {
        bool blank = false;
        const bool midClear = NoteComposite(request, "host present, old path", blank);
        const uint64_t tag = (uint64_t{1} << 63u) | (++m_hostPresentSeq << 1u) | (midClear ? 1u : 0u);
        if (m_gpuInner)
            m_gpuInner->SetPresentTag(tag);
        frame = m_inner->Present(request);
        // The GPU's asynchronous present returns an earlier composite (its tag tells whether that one was mid-clear).
        const uint64_t shown = (m_gpuInner && m_gpuInner->PresentIsAsync()) ? m_gpuInner->LastFrameTag() : tag;
        ps2xGsNoteHostShownMidClear(static_cast<bool>(frame) && (shown & 1u) != 0u); });
    return frame;
}

bool GSThreadedBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    bool result = false;
    RunOnWorker([&] { result = m_inner->ClearFramebuffer(context, rgba); });
    return result;
}

// ---- Present at the vblank (PS2X_PRESENT_AT_VBLANK, default on) --------------------------------------------------------
// GS::vblank (EE thread, gs_frontend.cpp) issues a latch number and queues Op::LatchPresent with the display registers
// of that vblank. The GS thread composes right there in stream order and publishes the image (CPU back end and
// synchronous GPU readback: at once; asynchronous GPU readback: once the GPU has finished it, picked up by polling
// between chunks, about every 1 ms when idle). The present thread (TakeLatched) waits up to a bounded time for the newest
// latch and otherwise keeps the last image. Nothing here makes the EE thread wait for the GS thread or the present
// thread, and the GS thread never waits for the present thread.
namespace
{
    struct LatchArgs
    {
        GSPresentationRequest request;
        uint64_t seq;
    };

    // EE thread: issued. GS thread: resolved, the image, blank. Present thread: takes. One per process (one present
    // thread); a GS back end that is replaced takes its latches with it (the present thread then times out once and
    // keeps its last image).
    struct LatchMailbox
    {
        std::mutex mutex;
        std::condition_variable cv;
        uint64_t issued = 0u;     // newest latch number issued
        uint64_t resolved = 0u;   // newest latch number with a result (an image, blank, or nothing new)
        uint64_t frameSeq = 0u;   // latch number of the image in pixels
        uint64_t blankSeq = 0u;   // newest latch that found no display circuit on
        uint64_t takenSeq = 0u;   // present thread: frameSeq of the image it took last
        uint64_t seenIssued = 0u; // present thread: issued at its last take
        std::vector<uint8_t> pixels;
        uint32_t width = 0u;
        uint32_t height = 0u;
        uint32_t displayFbp = 0u;
        uint32_t sourceFbp = 0u;
        bool usedPreferred = false;
        bool midClear = false;
    };

    LatchMailbox &latchMailbox()
    {
        static LatchMailbox *mailbox = new LatchMailbox(); // never destroyed: the GS thread may still publish at exit
        return *mailbox;
    }

    std::atomic<uint64_t> g_midClearComposites{0u};
    std::atomic<uint32_t> g_midClearLogged{0u};
    std::atomic<bool> g_hostShownMidClear{false};
    constexpr uint32_t kMidClearLogLines = 20u;

    // GS thread. frame: swapped into the mailbox (frame gets the mailbox's old buffer back for reuse).
    void latchPublish(uint64_t seq, PresentationFrame *frame, bool midClear, bool blank)
    {
        LatchMailbox &mb = latchMailbox();
        {
            std::lock_guard<std::mutex> lock(mb.mutex);
            if (blank)
                mb.blankSeq = std::max(mb.blankSeq, seq);
            else if (frame && seq > mb.frameSeq && seq > mb.blankSeq)
            {
                mb.pixels.swap(frame->pixels);
                mb.width = frame->width;
                mb.height = frame->height;
                mb.displayFbp = frame->displayFbp;
                mb.sourceFbp = frame->sourceFbp;
                mb.usedPreferred = frame->usedPreferred;
                mb.midClear = midClear;
                mb.frameSeq = seq;
            }
            mb.resolved = std::max(mb.resolved, seq);
        }
        mb.cv.notify_all();
    }
}

void ps2xGsNoteHostShownMidClear(bool shown)
{
    g_hostShownMidClear.store(shown, std::memory_order_relaxed);
}

bool GSThreadedBackend::PresentAtVblankRequested()
{
    static const bool on = []
    {
        const char *v = std::getenv("PS2X_PRESENT_AT_VBLANK");
        return !(v && *v == '0');
    }();
    return on;
}

bool GSThreadedBackend::HostPresentShownMidClear()
{
    return g_hostShownMidClear.load(std::memory_order_relaxed);
}

uint64_t GSThreadedBackend::MidClearComposites()
{
    return g_midClearComposites.load(std::memory_order_relaxed);
}

bool GSThreadedBackend::DisplayRectFromRegs(uint64_t pmode, uint64_t smode2, uint64_t dispfb1, uint64_t display1,
                                            uint64_t dispfb2, uint64_t display2, DisplayRect &out)
{
    // Same rules as the composite (decodeCircuit in gs_gpu_backend.cpp / gs_cpu_backend.cpp).
    const bool halfHeight = (smode2 & 0x3ull) == 0x3ull;
    const auto decode = [&](bool enabled, uint64_t dispfb, uint64_t display)
    {
        if (!enabled || display == 0u)
            return false;
        const int32_t hdiv = static_cast<int32_t>((display >> 23) & 0xFu) + 1;
        const int32_t vdiv = (static_cast<int32_t>((display >> 27) & 0x3u) + 1) * (halfHeight ? 2 : 1);
        const int32_t dw = static_cast<int32_t>((display >> 32) & 0xFFFu) + 1;
        const int32_t dh = static_cast<int32_t>((display >> 44) & 0x7FFu) + 1;
        if (dw < hdiv || dh < vdiv)
            return false;
        out.valid = true;
        out.fbp = static_cast<uint32_t>(dispfb & 0x1FFu);
        out.x0 = static_cast<int32_t>((dispfb >> 32) & 0x7FFu);
        out.y0 = static_cast<int32_t>((dispfb >> 43) & 0x7FFu);
        out.x1 = out.x0 + dw / hdiv - 1;
        out.y1 = out.y0 + dh / vdiv - 1;
        return true;
    };
    out = {};
    return decode((pmode & 0x1ull) != 0u, dispfb1, display1) || decode((pmode & 0x2ull) != 0u, dispfb2, display2);
}

// An untextured sprite over the whole displayed rectangle of the displayed frame buffer (colour not fully masked):
// sceGsSetDefClear-style clears (Haunting Ground: rgba 0 into FBP 0, 640x448, every game frame).
bool GSThreadedBackend::IsDisplayClear(const GSPrimitiveBatch &draw, const DisplayRect &rect)
{
    const GSDrawState &state = draw.state;
    const GSContext &ctx = state.context;
    if (!rect.valid || ctx.frame.fbp != rect.fbp || state.prim.tme || state.prim.type != GS_PRIM_SPRITE || draw.vertexCount < 2u)
        return false;
    if ((ctx.frame.fbmsk & 0x00FFFFFFu) == 0x00FFFFFFu)
        return false;
    const GSVertex &v0 = draw.vertices[0];
    const GSVertex &v1 = draw.vertices[1];
    int x0 = static_cast<int>(v0.x) - (ctx.xyoffset.ofx >> 4);
    int y0 = static_cast<int>(v0.y) - (ctx.xyoffset.ofy >> 4);
    int x1 = static_cast<int>(v1.x) - (ctx.xyoffset.ofx >> 4);
    int y1 = static_cast<int>(v1.y) - (ctx.xyoffset.ofy >> 4);
    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);
    const int xEnd = x0 + std::max(1, x1 - x0) - 1;
    const int yEnd = y0 + std::max(1, y1 - y0) - 1;
    return x0 <= rect.x0 && y0 <= rect.y0 && xEnd >= rect.x1 && yEnd >= rect.y1;
}

// GS thread, a draw into the displayed frame buffer: a clear sets the flag, the next textured draw (the copy) resets it.
void GSThreadedBackend::WatchDisplayDraw(const GSPrimitiveBatch &draw)
{
    if (draw.state.prim.tme)
        m_clearPending = false;
    else if (IsDisplayClear(draw, m_watchRect))
        m_clearPending = true;
}

// GS thread, at every composite (vblank latch or old path): the displayed buffer for the watch; true when this composite
// sees the display buffer cleared and not yet drawn again. blank: no display circuit on.
bool GSThreadedBackend::NoteComposite(const GSPresentationRequest &request, const char *path, bool &blank)
{
    DisplayRect rect;
    blank = !DisplayRectFromRegs(request.pmode, request.smode2, request.dispfb1, request.display1, request.dispfb2,
                                 request.display2, rect);
    if (!rect.valid || !m_watchRect.valid || rect.fbp != m_watchRect.fbp)
        m_clearPending = false; // another buffer shown: its state is not known
    m_watchRect = rect;
    if (blank || !m_clearPending)
        return false;
    const uint64_t count = g_midClearComposites.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const uint32_t logged = g_midClearLogged.fetch_add(1u, std::memory_order_relaxed);
    if (logged < kMidClearLogLines)
    {
        std::fprintf(stderr, "[present] composite between clear and copy: field %llu, display FBP %u (%s, #%llu)%s\n",
                     static_cast<unsigned long long>(request.vsyncTick), rect.fbp, path, static_cast<unsigned long long>(count),
                     logged + 1u == kMidClearLogLines ? "; further ones are only counted" : "");
        std::fflush(stderr);
    }
    return true;
}

uint64_t GSThreadedBackend::LatchIssue()
{
    LatchMailbox &mb = latchMailbox();
    std::lock_guard<std::mutex> lock(mb.mutex);
    return ++mb.issued;
}

// EE thread, at the vblank (GS::vblank holds the GS state lock). Appends the latch like Enqueue does, but publishes only
// when the queue has room, and never through PublishLocked's wait: the EE thread does not wait here.
void GSThreadedBackend::LatchPresent(const GSPresentationRequest &request, uint64_t seq)
{
    const LatchArgs args{request, seq};
    std::lock_guard<std::mutex> lock(m_producerMutex);
    const uint32_t csr = _mm_getcsr();
    if (csr != m_lastMxcsr)
    {
        m_lastMxcsr = csr;
        const uint8_t header = static_cast<uint8_t>(Op::Mxcsr);
        const uint32_t csrSize = sizeof(csr);
        m_open.push_back(header);
        m_open.insert(m_open.end(), reinterpret_cast<const uint8_t *>(&csrSize), reinterpret_cast<const uint8_t *>(&csrSize) + 4);
        m_open.insert(m_open.end(), reinterpret_cast<const uint8_t *>(&csr), reinterpret_cast<const uint8_t *>(&csr) + 4);
    }
    uint8_t header[5];
    header[0] = static_cast<uint8_t>(Op::LatchPresent);
    const uint32_t length = sizeof(args);
    std::memcpy(header + 1u, &length, sizeof(length));
    m_open.insert(m_open.end(), header, header + sizeof(header));
    m_open.insert(m_open.end(), reinterpret_cast<const uint8_t *>(&args), reinterpret_cast<const uint8_t *>(&args) + sizeof(args));
    TryPublishLocked();
}

// GS thread: the latch, in stream order.
void GSThreadedBackend::ExecuteLatch(const uint8_t *payload, uint32_t length)
{
    if (length < sizeof(LatchArgs))
        return;
    const LatchArgs args = readAt<LatchArgs>(payload);
    m_latchStarted = true;
    bool blank = false;
    const bool midClear = NoteComposite(args.request, "vblank latch", blank);
    m_inner->Sync(GSSyncReason::Presentation);
    if (m_gpuInner)
    {
        m_gpuInner->SetLatchedPresent(true);
        m_gpuInner->SetPresentTag((args.seq << 1u) | (midClear ? 1u : 0u));
    }
    PresentationFrame frame = m_inner->Present(args.request);
    if (blank)
        latchPublish(args.seq, nullptr, false, true);
    else if (m_gpuInner && m_gpuInner->PresentIsAsync())
    {
        if (!m_gpuInner->LastPresentQueued())
            latchPublish(args.seq, nullptr, false, false); // no free readback slot: nothing new for this latch
    }
    else
        latchPublish(args.seq, frame ? &frame : nullptr, midClear, false);
    PollLatched();
}

// GS thread: hands a finished asynchronous readback to the present thread (no wait).
void GSThreadedBackend::PollLatched()
{
    m_lastPoll = std::chrono::steady_clock::now();
    if (!m_gpuInner || !m_latchStarted)
    {
        m_latchPolling = false;
        return;
    }
    PresentationFrame frame;
    frame.pixels.swap(m_latchScratch);
    uint64_t tag = 0u;
    // Tags with the top bit come from old-path presents (before the first latch): not a latch image.
    if (m_gpuInner->TakePresentedFrame(frame, tag) && (tag >> 63u) == 0u)
        latchPublish(tag >> 1u, &frame, (tag & 1u) != 0u, false);
    m_latchScratch.swap(frame.pixels);
    m_latchPolling = m_gpuInner->PresentPending();
}

// Present thread.
GSThreadedBackend::LatchResult GSThreadedBackend::TakeLatched(int waitUs, std::vector<uint8_t> &pixels, LatchedInfo &info)
{
    LatchMailbox &mb = latchMailbox();
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mb.mutex);
    const uint64_t seen = mb.seenIssued;
    // Ready: a latch newer than at the last take was issued (this field's vblank) and the newest one has a result.
    const auto ready = [&] { return mb.issued > seen && mb.resolved >= mb.issued; };
    bool ok = ready();
    if (!ok && waitUs > 0)
        ok = mb.cv.wait_until(lock, start + std::chrono::microseconds(waitUs), ready);
    info = {};
    info.timedOut = !ok;
    info.waitedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    mb.seenIssued = mb.issued;
    if (mb.blankSeq > mb.frameSeq)
        return LatchResult::Blank;
    if (mb.frameSeq <= mb.takenSeq)
        return LatchResult::Repeat;
    mb.takenSeq = mb.frameSeq;
    pixels.swap(mb.pixels);
    info.width = mb.width;
    info.height = mb.height;
    info.displayFbp = mb.displayFbp;
    info.sourceFbp = mb.sourceFbp;
    info.usedPreferred = mb.usedPreferred;
    info.midClear = mb.midClear;
    info.seq = mb.frameSeq;
    return LatchResult::NewFrame;
}

// Save state loaded: a readback being served or captured belongs to the session before it (the cache stays).
void ps2xGsResetAsyncReadback()
{
    std::lock_guard<std::mutex> lock(g_readback.mutex);
    g_readback.serving = false;
    g_readback.capturing = false;
    g_readback.refresh.clear();
}

uint32_t GSThreadedBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    {
        std::lock_guard<std::mutex> lock(g_readback.mutex);
        if (g_readback.serving)
        {
            const size_t count = std::min<size_t>(maxBytes, g_readback.serve.size() - g_readback.servePos);
            if (dst && count)
                std::memcpy(dst, g_readback.serve.data() + g_readback.servePos, count);
            g_readback.servePos += count;
            if (g_readback.servePos >= g_readback.serve.size())
                g_readback.serving = false;
            return static_cast<uint32_t>(count);
        }
    }
    uint32_t result = 0u;
    RunOnWorker([&] { result = m_inner->ConsumeLocalToHostBytes(dst, maxBytes); });
    std::lock_guard<std::mutex> lock(g_readback.mutex);
    if (g_readback.capturing && dst && result)
    {
        g_readback.capture.insert(g_readback.capture.end(), dst, dst + result);
        if (g_readback.capture.size() >= g_readback.captureExpected)
        {
            g_readback.capture.resize(g_readback.captureExpected);
            g_readback.cache[g_readback.captureKey] = g_readback.capture;
            g_readback.capturing = false;
        }
    }
    return result;
}

uint32_t GSThreadedBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    uint32_t result = 0u;
    RunOnWorker([&] { result = m_inner->ReadVram(psm, base, bw, x, y); });
    return result;
}

void GSThreadedBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    const WriteVramArgs args{psm, base, bw, x, y, value};
    Enqueue(Op::WriteVram, &args, sizeof(args));
}

void GSThreadedBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    RunOnWorker([&] { m_inner->SnapshotVram(out); });
}

GSTransferSnapshot GSThreadedBackend::GetTransferSnapshot() const
{
    GSTransferSnapshot result;
    RunOnWorker([&] { result = m_inner->GetTransferSnapshot(); });
    return result;
}

bool GSThreadedBackend::ExportState(GSBackendState &out)
{
    bool result = false;
    RunOnWorker([&]
                {
        auto *access = dynamic_cast<GSBackendStateAccess *>(m_inner.get());
        result = access && access->ExportState(out); });
    return result;
}

bool GSThreadedBackend::ImportState(const GSBackendState &state)
{
    bool result = false;
    RunOnWorker([&]
                {
        auto *access = dynamic_cast<GSBackendStateAccess *>(m_inner.get());
        result = access && access->ImportState(state); });
    return result;
}
