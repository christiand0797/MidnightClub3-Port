// Replays (Race Over > Instant Replay, Replay menu Restart / End replay). docs/notes/replays.md has the full map.
//
// MC3 does not store car positions: a replay is the race simulated again from the same random seed with the
// recorded pad input. The record/playback stream (0x20E000 module, header "RPY1") keeps, per game frame, the pad
// bytes, the time scale, and only in measured-time mode the frame time; in fixed step mode (every race) the step is
// not recorded because on the console it was always 1 / (rate * substeps). Every 30 frames the player car's position
// is written while recording and compared bit for bit while playing back (sub_0002DAC0); the first mismatch flags
// the replay (recorder +0x14) and the next frame ends it (sub_00023C80 "EndReplay").
//
// The port's 60 fps timing (hooks.cpp sub_0020B6F0) lets a frame the PC was late with advance two to four 60 Hz
// fields so the race keeps to real time. That makes the step vary from frame to frame, differently while recording
// and while playing back, so the replayed race left the recording at the first position check after the first late
// frame (usually the first one, half a second in: race starts hitch) and the game ended it.
//
// Fix: the step every recorded frame used is kept here, by the stream's own frame number, and forced again on the
// same frame while playing back. Live races keep their real-time compensation; replays are exact.
// The stream buffer (256 KB, sized for 30 steps a second) is also given room for 60 a second.
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

using kernel::G;
using kernel::R32;
using kernel::W32;

namespace {
constexpr uint32_t kStream = 0x534AC4;    // dword: the open record/playback stream (0 = none)
constexpr uint32_t kPlaying = 0x534A9C;   // byte: 1 = playing back, 0 = recording
constexpr uint32_t kRecorded = 0x534AD4;  // dword: frames recorded; playback ends when kPlayed reaches it
constexpr uint32_t kPlayed = 0x534AD8;    // dword: frames played back
constexpr uint32_t kStep = 0x4B2D58;      // float: seconds a fixed step frame advances
constexpr uint32_t kRecDiverged = 0x14;   // recorder byte: the position check failed
constexpr uint32_t kBufferSize = 0x100000;   // console 0x40000 at 30 steps/s; x2 for 60, x2 headroom
constexpr size_t kMaxFrames = 1u << 22;      // 19 hours at 60 Hz

std::vector<uint32_t> g_steps;  // by recorded frame: the kStep bits it ran with, 0 = none
uint32_t g_buffer = 0;
bool g_full_logged = false;

// the stream object is {descriptor, cursor}, the descriptor {base, capacity, used}
bool StreamSpace(uint32_t stream, uint32_t& used, uint32_t& cap) {
    if (!stream || !mem::IsCommitted(stream)) return false;
    uint32_t desc = R32(stream);
    if (!desc || !mem::IsCommitted(desc)) return false;
    cap = R32(desc + 4);
    used = R32(desc + 8);
    return true;
}
}  // namespace

// hooks.cpp sub_0020B6F0 (through g_replay_fixed_step), fixed step mode only, after the 60 fps timing chose kStep and before the original reads
// it. Recording: remember it for this frame. Playing back: force what the recorded frame used (base_bits if the
// recording has nothing for it). Returns true when the step was forced, so the caller drops its real-time carry.
static bool ReplayFixedStep(uint32_t base_bits) {
    const uint32_t stream = R32(kStream);
    if (!stream) return false;
    if (!G<uint8_t>(kPlaying)) {
        const uint32_t frame = R32(kRecorded);
        if (frame < kMaxFrames) {
            if (frame >= g_steps.size()) g_steps.resize(size_t(frame) + 1, 0);
            g_steps[frame] = R32(kStep);  // a frame seen twice (a pause) keeps the step it completed with
        }
        uint32_t used, cap;
        if (!g_full_logged && StreamSpace(stream, used, cap) && used >= cap) {
            g_full_logged = true;
            LOGR("replay: the recording buffer is full after %u frames (%u KB); the replay will end there", frame,
                 cap >> 10);
        }
        return false;
    }
    const uint32_t frame = R32(kPlayed);
    const uint32_t bits = frame < g_steps.size() && g_steps[frame] ? g_steps[frame] : base_bits;
    if (bits) W32(kStep, bits);
    return bits != 0;
}

