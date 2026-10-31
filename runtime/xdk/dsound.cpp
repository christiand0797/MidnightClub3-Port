// DirectSound (XDK 4831 and 5849) on a host software mixer. Buffers and streams are read from guest memory as they play.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <vector>

#include "audio/audio.h"
#include "audio/modulation.h"
#include "audio/stream_status.h"
#include "audio/voice3d.h"
#include "xbe/xbe.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

namespace kernel {
extern const xbe::Image* g_image;
}
namespace dispatch {
X86Func* Find(uint32_t guest);
}

// configs written before the SDK knew the name
#ifndef SYM_IDirectSound_SynchPlayback
#define SYM_IDirectSound_SynchPlayback FFFFFF00
#define HAS_IDirectSound_SynchPlayback 0
#endif
#ifndef SYM_IDirectSound_DownloadEffectsImage
#define SYM_IDirectSound_DownloadEffectsImage FFFFFF01
#define HAS_IDirectSound_DownloadEffectsImage 0
#endif
#ifndef SYM_IDirectSound_GetEffectData
#define SYM_IDirectSound_GetEffectData FFFFFF02
#define HAS_IDirectSound_GetEffectData 0
#endif
#ifndef SYM_IDirectSound_CommitEffectData
#define SYM_IDirectSound_CommitEffectData FFFFFF03
#define HAS_IDirectSound_CommitEffectData 0
#endif
#ifndef SYM_IDirectSound_GetOutputLevels
#define SYM_IDirectSound_GetOutputLevels FFFFFF04
#define HAS_IDirectSound_GetOutputLevels 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetPlayRegion
#define SYM_IDirectSoundBuffer_SetPlayRegion FFFFFF05
#define HAS_IDirectSoundBuffer_SetPlayRegion 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetAllParameters
#define SYM_IDirectSoundBuffer_SetAllParameters FFFFFF06
#define HAS_IDirectSoundBuffer_SetAllParameters 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetFormat
#define SYM_IDirectSoundBuffer_SetFormat FFFFFF07
#define HAS_IDirectSoundBuffer_SetFormat 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetOutputBuffer
#define SYM_IDirectSoundBuffer_SetOutputBuffer FFFFFF08
#define HAS_IDirectSoundBuffer_SetOutputBuffer 0
#endif
#ifndef SYM_IDirectSoundBuffer_SetRolloffCurve
#define SYM_IDirectSoundBuffer_SetRolloffCurve FFFFFF09
#define HAS_IDirectSoundBuffer_SetRolloffCurve 0
#endif
#ifndef SYM_IDirectSoundBuffer_StopEx
#define SYM_IDirectSoundBuffer_StopEx FFFFFF0A
#define HAS_IDirectSoundBuffer_StopEx 0
#endif
#ifndef SYM_DirectSoundUseLightHRTF
#define SYM_DirectSoundUseLightHRTF FFFFFF0B
#define HAS_DirectSoundUseLightHRTF 0
#endif
#ifndef SYM_IDirectSoundStream_SetEG
#define SYM_IDirectSoundStream_SetEG FFFFFF0C
#define HAS_IDirectSoundStream_SetEG 0
#endif
#ifndef SYM_IDirectSoundBuffer_GetVoiceProperties
#define SYM_IDirectSoundBuffer_GetVoiceProperties FFFFFF0D
#endif
#ifndef SYM_IDirectSoundBuffer_Set3DVoiceData
#define SYM_IDirectSoundBuffer_Set3DVoiceData FFFFFF0E
#endif
#ifndef SYM_IDirectSoundBuffer_Use3DVoiceData
#define SYM_IDirectSoundBuffer_Use3DVoiceData FFFFFF0F
#endif
#ifndef SYM_CDirectSoundStream_GetVoiceProperties
#define SYM_CDirectSoundStream_GetVoiceProperties FFFFFF10
#endif
#ifndef SYM_CDirectSoundStream_Set3DVoiceData
#define SYM_CDirectSoundStream_Set3DVoiceData FFFFFF11
#endif
#ifndef SYM_CDirectSoundStream_Use3DVoiceData
#define SYM_CDirectSoundStream_Use3DVoiceData FFFFFF12
#endif
#ifndef SYM_IDirectSoundBuffer_SetPitch
#define SYM_IDirectSoundBuffer_SetPitch FFFFFF13
#endif
#ifndef SYM_IDirectSoundBuffer_SetLFO
#define SYM_IDirectSoundBuffer_SetLFO FFFFFF14
#endif
#ifndef SYM_IDirectSoundBuffer_SetEG
#define SYM_IDirectSoundBuffer_SetEG FFFFFF15
#endif
#ifndef SYM_CDirectSoundStream_SetPitch
#define SYM_CDirectSoundStream_SetPitch FFFFFF16
#endif
#ifndef SYM_CDirectSoundStream_SetLFO
#define SYM_CDirectSoundStream_SetLFO FFFFFF17
#endif
#ifndef SYM_CDirectSoundStream_SetOutputBuffer
#define SYM_CDirectSoundStream_SetOutputBuffer FFFFFF18
#endif
#ifndef SYM_IDirectSound_GetCaps
#define SYM_IDirectSound_GetCaps FFFFFF19
#endif
#ifndef SYM_IDirectSound_CreateSoundStream
#define SYM_IDirectSound_CreateSoundStream FFFFFF1A
#endif

