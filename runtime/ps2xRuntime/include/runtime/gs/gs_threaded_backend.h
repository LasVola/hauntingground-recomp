#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_backend_state.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace GSRing
{
    struct Collected;
}

class GSGpuBackend;

class GSThreadedBackend final : public GSRasterBackend, public GSBackendStateAccess
{
public:
    explicit GSThreadedBackend(std::unique_ptr<GSRasterBackend> inner);
    ~GSThreadedBackend() override;

    static std::unique_ptr<GSRasterBackend> MakeDefault();
    static void SetEnabledByDefault(bool enabled);
    static void SetGpuByDefault(bool enabled);

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;

    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;

    bool WantsRawGif() const override { return m_rawGif; }
    void RawGif(const uint8_t *data, uint32_t sizeBytes) override;
    void VSync(const GSVSyncRegs &regs) override;
    void GsReset() override;

    bool ExportState(GSBackendState &out) override;
    bool ImportState(const GSBackendState &state) override;

    GSRasterBackend &Inner() { return *m_inner; }

    // Bug recorder GS ring (gs_recording.h GSRing; the game-thread side is GSRingRecorderBackend). EnableRing before the
    // first command; RingMark at every field change (restart: VRAM changed outside the stream); RingRecord adds a
    // .gsr record the queue does not carry (Reset, ClearFramebuffer run synchronously through Op::Call).
    void EnableRing();
    void RingMark(uint64_t tick, const std::array<uint64_t, 7> &display, bool restart);
    void RingRecord(uint8_t op, const void *payload, uint32_t size);
    static bool RingActive();
    static std::shared_ptr<GSRing::Collected> RingCollect(int waitMs, size_t keepSegments);
    static bool RingWrite(const GSRing::Collected &collected, const std::string &path, std::string &summary);
    static std::string RingStats();

    // ---- Present at the vblank (PS2X_PRESENT_AT_VBLANK, default on; =0 = the old path) ------------------------------
    // GS::vblank (EE thread) issues a latch and queues it here, in GS stream order: the GS thread composes the display
    // right there (after everything the game sent before the vblank, before anything after it) and publishes the image;
    // the present thread takes the newest one (TakeLatched: waits a bounded time for it, else the last image stays).
    // The old path composed at whatever point of the stream the game had reached when the present thread asked, e.g.
    // between Haunting Ground's black clear of the display buffer and its copy into it (a black frame;
    // port_prep/blackflash/ANALYSIS.md).
    static bool PresentAtVblankRequested();
    static uint64_t LatchIssue(); // EE thread: the number of a new latch (call LatchPresent with it right after)
    void LatchPresent(const GSPresentationRequest &request, uint64_t seq); // EE thread; never waits for the GS thread
    enum class LatchResult : uint8_t
    {
        NewFrame, // pixels/info hold a newer image than the last one taken
        Repeat,   // nothing newer (or not in time): the last image stays on screen
        Blank,    // the newest latch found no display circuit on: black is right
    };
    struct LatchedInfo
    {
        uint32_t width = 0u;
        uint32_t height = 0u;
        uint32_t displayFbp = 0u;
        uint32_t sourceFbp = 0u;
        bool usedPreferred = false;
        bool midClear = false; // composed while the display buffer was cleared and not yet drawn again (instrumentation)
        bool timedOut = false; // the newest latch was not ready within waitUs
        uint64_t seq = 0u;
        double waitedMs = 0.0;
    };
    // Present thread. pixels: swapped with the published image on NewFrame (pass the buffer back next time).
    static LatchResult TakeLatched(int waitUs, std::vector<uint8_t> &pixels, LatchedInfo &info);

    // Instrumentation (ANALYSIS.md section 5). The GS thread tracks whether the displayed frame buffer was cleared (an
    // untextured sprite over the whole display) and not drawn into with a texture since; a composite at such a point
    // logs "[present] composite between clear and copy" (first 20) and is counted.
    struct DisplayRect
    {
        bool valid = false;
        uint32_t fbp = 0u;
        int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; // inclusive, frame buffer pixels
    };
    // The first enabled display circuit's frame buffer and rectangle (false: no circuit on = blank).
    static bool DisplayRectFromRegs(uint64_t pmode, uint64_t smode2, uint64_t dispfb1, uint64_t display1, uint64_t dispfb2,
                                    uint64_t display2, DisplayRect &out);
    static bool IsDisplayClear(const GSPrimitiveBatch &draw, const DisplayRect &rect);
    static bool HostPresentShownMidClear(); // old path: the image the last Present returned was composed mid-clear
    static uint64_t MidClearComposites();

private:
    enum class Op : uint8_t
    {
        Mxcsr,
        Submit,
        LoadClut,
        BeginTransfer,
        Upload,
        TextureFlush,
        WriteVram,
        Sync,
        Present,
        DrawState,
        Vertices,
        Call,
        RawGif,
        VSync,
        GsReset,
        RingMark,   // bug recorder: field mark (RingMarkPayload)
        RingRecord, // bug recorder: one .gsr record (op byte + payload), nothing to execute
        LatchPresent, // present at the vblank (LatchArgs)
    };

    void Enqueue(Op op, const void *payload, uint32_t size);
    void PublishLocked();
    void TryPublishLocked(); // PublishLocked unless the queue is full (then the chunk goes with the next one)
    void Drain() const;
    void RunOnWorker(const std::function<void()> &function) const;
    void WorkerLoop();
    void Execute(const std::vector<uint8_t> &chunk, GSPrimitiveBatch &draw);
    void RingAtMark(const uint8_t *payload, uint32_t opOffset, const GSPrimitiveBatch &draw);
    void RingCapture(const std::vector<uint8_t> &chunk);
    // Present at the vblank, GS thread.
    void ExecuteLatch(const uint8_t *payload, uint32_t length);
    void PollLatched();
    bool NoteComposite(const GSPresentationRequest &request, const char *path, bool &blank);
    void WatchDisplayDraw(const GSPrimitiveBatch &draw);

    std::unique_ptr<GSRasterBackend> m_inner;
    bool m_rawGif = false; // m_inner->WantsRawGif(), read without going to the worker

    mutable std::mutex m_producerMutex;
    std::vector<uint8_t> m_open;
    uint32_t m_lastMxcsr = UINT32_MAX;
    GSDrawState m_lastDrawState{};
    bool m_drawStateValid = false;
    bool m_compactQueue = true;
    size_t m_queueChunkLimit = 8u;

    mutable std::mutex m_queueMutex;
    mutable std::condition_variable m_workCv;
    mutable std::condition_variable m_doneCv;
    std::deque<std::vector<uint8_t>> m_pending;
    std::vector<std::vector<uint8_t>> m_spare;
    uint64_t m_published = 0u;
    uint64_t m_completed = 0u;
    bool m_stop = false;

    mutable std::mutex m_execMutex;
    std::atomic<bool> m_ringEnabled{false};

    // Present at the vblank + display-clear watch (GS thread only).
    GSGpuBackend *m_gpuInner = nullptr; // m_inner when it is the GPU back end
    DisplayRect m_watchRect{};
    bool m_clearPending = false;
    bool m_latchStarted = false;
    bool m_latchPolling = false; // a present readback is in flight: poll for it
    uint64_t m_hostPresentSeq = 0u;
    std::chrono::steady_clock::time_point m_lastPoll{};
    std::vector<uint8_t> m_latchScratch;

    std::thread m_worker;
};
