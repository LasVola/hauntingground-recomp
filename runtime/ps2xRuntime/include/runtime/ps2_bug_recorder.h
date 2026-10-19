#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Instant-replay bug recorder (PS2X_BUG_RECORDER=1; the Haunting Ground port turns it on, HG_BUG_RECORDER=0 = off).
// While playing it only keeps RAM: the last ~3 s of presented frames as 320-pixel-wide thumbnails (made on the CPU from
// the frame the host already has for its texture upload: no GPU readback of its own) and the last ~1-2 s of GS
// commands (GSRing, runtime/gs/gs_recording.h). requestBundle (F8, the PS2X_BUGSHOT_TEST_FIELD test hook, or a black
// flash it detects itself) writes logs/bugshots/<date-time>_<field>/ on a writer thread: frame_<field>.png per frame,
// contact.png (the last 48 frames with field numbers), frames.csv, gs.gsr (the GS ring; replay with hg_replay play
// gs.gsr <out> --fields), info.txt, and one "[bugshot] ..." log line. One bundle at a time: a request while one is
// still being written is skipped (logged).
namespace ps2x::bugrec
{
    bool enabled();
    // Run loop start (main thread): one start-up line.
    void start();
    // Main thread, once per new field: the RGBA frame (width x height) the host is about to show for that field.
    // displayFbp: the frame buffer it came from (frames.csv only).
    void onPresentedFrame(const uint8_t *rgba, uint32_t width, uint32_t height, uint64_t field, uint32_t displayFbp = 0u);
    // Main thread, once per new field without an image (no display circuit on, no frame yet): the host shows black.
    void onBlankFrame(uint64_t field);
    // Main thread. reason: "F8", "test hook", ... note: an extra line for info.txt. marked: fields framed in red on the
    // contact sheet. keepGsSegments > 0: only the newest that many 15-field GS segments go into gs.gsr. False when
    // skipped (off, or a bundle is still being written).
    bool requestBundle(const std::string &reason, uint64_t field, const std::string &note,
                       const std::vector<uint64_t> &marked = {}, size_t keepGsSegments = 0u);
    // PS2X_BUGSHOT_TEST_FIELD=<n>: the field at which the run loop fires the F8 path once (0 = none).
    uint64_t testField();
    // Host overlay (main thread): a short note after F8 ("bug snapshot ..."), like the save-state message.
    bool overlayMessage(std::string &text, float &alpha);
}
