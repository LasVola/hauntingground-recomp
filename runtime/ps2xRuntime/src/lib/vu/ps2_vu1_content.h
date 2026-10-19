#pragma once
// Content-keyed VU1 AOT (Haunting Ground 2026-10-18; design codex/vu1keyed/DESIGN.md). Opt-in: PS2X_VU1_CONTENT_AOT=1|2
// (Haunting Ground's src/main.cpp sets 2 as its default; God Hand never does, so its lookup is unchanged).
// A content program is generated code for one set of verified VU1 instruction pairs (reachable from its root PCs up to
// the E-bit stops), selected only when every one of those bytes is resident in VU1 micro memory, whatever the stale
// bytes around them. Its code uses the plain runImage/fastBlock templates under the content key (separate file prefix
// vu1_content_); these declarations live here and not in ps2_vu1.h / ps2_vu1_impl.inl, which every generated file
// includes (a change there rebuilds all of them).
#include "ps2_vu1_impl.inl"

#include <cstddef>
#include <cstdint>

// Content descriptors own no storage. Generated arrays must have static lifetime.
// Spans contain complete, sorted, non-overlapping instruction pairs only.
struct Vu1ContentSpan
{
    uint32_t offset;
    uint32_t size;
    const uint8_t *bytes;
};

struct Vu1ContentProgram
{
    uint64_t key; // offline identity, never accepted as byte-equality evidence
    const Vu1ContentSpan *spans;
    uint32_t spanCount;
    VU1InterpreterAccess::CompiledProgram program;
};

// PS2X_VU1_CONTENT_AOT (read once, at the first VU1 lookup or VIF1 MPG); unset or anything else = 0:
//   0       off: whole-image registry only (the runtime default; God Hand).
//   1/first content first: a content unit, else the interpreter on content-known code, else whole-image code.
//   2/fallback whole-image code first (the old fast path, no content work on a hit); on a whole-image miss the content
//           unit; the interpreter last (Haunting Ground default). A whole-image miss saves the image (logs/vu1_missing)
//           with one [vu1-aot] line saying whether content units or the interpreter ran it.
// Enabled = mode 1 or 2 (also the VIF1 MPG compare-skip: an unchanged upload keeps the code generation; valid for the
// whole-image cache too, which only depends on the bytes).
int ps2xVu1ContentAotMode() noexcept;
bool ps2xVu1ContentAotEnabled() noexcept;

void registerVu1ContentProgram(const Vu1ContentProgram &program);

// Offline tools (port_prep/vu1/tools/vu1_replay.cpp): the exact lookup a VU1 run starts with (in the order of the
// mode above), and the content descriptor behind a returned program (nullptr for
// whole-image code). Not used by the game.
VU1InterpreterAccess::CompiledProgram ps2xVu1LookupCompiledProgram(const uint8_t *vuCode, uint32_t codeSize,
                                                                   const PS2Memory *memory, uint32_t pc);
const Vu1ContentProgram *ps2xVu1FindContentProgram(VU1InterpreterAccess::CompiledProgram program);
size_t ps2xVu1ContentProgramCount();
const Vu1ContentProgram *ps2xVu1ContentProgramAt(size_t index); // registration order; nullptr past the end

// Offline replay counters (port_prep/vu1/tools/hg_vu1_replay.cpp): while g_vu1RunStatsHook is set, runLoopWith counts
// its compiled-code calls by result and the pairs it steps in the interpreter. Always nullptr in the game.
struct Vu1RunStatsHook
{
    uint64_t compiledCalls = 0;
    uint64_t compiledFallbacks = 0; // the next pair ran in the interpreter (uncovered pair or unsafe entry state)
    uint64_t compiledBudget = 0;
    uint64_t compiledEnded = 0;
    uint64_t interpretedPairs = 0;
    int firstResult = -1; // StepResult of the first compiled call since the hook was set (-1: none)
};
extern Vu1RunStatsHook *g_vu1RunStatsHook;
