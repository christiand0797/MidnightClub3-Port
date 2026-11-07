// Browser port: Web Audio API output for the game's audio mixer
// (docs/notes/browser_port_plan.md). Implements audio::StartOutput so the game's
// XAudio2/DirectSound path produces sound through the browser's Web Audio API.
//
// The game calls audio::StartOutput(render) on its thread. We store the callback
// and export web_audio_render() so the page's AudioWorklet (set up in play.html)
// can pull interleaved stereo float samples from Wasm memory.
//
// The AudioContext is created and resumed by play.html after a user gesture.
#include "audio/audio.h"

#include <emscripten/emscripten.h>
#include <functional>

namespace audio {

// The game's render callback: fills `frames` interleaved stereo float frames.
static std::function<void(float* out, uint32_t frames)> g_render;

// Called from the AudioWorklet (via play.html's JavaScript) to fill its output
// buffer with the next frames of interleaved stereo float audio.
extern "C" EMSCRIPTEN_KEEPALIVE void web_audio_render(float* out, uint32_t frames) {
    if (g_render) g_render(out, frames);
}

void StartOutput(std::function<void(float* out, uint32_t frames)> render) {
    g_render = std::move(render);
}

}  // namespace audio