namespace {

constexpr uint32_t kStreamVtable = SYM(DSound_StreamVtable);  // CDirectSoundStream XMediaObject vtable
constexpr uint32_t kStreamVtable2 = SYM(DSound_StreamVtable2);
constexpr uint32_t kPacketPending = 0x8000000A, kFlushed = 0x80004004;  // E_PENDING, E_ABORT

struct Packet {
    uint32_t data, size, completed_ptr, status_ptr, context;
    uint32_t used = 0;
};

struct Voice {
    uint32_t obj = 0;
    bool stream = false;
    int refs = 1;
    // format
    uint16_t tag = 1, channels = 1, bits = 16, align = 2;
    uint32_t rate = 48000;
    // buffer
    // play region in bytes from data, loop region in bytes from the play region
    uint32_t data = 0, bytes = 0, loop_start = 0, loop_len = 0, play_start = 0, play_len = 0;
    uint32_t owned = 0;  // memory the library made for the buffer
    bool playing = false, looping = false, paused = false;
    bool sync = false;  // held until IDirectSound_SynchPlayback
    bool active = false, ending = false;  // stream: took a packet since it went quiet, quiet again once they run out
    bool hold = false;  // stream: DSSTREAMPAUSE_PAUSENOACTIVATE, packets queue without the voice starting
    bool stopping = false;  // stream: FlushEx released its envelope, it ends with the release or its last packet
    int64_t starved_at = -1;  // stream: steady ms its packets ran out with no Discontinuity, -1 while fed
    double pos = 0;  // frames
    uint32_t freq = 0;
    int32_t pitch = 0;
    bool pitch_override = false;
    // stream
    uint32_t max_packets = 1, callback = 0, context = 0;
    std::deque<Packet> packets;
    // levels
    float volume = 1, headroom = 1;
    uint8_t nbins = 0, bin[8] = {};
    int32_t bin_mb[32] = {};  // by mix bin
    std::vector<float> curve;  // rolloff
    bool is3d = false;
    audio::Voice3DData voice3d;
    uint32_t mode = 0;  // DS3DMODE: 1 head relative, 2 disabled
    float p[3] = {}, min_dist = 1, max_dist = 1e9f;
    // SetEG / SetLFO, restarted at voice on: a buffer's Play while stopped, a stream's first packet since it went quiet
    audio::Modulation mod;
    // ADPCM block cache
    int64_t block = -1;
    int16_t decoded[65 * 8];
};

struct Done {
    uint32_t voice, callback, context, packet_context, status;
};

std::recursive_mutex g_lock;
std::map<uint32_t, Voice> g_voices;
float g_listener[3] = {}, g_front[3] = {0, 0, 1}, g_top[3] = {0, 1, 0};
// Completed packets' callbacks, run as the XDK runs them: in DirectSoundDoWork on the title's thread, so they never
// overlap the title's own stream code. A stream flushed or released runs its own at once, before it is gone.
std::deque<Done> g_done;
std::condition_variable_any g_done_cv;
thread_local std::vector<Done> t_flushed;  // run by the STREAM call that flushed them, once g_lock is let go
std::atomic<int64_t> g_pumped{0};          // the last DirectSoundDoWork, steady ms
bool g_started = false;
uint32_t g_ds = 0;

float Db(int32_t hundredths) { return hundredths <= -10000 ? 0.0f : std::pow(10.0f, hundredths / 2000.0f); }

uint32_t FramesOf(const Voice& v, uint32_t bytes) {
    if (v.tag == 0x69) return bytes / (36u * v.channels) * 64;  // WAVE_FORMAT_XBOX_ADPCM
    return bytes / std::max<uint32_t>(v.align, 1);
}
uint32_t BytesOf(const Voice& v, uint32_t frames) {
    if (v.tag == 0x69) return frames / 64 * 36 * v.channels;
    return frames * v.align;
}

const int kIndex[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};
const int kStep[89] = {7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,    25,    28,
                       31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
                       130,   143,   157,   173,   190,   209,   230,   253,   279,   307,   337,   371,   408,   449,   494,
                       544,   598,   658,   724,   796,   876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
                       2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,
                       9493,  10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

// one 36 byte per channel IMA block, the voice plays 64 of its 65 frames
void DecodeBlock(const uint8_t* b, int ch, int16_t* out) {
    for (int c = 0; c < ch; c++) {
        int pred = int16_t(b[c * 4] | b[c * 4 + 1] << 8), idx = std::min<int>(b[c * 4 + 2], 88);
        out[c] = int16_t(pred);
        const uint8_t* d = b + ch * 4;
        for (int k = 0; k < 8; k++)
            for (int j = 0; j < 8; j++) {
                int n = (d[(k * ch + c) * 4 + j / 2] >> ((j & 1) * 4)) & 15;
                int step = kStep[idx], diff = step >> 3;
                if (n & 1) diff += step >> 2;
                if (n & 2) diff += step >> 1;
                if (n & 4) diff += step;
                pred = std::clamp(n & 8 ? pred - diff : pred + diff, -32768, 32767);
                idx = std::clamp(idx + kIndex[n], 0, 88);
                out[(1 + k * 8 + j) * ch + c] = int16_t(pred);
            }
    }
}

// sample of channel c at frame f of memory starting at data
float Sample(Voice& v, uint32_t data, uint32_t f, int c) {
    c = std::min<int>(c, v.channels - 1);
    const uint8_t* p = static_cast<const uint8_t*>(HostPtr(data));
    if (v.tag == 0x69) {
        int64_t at = int64_t(data) + int64_t(f / 64) * 36 * v.channels;
        if (at != v.block) {
            DecodeBlock(static_cast<const uint8_t*>(HostPtr(uint32_t(at))), std::min<int>(v.channels, 8), v.decoded);
            v.block = at;
        }
        return v.decoded[(f % 64) * v.channels + c] / 32768.0f;
    }
    if (v.bits == 8) return (p[f * v.align + c] - 128) / 128.0f;
    int16_t s;
    std::memcpy(&s, p + f * v.align + c * 2, 2);
    return s / 32768.0f;
}

void Complete(Voice& v, Packet& pk, uint32_t status) {
    if (pk.completed_ptr) W32(pk.completed_ptr, status == 0 ? pk.size : pk.used);
    if (pk.status_ptr) W32(pk.status_ptr, status);
    if (v.callback) {
        g_done.push_back({v.obj, v.callback, v.context, pk.context, status});
        g_done_cv.notify_one();
    } else if (pk.context) {
        if (ObjectRef e = FromHandle(pk.context)) SetEvent(*e);
    }
}

// the console's stereo downmix: centre, back pair and back crosstalk at -3 dB, the sub dropped
float StereoFold(uint32_t bin) {
    bool folded = bin == 2 || bin == 4 || bin == 5 || bin == 8 || bin == 9;
    return folded ? 0.707f : 1.0f;
}

// gain of voice channel c on host left and right. A hardware voice holds two channels and owns a run of slots,
// which alternate left and right.
void Levels(Voice& v, float g[2][2]) {
    static const uint8_t kFront[2] = {0, 1};
    const uint8_t* bin = v.nbins ? v.bin : kFront;
    uint32_t n = v.nbins ? v.nbins : 2, voices = (std::max<uint32_t>(v.channels, 1) - 1) / 2 + 1;
    uint32_t per = std::max<uint32_t>(n / voices, 2), step = v.channels == 1 ? 1 : 2;
    bool speakers = false;
    for (uint32_t s = 0; s < n; s++) speakers |= bin[s] < 10 && bin[s] != 3;
    for (uint32_t c = 0; c < 2; c++) {
        g[c][0] = g[c][1] = 0;
        for (uint32_t s = c; c < v.channels && s < per && s < n; s += step) {
            uint32_t b = bin[s];
            if (speakers && (b == 3 || b > 9)) continue;  // sub and effect sends
            float x = Db(v.bin_mb[b]) * v.volume * v.headroom * StereoFold(b) * v.voice3d.Gain(b);
            // even bins are left, odd right, 2 is the centre
            bool left = !speakers || b == 2 || !(b & 1);
            bool right = !speakers || b == 2 || (b & 1);
            // slots that reach one speaker add, as on the DSP; effect sends alone play the loudest
            if (speakers) {
                if (left) g[c][0] += x;
                if (right) g[c][1] += x;
            } else {
                g[c][0] = std::max(g[c][0], x);
                g[c][1] = std::max(g[c][1], x);
            }
        }
    }
    if (v.voice3d.enabled) return;
    if (!v.is3d || v.mode == 2) return;
    bool rel = v.mode == 1;  // the position is already in listener space
    float d[3] = {v.p[0], v.p[1], v.p[2]};
    for (int i = 0; i < 3 && !rel; i++) d[i] -= g_listener[i];
    float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float att = 1;
    if (dist > v.min_dist && v.curve.empty()) {
        att = v.min_dist / std::min(dist, v.max_dist);
    } else if (dist > v.min_dist && v.max_dist > v.min_dist) {
        // points spread evenly from min to max distance, 1.0 at min
        float at = (std::min(dist, v.max_dist) - v.min_dist) / (v.max_dist - v.min_dist) * v.curve.size();
        size_t i = std::min(size_t(at), v.curve.size() - 1);
        float from = i ? v.curve[i - 1] : 1.0f;
        att = from + (v.curve[i] - from) * (at - i);
    }
    float right[3] = {g_top[1] * g_front[2] - g_top[2] * g_front[1], g_top[2] * g_front[0] - g_top[0] * g_front[2],
                      g_top[0] * g_front[1] - g_top[1] * g_front[0]};
    float side = rel ? d[0] : d[0] * right[0] + d[1] * right[1] + d[2] * right[2];
    float pan = dist > 1e-3f ? side / dist : 0.0f;
    for (int c = 0; c < 2; c++) {
        g[c][0] *= att * std::min(1.0f, 1.0f - pan);
        g[c][1] *= att * std::min(1.0f, 1.0f + pan);
    }
}

int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// a stream goes quiet: the synch hold ends, a started one loses its pause as well
void Deactivate(Voice& v) {
    v.sync = false;
    v.starved_at = -1;
    v.stopping = false;
    if (v.active) v.active = v.paused = v.ending = false;
}

// the amplitude envelope's release ran out and the hardware turned the voice off. A buffer stops where it is; a
// stream's packets still queued complete as flushed, their callbacks run in DirectSoundDoWork as any others.
void ReleaseEnded(Voice& v) {
    if (!v.stream) {
        v.playing = false;
        return;
    }
    for (auto& pk : v.packets) Complete(v, pk, kFlushed);
    v.packets.clear();
    v.pos = 0;
    Deactivate(v);
}

// Two stereo streams (the format of every song) fed and audible together for longer than a crossfade is two songs
// at once. Logged once per episode so a run can be checked without listening.
void WatchOverlap() {
    static int64_t since = -1;
    static bool reported = false;
    static std::atomic<unsigned> episodes{0};
    uint32_t ids[2]{}, n = 0;
    for (const auto& entry : g_voices) {
        const Voice& v = entry.second;
        if (v.stream && v.channels == 2 && v.active && !v.hold && !v.paused && !v.sync && !v.packets.empty() &&
            !v.stopping && v.volume > 0.01f && n < 2)
            ids[n++] = v.obj;
    }
    if (n < 2) {
        since = -1;
        reported = false;
        return;
    }
    const int64_t now = NowMs();
    if (since < 0) since = now;
    if (!reported && now - since > 3000) {
        reported = true;
        if (episodes.fetch_add(1) < 16) LOGR("dsound: stereo streams %08X and %08X both playing for over 3 s", ids[0], ids[1]);
    }
}

void Render(float* out, uint32_t frames) {
    std::memset(out, 0, frames * 2 * sizeof(float));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    WatchOverlap();
    for (auto& entry : g_voices) {
        Voice& v = entry.second;  // a plain reference, the lambdas below capture it
        if (v.paused || v.sync || v.hold || (!v.stream && !v.playing)) continue;
        float g[2][2];
        Levels(v, g);
        uint32_t base = v.data + v.play_start;
        double hz = v.freq ? v.freq : v.rate;
        double pitch = v.pitch_override ? v.pitch : 4096.0 * std::log2(std::max(hz, 1.0) / 48000.0);
        // envelopes and LFOs move on the hardware's 32 sample frames of output time, whatever the source rate
        auto rate = [&] { return 48000.0 / audio::kRate * v.voice3d.PitchRatio(pitch + v.mod.pitch); };
        double step = rate();
        bool off = false;  // the amplitude envelope ended the voice
        auto tick = [&] {
            if (v.mod.sample == 0) {
                off = !v.mod.Step();
                step = rate();
            }
            v.mod.sample = (v.mod.sample + 1) % audio::kFrameSamples;
        };
        uint32_t i = 0;
        for (; i < frames; i++) {
            if (v.stream && v.packets.empty() && !v.active) break;  // never started or gone quiet: no voice runs
            tick();
            if (off) break;
            uint32_t f = uint32_t(v.pos);
            float a, b, a1, b1;  // this frame and the next, interpolated between
            if (v.stream) {
                while (!v.packets.empty() && f >= FramesOf(v, v.packets.front().size)) {
                    Packet pk = v.packets.front();
                    v.packets.pop_front();
                    pk.used = pk.size;
                    Complete(v, pk, 0);
                    v.pos -= FramesOf(v, pk.size);
                    f = uint32_t(v.pos);
                }
                if (v.packets.empty()) {
                    v.pos = 0;
                    // a stopping stream out of data is done: its release cannot run on, and it must not wait for a
                    // refill as a stalled song does
                    if (v.ending || v.stopping) Deactivate(v);
                    else if (v.starved_at < 0) {
                        v.starved_at = NowMs();
                        static std::atomic<unsigned> reported{0};  // release builds keep LOGR, not LOG_FIRST
                        if (reported.fetch_add(1) < 16)
                            LOGR("dsound: stream %08X ran out of packets mid-wave, still PLAYING for its refill", v.obj);
                    }
                    break;
                }
                Packet& pk = v.packets.front();
                a = Sample(v, pk.data, f, 0);
                b = Sample(v, pk.data, f, 1);
                if (f + 1 < FramesOf(v, pk.size)) {
                    a1 = Sample(v, pk.data, f + 1, 0);
                    b1 = Sample(v, pk.data, f + 1, 1);
                } else if (v.packets.size() > 1) {
                    a1 = Sample(v, v.packets[1].data, 0, 0);
                    b1 = Sample(v, v.packets[1].data, 0, 1);
                } else {
                    a1 = a;
                    b1 = b;
                }
            } else {
                uint32_t start = v.looping ? FramesOf(v, v.loop_start) : 0;
                uint32_t end = FramesOf(v, v.looping ? v.loop_start + v.loop_len : v.play_len);
                if (f >= end) {
                    if (!v.looping || end <= start) {
                        v.playing = false;
                        v.pos = 0;
                        break;
                    }
                    v.pos = start + std::fmod(v.pos - start, double(end - start));
                    f = uint32_t(v.pos);
                }
                if (!v.data) break;
                a = Sample(v, base, f, 0);
                b = Sample(v, base, f, 1);
                uint32_t next = f + 1 < end ? f + 1 : v.looping ? start : f;
                a1 = Sample(v, base, next, 0);
                b1 = Sample(v, base, next, 1);
            }
            float t = float(v.pos - f);
            a = (a + (a1 - a) * t) * v.mod.gain;
            b = (b + (b1 - b) * t) * v.mod.gain;
            out[i * 2] += a * g[0][0] + b * g[1][0];
            out[i * 2 + 1] += a * g[0][1] + b * g[1][1];
            v.pos += step;
        }
        // a starved stream's voice still runs, and so do its envelopes, for the rest of the block
        bool running = v.stream ? v.active : v.playing;
        for (uint32_t k = i + 1; running && !off && k < frames; k++) tick();
        if (off) ReleaseEnded(v);
    }
    for (uint32_t i = 0; i < frames * 2; i++) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

void Run(X86Context& ctx, const std::vector<Done>& done) {
    for (const Done& d : done) CallGuest(ctx, d.callback, {d.context, d.packet_context, d.status});
}

void Start() {
    if (g_started) return;
    g_started = true;
    audio::StartOutput(Render);
    // for a title that never calls DirectSoundDoWork: its callbacks come from here instead
    StartHostThread("dsound", [](X86Context& ctx) {
        constexpr int64_t kPumpMs = 250;
        for (;;) {
            std::vector<Done> done;
            {
                std::unique_lock<std::recursive_mutex> lock(g_lock);
                g_done_cv.wait_for(lock, std::chrono::milliseconds(50));
                if (NowMs() - g_pumped < kPumpMs) continue;
                done.assign(g_done.begin(), g_done.end());
                g_done.clear();
            }
            Run(ctx, done);
        }
    });
}

// WAVEFORMATEX
void ReadFormat(Voice& v, uint32_t wfx) {
    if (!wfx) return;
    v.tag = R16(wfx);
    v.channels = std::max<uint16_t>(R16(wfx + 2), 1);
    v.rate = R32(wfx + 4);
    v.align = std::max<uint16_t>(R16(wfx + 12), 1);
    v.bits = R16(wfx + 14);
    if (v.tag != 1 && v.tag != 0x69) LOG_FIRST(8, "dsound: format tag %X not supported", v.tag);
}

// DSMIXBINS: the voice's bins and their volumes
void SetBins(Voice& v, uint32_t mixbins) {
    v.nbins = 0;  // none listed: front left and right
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 8), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) {
        v.bin[i] = R32(pairs + i * 8) & 31;
        v.bin_mb[v.bin[i]] = int32_t(R32(pairs + i * 8 + 4));
    }
    v.nbins = uint8_t(n);
}
// volumes only, the bins stay
void SetBinVolumes(Voice& v, uint32_t mixbins) {
    if (!mixbins) return;
    uint32_t n = std::min<uint32_t>(R32(mixbins), 32), pairs = R32(mixbins + 4);
    for (uint32_t i = 0; i < n; i++) v.bin_mb[R32(pairs + i * 8) & 31] = int32_t(R32(pairs + i * 8 + 4));
}
// 600 mB unless 3D or a mix in or effect in buffer
float DefaultHeadroom(uint32_t flags) { return flags & 0x382010 ? 1.0f : Db(-600); }

Voice* Find(uint32_t obj) {
    auto it = g_voices.find(obj);
    return it == g_voices.end() ? nullptr : &it->second;
}

uint32_t GetVoiceProperties(const Voice& v, uint32_t out) {
    if (!out) return 0x80070057u;
    // DSVOICEPROPS is 0x5C bytes, including eight inline DSMIXBINVOLUMEPAIR entries.
    std::memset(HostPtr(out), 0, 0x5C);
    uint32_t n = v.nbins ? v.nbins : 2;
    W32(out, n);
    float level = v.volume * v.headroom;
    int32_t mb = level > 0 ? int32_t(std::lround(2000.0f * std::log10(level))) : -10000;
    for (uint32_t i = 0; i < 8; ++i) {
        uint32_t bin = i < n ? (v.nbins ? v.bin[i] : i) : 0xFFFFFFFFu;
        W32(out + 4 + i * 8, bin);
        int32_t volume = i < n ? int32_t(std::clamp<int64_t>(int64_t(mb) + v.bin_mb[bin] + v.voice3d.Attenuation(bin), -10000, 0)) : -10000;
        W32(out + 8 + i * 8, uint32_t(volume));
    }
    double hz = v.freq ? v.freq : v.rate;
    int32_t pitch = v.pitch_override ? v.pitch : int32_t(std::lround(4096.0 * std::log2(std::max(hz, 1.0) / 48000.0)));
    int64_t combined_pitch = int64_t(pitch) + (v.voice3d.enabled ? v.voice3d.DopplerPitch() : 0);
    W32(out + 0x44, uint32_t(int32_t(std::clamp<int64_t>(combined_pitch, -32767, 8191))));
    if (v.voice3d.enabled) {
        W32(out + 0x48, v.voice3d.words[1]);
        W32(out + 0x4C, v.voice3d.words[2]);
        W32(out + 0x50, v.voice3d.words[9]);
        W32(out + 0x54, v.voice3d.words[7]);
        W32(out + 0x58, v.voice3d.words[8]);
    }
    return 0;
}

uint32_t Set3DVoiceData(Voice& v, uint32_t data) {
    if (!data) return 0x80070057u;
    uint32_t words[14];
    std::memcpy(words, HostPtr(data), sizeof(words));
    v.voice3d.Set(words);
    return 0;
}

// DSENVELOPEDESC, ten dwords: generator, mode, delay, attack, hold, decay, release, sustain, pitch scale, cutoff scale.
// As CMcpxVoiceClient::SetEG (0x3C6F1B) packs it: generator 0 is the multi envelope (ENV1/ENVF, release in MISC, the
// scales in the top bytes of ENV0/ENV1, mode in VOICE_ON bits 24-27), generator 1 the amplitude one (ENV0/ENVA,
// release in LFO_ENV, mode in bits 28-31) and leaves the scales alone. Any other generator changes nothing.
// The times and the sustain take effect at once; the mode is only read at the next voice on.
uint32_t SetEG(Voice& v, uint32_t desc) {
    if (!desc) return 0x80070057u;
    uint32_t gen = R32(desc);
    if (gen > 1) return 0;
    audio::Envelope& e = gen ? v.mod.amp : v.mod.multi;
    e.mode = uint8_t(R32(desc + 4) & 15);
    e.delay = uint16_t(R32(desc + 8) & 0xFFF);
    e.attack = uint16_t(R32(desc + 12) & 0xFFF);
    e.hold = uint16_t(R32(desc + 16) & 0xFFF);
    e.decay = uint16_t(R32(desc + 20) & 0xFFF);
    e.release = uint16_t(R32(desc + 24) & 0xFFF);
    e.sustain = uint8_t(R32(desc + 28));
    if (!gen) {
        v.mod.pitch_scale = int8_t(uint8_t(R32(desc + 32)));
        v.mod.cutoff_scale = int8_t(uint8_t(R32(desc + 36)));
    }
    LOG_FIRST(16, "dsound: %08X %s envelope mode %u delay %u attack %u hold %u decay %u release %u sustain %u scales %d %d",
              v.obj, gen ? "amplitude" : "multi", unsigned(e.mode), unsigned(e.delay), unsigned(e.attack),
              unsigned(e.hold), unsigned(e.decay), unsigned(e.release), unsigned(e.sustain), int(v.mod.pitch_scale),
              int(v.mod.cutoff_scale));
    return 0;
}

// DSLFODESC, six dwords: LFO, delay, delta, pitch, cutoff and amplitude modulation. CMcpxVoiceClient::SetLFO (0x3C6DEC)
// writes DSLFO_MULTI (0) to LFOA: enable MISC bit 14 for a nonzero delay, 15 bit delay, 10 bit delta, three signed
// depth bytes in LFO_MOD; DSLFO_PITCH (1) to LFOF: bit 15, the upper delay and delta fields, the pitch depth only.
// The registers go to the voice at once, the delay counts from voice on.
uint32_t SetLFO(Voice& v, uint32_t desc) {
    if (!desc) return 0x80070057u;
    uint32_t which = R32(desc), delay = R32(desc + 4);
    if (which > 1) return 0;
    audio::Lfo& l = which ? v.mod.pitch_lfo : v.mod.multi_lfo;
    l.enabled = delay != 0;
    l.delay = uint16_t(delay & 0x7FFF);
    l.delta = uint16_t(R32(desc + 8) & 0x3FF);
    l.pitch = int8_t(uint8_t(R32(desc + 12)));
    if (!which) {
        l.cutoff = int8_t(uint8_t(R32(desc + 16)));
        l.amplitude = int8_t(uint8_t(R32(desc + 20)));
    }
    LOG_FIRST(8, "dsound: %08X %s LFO delay %u delta %u depths %d %d %d", v.obj, which ? "pitch" : "multi",
              unsigned(l.delay), unsigned(l.delta), int(l.pitch), int(l.cutoff), int(l.amplitude));
    return 0;
}

uint32_t NewBuffer(uint32_t desc, uint32_t out) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    uint32_t obj = mem::AllocSystem(0x40);
    std::memset(HostPtr(obj), 0, 0x40);
    Voice& v = g_voices[obj];
    v.obj = obj;
    uint32_t flags = R32(desc + 4), bytes = R32(desc + 8);  // DSBUFFERDESC: size, flags, bytes, format, mix bins
    ReadFormat(v, R32(desc + 12));
    SetBins(v, R32(desc + 16));
    v.is3d = flags & 0x10;  // DSBCAPS_CTRL3D
    v.headroom = DefaultHeadroom(flags);
    if (bytes) {
        v.data = v.owned = mem::AllocContiguous(bytes, 0, 0xFFFFFFFF, 4);
        v.bytes = v.play_len = v.loop_len = bytes;
    }
    W32(out, obj);
    return 0;
}

