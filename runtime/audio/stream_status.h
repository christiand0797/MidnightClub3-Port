#pragma once
#include <cstdint>

namespace audio {

// IDirectSoundStream::GetStatus. As the XDK's 0x3C4FA9: DSSTREAMSTATUS_READY (XMEDIASTATUS_ACCEPT_INPUT) with a free
// packet slot; a running voice adds PLAYING, or PAUSED when paused or synch-held, PAUSED | STARVED with no packets.
// A held (PAUSENOACTIVATE) stream is not running.
//
// XACT's IsPlaying (0x40F198) is only the PLAYING bit, and its voice update (0x40D871) ends the wave, and with it
// the song, as soon as no read is pending and the stream is not PLAYING. The console's packets never run dry
// mid-wave; the port's can, whenever the title thread stalls past the queued audio (a load, a hitch) and XACT
// misses its refill. A wave XACT is done with always gets Discontinuity (0x40F7DD from 0x40CC88/0x40CD57, also
// after a failed read), which drains the stream and stops it. So a stream starved without one is mid-song: report
// it PLAYING while it waits for the refill, up to kStarvedGraceMs, after which it reports what the console would.
// A stream XACT is stopping (FlushEx with an envelope release, the song changing) is not mid-song: no grace, or the
// stopped song stays alive, gets refilled and plays over the next one.
constexpr int64_t kStarvedGraceMs = 10000;
constexpr uint32_t kStreamReady = 0x1, kStreamPlaying = 0x10000, kStreamPaused = 0x20000, kStreamStarved = 0x40000;

struct StreamState {
    bool free_slot = false;  // fewer packets queued than the stream's maximum
    bool running = false;    // took a packet since it went quiet, and not held
    bool has_packets = false;
    bool paused = false;  // paused or synch-held
    bool ending = false;  // Discontinuity with packets still queued
    bool stopping = false;  // FlushEx released the envelope; the stream ends with the release
    int64_t starved_ms = -1;  // how long the packets have been gone without a Discontinuity, -1 when not starved
};

inline uint32_t StreamStatus(const StreamState& s) {
    uint32_t status = s.free_slot ? kStreamReady : 0u;
    if (!s.running) return status;
    if (!s.has_packets) {
        const bool refilling = !s.ending && !s.stopping && s.starved_ms >= 0 && s.starved_ms < kStarvedGraceMs;
        return status | (refilling && !s.paused ? kStreamPlaying : kStreamPaused | kStreamStarved);
    }
    return status | (s.paused ? kStreamPaused : kStreamPlaying);
}

}  // namespace audio
