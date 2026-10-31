// IDirectSoundStream::GetStatus bits: the console's, except a stream starved short of its Discontinuity, which
// stays PLAYING so XACT does not end the song while the title thread is late with the refill.
#include "audio/stream_status.h"

#include <cassert>
#include <iostream>

int main() {
    using namespace audio;
    StreamState s;
    // never started, or held by PAUSENOACTIVATE: ready only
    s.free_slot = true;
    assert(StreamStatus(s) == kStreamReady);
    s.has_packets = true;
    assert(StreamStatus(s) == kStreamReady);
    // fed and running
    s.running = true;
    assert(StreamStatus(s) == (kStreamReady | kStreamPlaying));
    s.free_slot = false;
    assert(StreamStatus(s) == kStreamPlaying);
    s.paused = true;
    assert(StreamStatus(s) == kStreamPaused);
    s.paused = false;
    // the packets ran out mid-wave: XACT's IsPlaying (PLAYING bit) must stay true while it refills
    s.free_slot = true;
    s.has_packets = false;
    s.starved_ms = 0;
    assert(StreamStatus(s) & kStreamPlaying);
    s.starved_ms = kStarvedGraceMs - 1;
    assert(StreamStatus(s) == (kStreamReady | kStreamPlaying));
    // no refill for the whole grace: the console's starved status
    s.starved_ms = kStarvedGraceMs;
    assert(StreamStatus(s) == (kStreamReady | kStreamPaused | kStreamStarved));
    // paused while starved, or not yet timed: also the console's
    s.starved_ms = 5;
    s.paused = true;
    assert(StreamStatus(s) == (kStreamReady | kStreamPaused | kStreamStarved));
    s.paused = false;
    s.starved_ms = -1;
    assert(StreamStatus(s) == (kStreamReady | kStreamPaused | kStreamStarved));
    // the wave's Discontinuity came: running dry is its end, never PLAYING
    s.starved_ms = 5;
    s.ending = true;
    assert(!(StreamStatus(s) & kStreamPlaying));
    // a stream being stopped (FlushEx envelope release) that starves is not mid-song either: the console's status,
    // so XACT retires it instead of refilling it over the next song
    s.ending = false;
    s.stopping = true;
    assert(StreamStatus(s) == (kStreamReady | kStreamPaused | kStreamStarved));
    // ... while it still has packets it plays out its release as before
    s.has_packets = true;
    assert(StreamStatus(s) == (kStreamReady | kStreamPlaying));
    std::cout << "stream status: console bits, starved mid-wave stays PLAYING through the grace: PASS\n";
}
