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
// same frame while playing back. Live races keep their real-time compensation. That keeps a replay in step for
// hundreds of frames, but not bit for bit forever: the world streams in on other threads, so two runs can still
// round differently later on. A replay therefore plays on past a failed position check instead of ending (the
// stream stays in step; the drift starts at float rounding). The stream buffer (256 KB, sized for 30 steps a
// second) is also given room for 60 a second.
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "kernel/kernel.h"
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
constexpr uint32_t kFrameTime = 0x4B2D70; // float: seconds the last update settled on, read by everything after it
constexpr uint32_t kRecDiverged = 0x14;   // recorder byte: the position check failed
constexpr uint32_t kBufferSize = 0x100000;   // console 0x40000 at 30 steps/s; x2 for 60, x2 headroom
constexpr size_t kMaxFrames = 1u << 22;      // 19 hours at 60 Hz

std::vector<uint32_t> g_steps;  // by recorded frame: the kStep bits it ran with, 0 = none
// Diagnosis: the player car position (float bits) at the end of every recorded frame, compared on every played frame,
// so the first frame the re-simulation leaves the recording is known, not just the 30-frame check that caught it.
std::vector<std::array<uint32_t, 3>> g_positions;
bool g_trace_logged = false;
bool g_drift_logged = false;
// The player car transform at the end of every recorded frame (48 or 64 bytes, see TransformSpan). Playback writes it
// back at the same point: the re-simulation's float drift (timing-dependent world streaming) never shows, and the
// game's own position check then passes.
struct Transform {
    uint32_t size = 0;
    std::array<uint32_t, 16> words{};
};
std::vector<Transform> g_transforms;
bool g_snap_logged = false;

// the position the check at 0x2DAC0 compares: player index (0x2694D0) -> object (0x2BF570) -> +0x14 -> +8 -> +0x68
// -> +0x10 -> translation at +0x80
// the player car transform holding that position: a Matrix34 (three rows of three floats, translation at +0x80) or a
// 4x4 with 16-byte rows; told apart by unit-length rotation rows. Returns the first byte and the size, 0 if neither.
uint32_t TransformSpan(uint32_t p, uint32_t& start) {
    const auto unit = [](uint32_t a) {
        float v[3];
        for (int i = 0; i < 3; i++) {
            const uint32_t b = R32(a + 4 * i);
            std::memcpy(&v[i], &b, 4);
        }
        const float l = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        return l > 0.998f && l < 1.002f;
    };
    if (!mem::IsCommitted(p + 0x50) || !mem::IsCommitted(p + 0x8F)) return 0;
    if (unit(p + 0x5C) && unit(p + 0x68) && unit(p + 0x74)) return start = p + 0x5C, 48;
    if (unit(p + 0x50) && unit(p + 0x60) && unit(p + 0x70)) return start = p + 0x50, 64;
    return 0;
}

bool PlayerPosition(X86Context& ctx, std::array<uint32_t, 3>& out, uint32_t* matrix = nullptr) {
    const X86Context saved = ctx;
    const uint32_t index = kernel::CallGuest(ctx, 0x2694D0, {});
    const uint32_t object = kernel::CallGuest(ctx, 0x2BF570, {index});
    ctx = saved;
    uint32_t p = object;
    for (uint32_t offset : {0x14u, 0x8u, 0x68u, 0x10u}) {
        if (!p || !mem::IsCommitted(p + offset)) return false;
        p = R32(p + offset);
    }
    if (!p || !mem::IsCommitted(p + 0x88)) return false;
    for (int i = 0; i < 3; i++) out[i] = R32(p + 0x80 + 4 * i);
    if (matrix) *matrix = p;
    return true;
}