float F32(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

}  // namespace

// device

// DSOUND objects are the host's; .XTLID names linked exports that would still run console code on them
static void AuditExports() {
    static const uint32_t handled[] = {
        SYM(DirectSoundCreate), SYM(IDirectSound_Release), SYM(DirectSoundUseFullHRTF), SYM(DirectSoundUseLightHRTF),
        SYM(IDirectSound_GetSpeakerConfig),
        SYM(IDirectSound_SetEffectData), SYM(IDirectSound_EnableHeadphones), SYM(IDirectSound_SetMixBinHeadroom),
        SYM(IDirectSound_CommitDeferredSettings), SYM(IDirectSound_SetDopplerFactor), SYM(IDirectSound_SetRolloffFactor),
        SYM(IDirectSound_SetDistanceFactor), SYM(IDirectSound_SetVelocity), SYM(IDirectSound_SetI3DL2Listener),
        SYM(IDirectSound_SetPosition), SYM(IDirectSound_SetOrientation), SYM(IDirectSound_SynchPlayback), SYM(DirectSoundDoWork),
        SYM(IDirectSound_DownloadEffectsImage), SYM(IDirectSound_GetEffectData), SYM(IDirectSound_CommitEffectData),
        SYM(IDirectSound_GetOutputLevels), SYM(IDirectSoundBuffer_SetPlayRegion), SYM(IDirectSoundBuffer_SetAllParameters),
        SYM(IDirectSoundBuffer_SetFormat), SYM(IDirectSoundBuffer_SetOutputBuffer), SYM(IDirectSoundBuffer_SetRolloffCurve),
        SYM(IDirectSoundBuffer_StopEx),
        SYM(IDirectSoundBuffer_GetVoiceProperties), SYM(IDirectSoundBuffer_Set3DVoiceData),
        SYM(IDirectSoundBuffer_Use3DVoiceData), SYM(CDirectSoundStream_GetVoiceProperties),
        SYM(CDirectSoundStream_Set3DVoiceData), SYM(CDirectSoundStream_Use3DVoiceData),
        SYM(IDirectSoundBuffer_SetPitch), SYM(IDirectSoundBuffer_SetLFO), SYM(IDirectSoundBuffer_SetEG),
        SYM(CDirectSoundStream_SetPitch), SYM(CDirectSoundStream_SetLFO), SYM(CDirectSoundStream_SetOutputBuffer),
        SYM(IDirectSound_GetCaps),
        SYM(XAudioDownloadEffectsImage), SYM(DirectSoundCreateBuffer), SYM(IDirectSound_CreateSoundBuffer),
        SYM(IDirectSoundBuffer_Release), SYM(IDirectSoundBuffer_Unlock), SYM(IDirectSoundBuffer_SetBufferData),
        SYM(IDirectSoundBuffer_Play), SYM(IDirectSoundBuffer_Stop), SYM(IDirectSoundBuffer_SetLoopRegion),
        SYM(IDirectSoundBuffer_GetStatus), SYM(IDirectSoundBuffer_GetCurrentPosition), SYM(IDirectSoundBuffer_SetCurrentPosition),
        SYM(IDirectSoundBuffer_Lock), SYM(IDirectSoundBuffer_SetVolume), SYM(IDirectSoundBuffer_SetFrequency),
        SYM(IDirectSoundBuffer_SetFilter), SYM(IDirectSoundBuffer_SetHeadroom), SYM(IDirectSoundBuffer_SetMixBinVolumes),
        SYM(IDirectSoundBuffer_SetMixBins), SYM(IDirectSoundBuffer_SetMaxDistance), SYM(IDirectSoundBuffer_SetMinDistance),
        SYM(IDirectSoundBuffer_SetPosition), SYM(IDirectSoundBuffer_SetVelocity), SYM(IDirectSoundBuffer_SetI3DL2Source),
        SYM(DirectSoundCreateStream), SYM(IDirectSound_CreateSoundStream), SYM(CDirectSoundStream_AddRef), SYM(CDirectSoundStream_SetFormat),
        SYM(CDirectSoundStream_Release), SYM(CDirectSoundStream_GetInfo), SYM(CDirectSoundStream_GetStatus),
        SYM(CDirectSoundStream_Process), SYM(CDirectSoundStream_Discontinuity), SYM(CDirectSoundStream_Flush),
        SYM(CDirectSoundStream_FlushEx), SYM(CDirectSoundStream_Pause), SYM(CDirectSoundStream_SetVolume),
        SYM(CDirectSoundStream_SetFrequency), SYM(CDirectSoundStream_SetFilter), SYM(CDirectSoundStream_SetHeadroom),
        SYM(CDirectSoundStream_SetMixBinVolumes), SYM(CDirectSoundStream_SetMixBins), SYM(CDirectSoundStream_SetMaxDistance),
        SYM(CDirectSoundStream_SetMinDistance), SYM(CDirectSoundStream_SetPosition), SYM(CDirectSoundStream_SetVelocity),
        SYM(CDirectSoundStream_SetI3DL2Source), SYM(IDirectSoundStream_SetEG)};
    std::string left;
    // the library's own section: the WMA decoder shares the export ids and touches no DSOUND object
    uint32_t lo = 0, hi = 0;
    for (auto& s : g_image->sections)
        if (SYM(DirectSoundCreate) - s.va < s.vsize) lo = s.va, hi = s.va + s.vsize;
    for (auto& s : g_image->sections)
        for (uint32_t o = 0; s.name == ".XTLID" && o + 8 <= std::min(s.vsize, s.rsize); o += 8) {
            uint32_t id = g_image->U32(s.raw + o), va = g_image->U32(s.raw + o + 4);
            auto done = [&](uint32_t f) { return std::find(std::begin(handled), std::end(handled), f) != std::end(handled); };
            // a jmp to a host version is covered by it
            if (id >> 16 != 0xB || va < lo || va >= hi || !dispatch::Find(va) || done(va) ||  // 0xB: DSOUND's library id
                (G<uint8_t>(va) == 0xE9 && done(va + 5 + R32(va + 1))))
                continue;
            char b[24];
            std::snprintf(b, sizeof b, " %05X at %08X", id, va);
            left += b;
        }
    if (!left.empty()) LOGW("dsound: exports running their original code:%s", left.c_str());
}

