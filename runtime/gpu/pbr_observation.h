#pragma once
#include <atomic>
namespace gpu::pbr::observation {
// Render thread publishes under its owned settings/library update. The guest/title thread must never
// borrow F1's mutable path/digest strings or acquire UiLock while holding the producer's g_lock.
inline std::atomic<bool> guest_proof_requested{false};
inline void RequestGuestProof(bool requested) {guest_proof_requested.store(requested,std::memory_order_release);}
inline bool GuestProofRequested() {return guest_proof_requested.load(std::memory_order_acquire);}
} // namespace gpu::pbr::observation
