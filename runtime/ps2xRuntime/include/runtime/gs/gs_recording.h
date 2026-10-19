#pragma once

#include "runtime/gs/gs_backend.h"
#include "runtime/gs/gs_backend_state.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

struct GSRegisters;

namespace GSRecording
{
    enum class Op : uint8_t
    {
        State = 1,
        Submit = 2,
        LoadClut = 3,
        BeginTransfer = 4,
        Upload = 5,
        TextureFlush = 6,
        Clear = 7,
        WriteVram = 8,
        Consume = 9,
        Field = 10,
        ReadVram = 11,
        Reset = 12,
        End = 13,
        Hash = 14,
        Mxcsr = 15,
    };

    struct Record
    {
        Op op = Op::End;
        std::vector<uint8_t> payload;
    };

    struct Checkpoint
    {
        uint64_t tick = 0u;
        std::array<uint64_t, 7> display{};
        std::vector<uint8_t> vram;
        GSBackendState state;
    };

    void AppendState(std::vector<uint8_t> &out, const GSBackendState &state);
    bool ParseState(const uint8_t *data, size_t size, size_t &offset, GSBackendState &out);
    bool ParseCheckpoint(const Record &record, Checkpoint &out);

    class Reader
    {
    public:
        bool Open(const std::string &path);
        bool Next(Record &out);
        const std::string &Error() const { return m_error; }

    private:
        std::unique_ptr<std::FILE, int (*)(std::FILE *)> m_file{nullptr, &std::fclose};
        std::string m_error;
    };

    struct ApplyResult
    {
        bool ok = true;
        std::string message;
    };

    ApplyResult Apply(GSRasterBackend &backend, const Record &record);

    uint64_t HashVram(const uint8_t *vram, size_t size);
}

class GSRecordingBackend final : public GSRasterBackend, public GSBackendStateAccess
{
public:
    GSRecordingBackend(std::unique_ptr<GSRasterBackend> inner,
                       const GSRegisters *regs,
                       uint64_t firstTick,
                       uint64_t tickCount,
                       const std::string &path);
    ~GSRecordingBackend() override;

    static std::unique_ptr<GSRasterBackend> WrapFromEnvironment(std::unique_ptr<GSRasterBackend> inner,
                                                                const GSRegisters *regs);

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

    bool ExportState(GSBackendState &out) override;
    bool ImportState(const GSBackendState &state) override;

private:
    bool Recording();
    void WriteRecord(GSRecording::Op op, const void *payload, size_t size);
    void WriteRecord(GSRecording::Op op, const std::vector<uint8_t> &payload);
    void WriteCheckpoint(GSRecording::Op op, uint64_t tick);
    void WriteHash();

    std::unique_ptr<GSRasterBackend> m_inner;
    const GSRegisters *m_regs = nullptr;
    uint64_t m_firstTick = 0u;
    uint64_t m_endTick = 0u;
    uint64_t m_lastTick = UINT64_MAX;
    std::FILE *m_file = nullptr;
    bool m_started = false;
    bool m_finished = false;
    bool m_hashEveryCommand = false;
    uint64_t m_commandIndex = 0u;
    uint32_t m_lastMxcsr = UINT32_MAX;
    std::vector<uint64_t> m_extraCheckpoints;
    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0u;
    std::vector<uint8_t> m_scratch;
};

class GSThreadedBackend;

// Bug recorder GS ring (PS2X_BUG_RECORDER=1; ps2_bug_recorder.cpp). The GS thread keeps the command chunks it has
// executed (GSThreadedBackend's own queue format, no extra copy on the game thread) for the last ~1 s, in segments of 15
// fields that each start at a State checkpoint taken on the GS thread in stream order without a stall (an asynchronous
// copy on the GPU back end, GSGpuBackend::BeginSnapshotAsync). The oldest segment goes once the newer ones hold >= 60
// fields or the RAM budget (PS2X_BUG_RECORDER_GS_MB, default 256) is exceeded. GSRing::Write turns a collected ring into
// a normal .gsr (PS2X_GS_RECORD format). Implemented in gs_threaded_backend.cpp.
namespace GSRing
{
    struct Checkpoint;
    struct Collected;

    bool Active();
    // Hands the ring to a writer: asks the GS thread to end the stream at the next field mark (a new segment starts
    // there; its checkpoint becomes the End record) and give everything before it. Waits up to waitMs for that (with no
    // GS activity it takes the ring as it is, without an End checkpoint), then up to waitMs for the checkpoints.
    // keepSegments > 0: only the newest that many segments (of 15 fields) are kept (a shorter .gsr).
    std::shared_ptr<Collected> Collect(int waitMs, size_t keepSegments = 0u);
    // Writes the .gsr: State, the records, a Field record without VRAM per field, End. summary: fields, ticks, size.
    bool Write(const Collected &collected, const std::string &path, std::string &summary);
    // One line: fields and MB in RAM, checkpoints taken / failed, checkpoint delay.
    std::string Stats();
}

// The game-thread side of the ring: forwards everything to the GS thread back end and only adds a field mark to its
// queue when the vsync tick changes (one atomic load per command), plus the two VRAM-changing calls that the queue runs
// synchronously and so does not carry (Reset, ClearFramebuffer).
class GSRingRecorderBackend final : public GSRasterBackend, public GSBackendStateAccess
{
public:
    GSRingRecorderBackend(std::unique_ptr<GSRasterBackend> inner, GSThreadedBackend *threaded, const GSRegisters *regs);
    ~GSRingRecorderBackend() override;

    // PS2X_BUG_RECORDER=1 wraps inner when it is the GS thread back end (not with PS2X_GS_RECORD, not with a raw-GIF
    // back end).
    static std::unique_ptr<GSRasterBackend> WrapFromEnvironment(std::unique_ptr<GSRasterBackend> inner,
                                                                const GSRegisters *regs);

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

    bool ExportState(GSBackendState &out) override;
    bool ImportState(const GSBackendState &state) override;

private:
    void Mark();

    std::unique_ptr<GSRasterBackend> m_inner;
    GSThreadedBackend *m_threaded = nullptr;
    const GSRegisters *m_regs = nullptr;
    uint64_t m_lastTick = UINT64_MAX;
    bool m_restart = true;
};