HLE_STDCALL(DirectSoundCreate, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    if (!g_ds) {
        AuditExports();
        g_ds = mem::AllocSystem(0x40);
        std::memset(HostPtr(g_ds), 0, 0x40);
    }
    W32(a[1], g_ds);
    return 0;
}
HLE_STDCALL(IDirectSound_Release, 1) { return 0; }
HLE_STDCALL(IDirectSound_GetCaps, 2) {
    if (!a[1]) return 0x80070057u;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t two_d = 0, three_d = 0, pages = 0, allocated = 0;
    for (const auto& [obj, voice] : g_voices) {
        if (voice.is3d) ++three_d;
        else ++two_d;
        pages += (voice.bytes + 4095) / 4096;
        allocated += mem::AllocationSize(obj) + (voice.owned ? mem::AllocationSize(voice.owned) : 0);
    }
    // DSCAPS: report the title's voice/SGE limits and the host mixer's current allocations.
    W32(a[1], 192 - std::min(two_d, 192u));
    W32(a[1] + 4, 64 - std::min(three_d, 64u));
    W32(a[1] + 8, 2047 - std::min(pages, 2047u));
    W32(a[1] + 12, allocated);
    return 0;
}
HLE_STDCALL(DirectSoundUseFullHRTF, 0) { return 0; }
HLE_STDCALL(DirectSoundUseLightHRTF, 0) { return 0; }
HLE_STDCALL(IDirectSound_GetSpeakerConfig, 2) {
    W32(a[1], 0);  // DSSPEAKER_STEREO
    return 0;
}
HLE_STDCALL(IDirectSound_SetEffectData, 6) { return 0; }
HLE_STDCALL(IDirectSound_EnableHeadphones, 2) { return 0; }
HLE_STDCALL(IDirectSound_SetMixBinHeadroom, 3) { return 0; }
HLE_STDCALL(IDirectSound_CommitDeferredSettings, 1) { return 0; }
HLE_STDCALL(IDirectSound_SetDopplerFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetRolloffFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetDistanceFactor, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetVelocity, 5) { return 0; }
HLE_STDCALL(IDirectSound_SetI3DL2Listener, 3) { return 0; }
HLE_STDCALL(IDirectSound_SetPosition, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) g_listener[i] = F32(a[1 + i]);
    return 0;
}
HLE_STDCALL(IDirectSound_SetOrientation, 8) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (int i = 0; i < 3; i++) {
        g_front[i] = F32(a[1 + i]);
        g_top[i] = F32(a[4 + i]);
    }
    return 0;
}
HLE_STDCALL(DirectSoundDoWork, 0) {
    g_pumped = NowMs();
    std::vector<Done> done;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        done.assign(g_done.begin(), g_done.end());
        g_done.clear();
    }
    Run(ctx, done);
    return 0;
}
// starts together every voice played or unpaused with the synch playback flag
HLE_STDCALL(IDirectSound_SynchPlayback, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    for (auto& [obj, v] : g_voices) v.sync = false;
    return 0;
}