// A stream opening is the start of the simulated race. Whatever runs from here before the first stream frame reads the
// last frame's step and frame time, which the port's 60 fps timing varies (a load or menu hitch). Start both from the
// console's one 60 Hz field so recording and playback begin alike.
void CanonicalStart(const char* what) {
    float before = 0;
    const uint32_t bits = R32(kFrameTime);
    std::memcpy(&before, &bits, 4);
    const float field = 1.0f / 60.0f;
    uint32_t field_bits;
    std::memcpy(&field_bits, &field, 4);
    LOGR("replay: %s opened with frame time %.6f, starting from %.6f", what, before, field);
    if (R32(kStep)) W32(kStep, field_bits);
    W32(kFrameTime, field_bits);
}

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
        g_positions.clear();
        g_transforms.clear();
        CanonicalStart("recording");
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
    CanonicalStart("playback");
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
    g_trace_logged = false;
    g_drift_logged = false;
    g_snap_logged = false;
}

// end of a record/playback frame with the position check every 30 frames (thiscall, recorder, al)
X86_EXTERN_FUNC(__imp__sub_0002DAC0);
X86_FUNC(sub_0002DAC0) {
    const uint32_t rec = ctx.ecx;
    const bool playing = R32(kStream) && G<uint8_t>(kPlaying);
    const bool was = G<uint8_t>(rec + kRecDiverged) != 0;
    if (R32(kStream)) {
        std::array<uint32_t, 3> pos{};
        uint32_t matrix = 0, start = 0;
        const uint32_t frame = playing ? R32(kPlayed) : R32(kRecorded);
        if (frame < kMaxFrames && PlayerPosition(ctx, pos, &matrix)) {
            const uint32_t size = TransformSpan(matrix, start);
            if (!playing) {
                if (frame >= g_positions.size()) g_positions.resize(size_t(frame) + 1, {~0u, ~0u, ~0u});
                g_positions[frame] = pos;
                if (frame >= g_transforms.size()) g_transforms.resize(size_t(frame) + 1);
                Transform& t = g_transforms[frame];
                t.size = size;
                for (uint32_t i = 0; i < size / 4; i++) t.words[i] = R32(start + 4 * i);
            } else if (!g_trace_logged && frame < g_positions.size() && g_positions[frame][0] != ~0u &&
                       g_positions[frame] != pos) {
                g_trace_logged = true;
                float r[3], q[3], rs = 0, qs = 0;
                std::memcpy(r, g_positions[frame].data(), 12);
                std::memcpy(q, pos.data(), 12);
                const uint32_t recorded_step = frame < g_steps.size() ? g_steps[frame] : 0, played_step = R32(kStep);
                std::memcpy(&rs, &recorded_step, 4);
                std::memcpy(&qs, &played_step, 4);
                LOGR("replay: frame %u first differs from the recording: car (%.6f %.6f %.6f) recorded (%.6f %.6f %.6f), "
                     "step %.6f recorded %.6f", frame, q[0], q[1], q[2], r[0], r[1], r[2], qs, rs);
            }
            if (playing && frame < g_transforms.size() && g_transforms[frame].size && g_transforms[frame].size == size) {
                const Transform& t = g_transforms[frame];
                for (uint32_t i = 0; i < t.size / 4; i++) W32(start + 4 * i, t.words[i]);
                if (!g_snap_logged) {
                    g_snap_logged = true;
                    LOGR("replay: keeping the player car on its recorded path (%s transform)",
                         t.size == 48 ? "3x4" : "4x4");
                }
            }
        }
    }
    __imp__sub_0002DAC0(ctx, base);
    if (!playing) return;
    if (!was && G<uint8_t>(rec + kRecDiverged)) {
        // The re-simulation still drifts by float rounding after a while (the world streams in on other threads,
        // so two runs need not read identical data on the same frame). The stream stays in step either way, so the
        // replay plays on instead of ending at the first check that notices; the drift is a few cm at most early on.
        G<uint8_t>(rec + kRecDiverged) = 0;
        if (!g_drift_logged) {
            g_drift_logged = true;
            LOGR("replay: playback drifted from the recording at frame %u of %u (position check); playing on",
                 R32(kPlayed), R32(kRecorded));
        }
    } else if (!R32(kStream))
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