extern bool (*g_replay_fixed_step)(uint32_t base_bits);  // hooks.cpp
namespace {
struct InstallReplayStep {
    InstallReplayStep() { g_replay_fixed_step = ReplayFixedStep; }
} g_install_replay_step;
}  // namespace

// open the stream for recording (cdecl, stream, al = opened): a new race, a new step record
X86_EXTERN_FUNC(__imp__sub_0020EA20);
X86_FUNC(sub_0020EA20) {
    __imp__sub_0020EA20(ctx, base);
    if ((ctx.eax & 0xFF) && R32(kStream)) {
        g_steps.clear();
        g_full_logged = false;
        uint32_t used = 0, cap = 0;
        StreamSpace(R32(kStream), used, cap);
        LOGR("replay: recording (%u KB buffer)", cap >> 10);
    }
}

// open the stream for playback (cdecl, stream, al = opened): Instant Replay and Restart Replay
X86_EXTERN_FUNC(__imp__sub_0020EAC0);
X86_FUNC(sub_0020EAC0) {
    __imp__sub_0020EAC0(ctx, base);
    if (!(ctx.eax & 0xFF) || !R32(kStream)) {
        LOGR("replay: the recording could not be opened for playback");
        return;
    }
    const uint32_t frames = R32(kRecorded);
    // late frames: steps longer than the shortest one (the usual single field)
    uint32_t known = 0, late = 0;
    float shortest = 0;
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < g_steps.size() && i < frames; i++) {
            if (!g_steps[i]) continue;
            float f;
            std::memcpy(&f, &g_steps[i], 4);
            if (pass == 0) {
                known++;
                if (shortest == 0 || f < shortest) shortest = f;
            } else if (f > shortest * 1.01f) {
                late++;
            }
        }
    uint32_t used = 0, cap = 0;
    StreamSpace(R32(kStream), used, cap);
    LOGR("replay: playing %u frames (%u of %u KB), steps known for %u, %u of them late frames (longer steps)",
         frames, used >> 10, cap >> 10, known, late);
}

// end of a record/playback frame with the position check every 30 frames (thiscall, recorder, al)
X86_EXTERN_FUNC(__imp__sub_0002DAC0);
X86_FUNC(sub_0002DAC0) {
    const uint32_t rec = ctx.ecx;
    const bool playing = R32(kStream) && G<uint8_t>(kPlaying);
    const bool was = G<uint8_t>(rec + kRecDiverged) != 0;
    __imp__sub_0002DAC0(ctx, base);
    if (!playing) return;
    if (!was && G<uint8_t>(rec + kRecDiverged))
        LOGR("replay: playback left the recording at frame %u of %u (position check), the game ends the replay",
             R32(kPlayed), R32(kRecorded));
    else if (!R32(kStream))
        LOGR("replay: played to the end (%u frames)", R32(kPlayed));
}

// the replay buffer (thiscall, int* size, returns the buffer; only the recorder's constructor 0x2D870 asks): the
// stock 256 KB held about as many seconds at 30 steps a second as half that at 60, so long races were cut short.
X86_EXTERN_FUNC(__imp__sub_0001DA90);
X86_FUNC(sub_0001DA90) {
    const uint32_t out = R32(ctx.esp + 4);
    if (!g_buffer) g_buffer = mem::AllocSystem(kBufferSize, 0x80);
    if (!g_buffer) {
        __imp__sub_0001DA90(ctx, base);
        return;
    }
    W32(out, kBufferSize);
    ctx.eax = g_buffer;
    ctx.esp += 8;  // ret 4
}