// Effects run on the console's DSP and are not reproduced.
// the descriptor keeps the image's effect list so the title finds its indices, every segment points at zeroed memory
static uint32_t EffectsDesc(const std::vector<uint8_t>& img, const char* name) {
    XDK_REQUIRE("dsound", XDK_KEY(EffectsImage_Skip0), XDK_KEY(EffectsImage_Skip1), XDK_KEY(EffectsImage_Desc));
    auto u32 = [&](size_t o) { uint32_t v = 0; if (o + 4 <= img.size()) std::memcpy(&v, &img[o], 4); return v; };
    size_t t = XDK_EffectsImage_Desc + size_t(u32(XDK_EffectsImage_Skip0) + u32(XDK_EffectsImage_Skip1)) * 4;
    uint32_t count = std::min<uint32_t>(u32(t), 64);
    LOGI("dsound: effects image %s, %u effects", name, count);
    uint32_t desc = mem::AllocSystem(8 + std::max(count, 1u) * 32);
    W32(desc, count);
    W32(desc + 4, u32(t + 4));
    for (uint32_t i = 0; i < count; i++) {
        size_t m = t + 8 + i * 32;
        uint32_t d = desc + 8 + i * 32;
        for (int k = 0; k < 4; k++) {
            uint32_t size = u32(m + k * 8 + 4);
            uint32_t seg = mem::AllocSystem(std::max(size, 0x40u));
            std::memset(HostPtr(seg), 0, std::max(size, 0x40u));
            W32(d + k * 8, seg);
            W32(d + k * 8 + 4, size);
        }
    }
    return desc;
}

