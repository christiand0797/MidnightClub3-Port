#pragma once
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();
// Parse pending title push commands before changing title-scoped draw metadata. No GPU/fence wait.
void FlushPendingCommands();

}  // namespace d3d