HLE_STDCALL(XAudioDownloadEffectsImage, 4) {
    std::string name = ReadCString(a[0]);
    std::vector<uint8_t> img;
    if (a[2] & 1) {
        for (auto& sec : g_image->sections)
            if (sec.name == name) img.assign(static_cast<uint8_t*>(HostPtr(sec.va)), static_cast<uint8_t*>(HostPtr(sec.va)) + sec.vsize);
    } else {
        ReadGameFile(name, img);
    }
    uint32_t desc = EffectsDesc(img, name.c_str());
    if (a[3]) W32(a[3], desc);
    return 0;
}

// (this, image, size, location, descriptor out): the title loaded the image itself
HLE_STDCALL(IDirectSound_DownloadEffectsImage, 5) {
    std::vector<uint8_t> img(static_cast<uint8_t*>(HostPtr(a[1])), static_cast<uint8_t*>(HostPtr(a[1])) + a[2]);
    uint32_t desc = EffectsDesc(img, "from memory");
    if (a[4]) W32(a[4], desc);
    return 0;
}
// (this, effect, offset, data, size): SetEffectData keeps nothing, so effect memory reads as zero
HLE_STDCALL(IDirectSound_GetEffectData, 5) {
    if (a[3]) std::memset(HostPtr(a[3]), 0, a[4]);
    return 0;
}
HLE_STDCALL(IDirectSound_CommitEffectData, 1) { return 0; }
// (this, DSOUTPUTLEVELS out, reset peaks): 16 dwords of meters, silent
HLE_STDCALL(IDirectSound_GetOutputLevels, 3) {
    if (a[1]) std::memset(HostPtr(a[1]), 0, 64);
    return 0;
}

// buffers

HLE_STDCALL(DirectSoundCreateBuffer, 2) { return NewBuffer(a[0], a[1]); }
HLE_STDCALL(IDirectSound_CreateSoundBuffer, 4) { return NewBuffer(a[1], a[2]); }

#define BUFFER(name, nargs)                                 \
    static uint32_t buf_##name(Voice& v, const Args& a);    \
    HLE_STDCALL(IDirectSoundBuffer_##name, nargs) {         \
        std::lock_guard<std::recursive_mutex> lock(g_lock); \
        Voice* v = Find(a[0]);                              \
        return v ? buf_##name(*v, a) : 0x80004005u;         \
    }                                                       \
    static uint32_t buf_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

BUFFER(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    if (v.owned) mem::FreeContiguous(v.owned);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    mem::FreeSystem(obj);
    return 0;
}
BUFFER(Unlock, 5) { return 0; }
BUFFER(GetVoiceProperties, 2) { return GetVoiceProperties(v, a[1]); }
BUFFER(Set3DVoiceData, 2) { return Set3DVoiceData(v, a[1]); }
BUFFER(Use3DVoiceData, 2) {
    v.voice3d.enabled = a[1] != 0;
    return 0;
}
BUFFER(SetPitch, 2) {
    v.pitch = int32_t(a[1]);
    v.pitch_override = true;
    return 0;
}
BUFFER(SetLFO, 2) { return SetLFO(v, a[1]); }
BUFFER(SetEG, 2) { return SetEG(v, a[1]); }
BUFFER(SetBufferData, 3) {
    if (a[1] == v.data && a[2] == v.bytes) return 0;
    // the library frees its own memory, and makes it again for a size with no data
    if (v.owned && v.owned != a[1]) {
        mem::FreeContiguous(v.owned);
        v.owned = 0;
    }
    v.data = a[1];
    if (!a[1] && a[2]) v.data = v.owned = mem::AllocContiguous(a[2], 0, 0xFFFFFFFF, 4);
    v.bytes = v.play_len = v.loop_len = a[2];
    v.play_start = v.loop_start = 0;
    v.pos = 0;
    v.block = -1;
    return 0;
}
BUFFER(Play, 4) {
    // VOICE_ON only for a voice that is not running (0x3C7CB5): one still releasing goes on releasing
    if (!v.playing) v.mod.On();
    v.playing = true;
    v.looping = a[3] & 1;
    if (a[3] & 2) v.pos = 0;
    v.sync = a[3] & 4;
    return 0;
}
BUFFER(Stop, 1) {
    v.playing = false;
    return 0;
}
// (this, time low, time high, flags), the time is not scheduled. As 0x3C87ED: DSBSTOPEX_ENVELOPE (1) releases both
// envelopes (VOICE_RELEASE) and the buffer plays on until the amplitude one has run out, DSBSTOPEX_RELEASEWAVEFORM
// (2) with it also leaves the loop so the buffer plays out to its end. A zero release time stops it at once.
BUFFER(StopEx, 4) {
    if (!v.playing) return 0;
    if (!(a[3] & 1)) {
        v.playing = false;
        return 0;
    }
    if (a[3] & 2) v.looping = false;
    v.mod.Release();
    if (v.mod.Instant()) v.playing = false;
    return 0;
}
// (this, start, length) in bytes, 0 length plays to the end. The loop becomes the whole region.
BUFFER(SetPlayRegion, 3) {
    if (a[1] > v.bytes || a[2] > v.bytes - a[1]) return 0x88780032u;  // DSERR_INVALIDCALL
    v.play_start = a[1];
    v.play_len = v.loop_len = a[2] ? a[2] : v.bytes - a[1];
    v.loop_start = 0;
    v.pos = 0;
    v.block = -1;
    return 0;
}
BUFFER(SetLoopRegion, 3) {
    if (a[1] > v.play_len || a[2] > v.play_len - a[1]) return 0x88780032u;
    v.loop_start = a[1];
    v.loop_len = a[2] ? a[2] : v.play_len - a[1];
    return 0;
}
BUFFER(GetStatus, 2) {
    W32(a[1], (v.playing ? 1u : 0u) | (v.playing && v.looping ? 4u : 0u));  // DSBSTATUS_PLAYING, DSBSTATUS_LOOPING
    return 0;
}
BUFFER(GetCurrentPosition, 3) {
    uint32_t p = BytesOf(v, uint32_t(v.pos));
    if (a[1]) W32(a[1], p);
    if (a[2]) W32(a[2], v.play_len ? (p + BytesOf(v, 64 * 4)) % v.play_len : 0);
    return 0;
}
BUFFER(SetCurrentPosition, 2) {
    v.pos = FramesOf(v, a[1]);
    return 0;
}
BUFFER(Lock, 8) {
    uint32_t off = a[1], n = a[2];
    if (a[7] & 1) n = v.bytes;  // DSBLOCK_ENTIREBUFFER
    if (!v.bytes) return 0x80004005u;  // E_FAIL
    off %= v.bytes;
    uint32_t first = std::min(n, v.bytes - off);
    W32(a[3], v.data + off);
    W32(a[4], first);
    if (a[5]) W32(a[5], n > first ? v.data : 0);
    if (a[6]) W32(a[6], n > first ? n - first : 0);
    return 0;
}
BUFFER(SetVolume, 2) {
    v.volume = Db(int32_t(a[1]));
    return 0;
}
BUFFER(SetFrequency, 2) {
    v.freq = a[1];
    v.pitch_override = false;
    return 0;
}
BUFFER(SetFilter, 2) { return 0; }
BUFFER(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
BUFFER(SetMixBinVolumes, 2) {
    SetBinVolumes(v, a[1]);
    return 0;
}
BUFFER(SetMixBins, 2) {
    SetBins(v, a[1]);
    return 0;
}
BUFFER(SetMaxDistance, 3) {
    v.max_dist = F32(a[1]);
    return 0;
}
BUFFER(SetMinDistance, 3) {
    v.min_dist = F32(a[1]);
    return 0;
}
BUFFER(SetPosition, 5) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(a[1 + i]);
    return 0;
}
BUFFER(SetVelocity, 5) { return 0; }
BUFFER(SetI3DL2Source, 3) { return 0; }
// (this, DS3DBUFFER, apply): position at +4, distances at +0x34 and +0x38, mode at +0x3C
BUFFER(SetAllParameters, 3) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(R32(a[1] + 4 + i * 4));
    v.min_dist = F32(R32(a[1] + 0x34));
    v.max_dist = F32(R32(a[1] + 0x38));
    v.mode = R32(a[1] + 0x3C);
    return 0;
}
// (this, points, count, apply)
BUFFER(SetRolloffCurve, 4) {
    v.curve.clear();
    for (uint32_t i = 0; a[1] && i < std::min<uint32_t>(a[2], 64); i++) v.curve.push_back(F32(R32(a[1] + i * 4)));
    return 0;
}
BUFFER(SetOutputBuffer, 2) { return 0; }  // the voice keeps its own mix bins
BUFFER(SetFormat, 2) {
    ReadFormat(v, a[1]);
    v.freq = 0;
    v.pitch_override = false;
    v.block = -1;
    return 0;
}

// streams, the object carries the XDK's own vtable

static uint32_t NewStream(uint32_t desc, uint32_t out) {
    XDK_REQUIRE("dsound", XDK_KEY(CDirectSoundStream_Vtable), XDK_KEY(CDirectSoundStream_Vtable2),
                XDK_DATA(DSound_StreamVtable), XDK_DATA(DSound_StreamVtable2));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Start();
    if (!desc || !out) return 0x80070057u;
    uint32_t obj = mem::AllocSystem(0x80);
    std::memset(HostPtr(obj), 0, 0x80);
    W32(obj + XDK_CDirectSoundStream_Vtable, kStreamVtable);
    W32(obj + XDK_CDirectSoundStream_Vtable2, kStreamVtable2);
    Voice& v = g_voices[obj];
    v.obj = obj;
    v.stream = true;
    uint32_t flags = R32(desc);
    v.max_packets = std::max<uint32_t>(R32(desc + 4), 1);
    ReadFormat(v, R32(desc + 8));
    v.callback = R32(desc + 12);
    v.context = R32(desc + 16);
    SetBins(v, R32(desc + 20));
    v.is3d = flags & 0x10;  // DSBCAPS_CTRL3D
    v.headroom = DefaultHeadroom(flags);
    W32(out, obj);
    return 0;
}

HLE_STDCALL(DirectSoundCreateStream, 2) { return NewStream(a[0], a[1]); }
HLE_STDCALL(IDirectSound_CreateSoundStream, 4) { return NewStream(a[1], a[2]); }

#define STREAM(name, nargs)                                     \
    static uint32_t str_##name(Voice& v, const Args& a);        \
    HLE_STDCALL(CDirectSoundStream_##name, nargs) {             \
        uint32_t r;                                             \
        std::vector<Done> flushed;                              \
        {                                                       \
            std::lock_guard<std::recursive_mutex> lock(g_lock); \
            Voice* v = Find(a[0]);                              \
            r = v ? str_##name(*v, a) : 0x80004005u;            \
            flushed.swap(t_flushed);                            \
        }                                                       \
        Run(ctx, flushed);                                      \
        return r;                                               \
    }                                                           \
    static uint32_t str_##name([[maybe_unused]] Voice& v, [[maybe_unused]] const Args& a)

// the stream's callbacks still queued and those of its flushed packets, in order, go to the calling thread
static void FlushStream(Voice& v) {
    for (auto& pk : v.packets) Complete(v, pk, kFlushed);
    v.packets.clear();
    v.pos = 0;
    for (auto it = g_done.begin(); it != g_done.end();) {
        if (it->voice != v.obj) {
            ++it;
            continue;
        }
        t_flushed.push_back(*it);
        it = g_done.erase(it);
    }
    Deactivate(v);
}

STREAM(AddRef, 1) { return uint32_t(++v.refs); }
STREAM(GetVoiceProperties, 2) { return GetVoiceProperties(v, a[1]); }
STREAM(Set3DVoiceData, 2) { return Set3DVoiceData(v, a[1]); }
STREAM(Use3DVoiceData, 2) {
    v.voice3d.enabled = a[1] != 0;
    return 0;
}
STREAM(SetPitch, 2) {
    v.pitch = int32_t(a[1]);
    v.pitch_override = true;
    return 0;
}
STREAM(SetLFO, 2) { return SetLFO(v, a[1]); }
STREAM(SetOutputBuffer, 2) { return 0; }  // the mixer currently folds every bus to stereo
STREAM(SetFormat, 2) {
    FlushStream(v);
    ReadFormat(v, a[1]);
    v.freq = 0;  // the XDK sets the pitch back to the new format's own rate
    v.pitch_override = false;
    v.block = -1;
    return 0;
}
STREAM(Release, 1) {
    if (--v.refs > 0) return uint32_t(v.refs);
    FlushStream(v);
    uint32_t obj = v.obj;
    g_voices.erase(obj);
    mem::FreeSystem(obj);
    return 0;
}
STREAM(GetInfo, 2) {
    uint32_t info = a[1];
    W32(info, 0x3);  // fixed sample size, fixed packet alignment
    W32(info + 4, v.tag == 0x69 ? 36u * v.channels : v.align);
    W32(info + 8, 0);
    W32(info + 12, 0);
    return 0;
}
// audio::StreamStatus has the bits, and why a starved stream short of its Discontinuity still says PLAYING. A stream
// releasing under FlushEx keeps PLAYING until its release ends, which XACT and the music player wait on.
STREAM(GetStatus, 2) {
    audio::StreamState s;
    s.free_slot = v.packets.size() < v.max_packets;
    s.running = v.active && !v.hold;
    s.has_packets = !v.packets.empty();
    s.paused = v.paused || v.sync;
    s.ending = v.ending;
    s.stopping = v.stopping;
    s.starved_ms = v.starved_at < 0 ? -1 : NowMs() - v.starved_at;
    W32(a[1], audio::StreamStatus(s));
    return 0;
}
STREAM(Process, 3) {
    uint32_t in = a[1];
    if (!in) return 0;
    Packet pk{R32(in), R32(in + 4), R32(in + 8), R32(in + 12), R32(in + 16)};
    if (pk.completed_ptr) W32(pk.completed_ptr, 0);
    if (pk.status_ptr) W32(pk.status_ptr, kPacketPending);
    v.packets.push_back(pk);
    if (v.stopping) {
        static std::atomic<unsigned> reported{0};
        if (reported.fetch_add(1) < 16) LOGR("dsound: stream %08X refilled while stopping", v.obj);
    }
    // the stream's voice comes on with its first packet, not again at packet boundaries, nor while held
    if (!v.active && !v.hold) v.mod.On();
    // how long a starved stream was silent: a song that skips is a stereo stream refilled late, the log says when
    if (v.starved_at >= 0) {
        const int64_t silent = NowMs() - v.starved_at;
        static std::atomic<unsigned> gaps{0};
        if (silent >= 20 && gaps.fetch_add(1) < 400)
            LOGR("dsound: stream %08X (%u ch) silent %lld ms until its refill", v.obj, v.channels, (long long)silent);
    }
    v.active = true;
    v.ending = false;
    v.starved_at = -1;
    return 0;
}
STREAM(Discontinuity, 1) {
    if (!v.packets.empty()) v.ending = true;
    else if (v.active) FlushStream(v);
    return 0;
}
STREAM(Flush, 1) {
    FlushStream(v);
    return 0;
}
// (this, time low, time high, flags), the time is not scheduled. As 0x3C4F37: DSSTREAMFLUSHEX_ENVELOPE2 (4) releases
// the envelopes and keeps the packets playing, they complete as flushed when the release ends; ENVELOPE (2) alone
// flushes them now, with the voice's data gone, and no flag flushes at once. ASYNC (1) only returns early.
STREAM(FlushEx, 4) {
    if (a[3] && !v.active) return 0;
    if (a[3] & 4) {
        v.mod.Release();
        if (!v.mod.Instant()) {
            v.stopping = true;
            return 0;
        }
    }
    FlushStream(v);
    return 0;
}
// As 0x3C5A8C: DSSTREAMPAUSE_RESUME (0), PAUSE (1), SYNCHPLAYBACK (2), PAUSENOACTIVATE (3). PAUSENOACTIVATE on a
// stream that is not running lets its packets queue without starting it; XACT prepares every streamed wave so
// (0x40FED4) and starts it with RESUME when the cue plays (0x40F390). Playing such a stream at once made the next
// song or a cutscene's stream sound over the current one. On a running stream PAUSENOACTIVATE resumes it.
STREAM(Pause, 2) {
    switch (a[1]) {
    case 0:
        v.paused = v.sync = false;
        if (v.hold) {
            v.hold = false;
            if (v.active) v.mod.On();  // the held packets start the voice now
        }
        return 0;
    case 1:
        v.paused = true;
        v.sync = false;
        return 0;
    case 2:
        v.paused = false;
        v.sync = true;
        return 0;
    case 3:
        if (v.active && !v.hold) v.paused = v.sync = false;
        else v.hold = true;
        return 0;
    }
    return 0x80004005u;
}
STREAM(SetVolume, 2) {
    v.volume = Db(int32_t(a[1]));
    return 0;
}
STREAM(SetFrequency, 2) {
    v.freq = a[1];
    v.pitch_override = false;
    return 0;
}
STREAM(SetFilter, 2) { return 0; }
// the IDirectSoundStream thunk at 0x3C22A3 jumps to CDirectSoundStream::SetEG (0x3C1D6F) with the same object
HLE_STDCALL(IDirectSoundStream_SetEG, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Voice* v = Find(a[0]);
    return v ? SetEG(*v, a[1]) : 0x80004005u;
}
STREAM(SetHeadroom, 2) {
    v.headroom = Db(-int32_t(a[1]));
    return 0;
}
STREAM(SetMixBinVolumes, 2) {
    SetBinVolumes(v, a[1]);
    return 0;
}
STREAM(SetMixBins, 2) {
    SetBins(v, a[1]);
    return 0;
}
STREAM(SetMaxDistance, 3) {
    v.max_dist = F32(a[1]);
    return 0;
}
STREAM(SetMinDistance, 3) {
    v.min_dist = F32(a[1]);
    return 0;
}
STREAM(SetPosition, 5) {
    for (int i = 0; i < 3; i++) v.p[i] = F32(a[1 + i]);
    return 0;
}
STREAM(SetVelocity, 5) { return 0; }
STREAM(SetI3DL2Source, 3) { return 0; }
