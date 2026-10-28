#pragma once
#include <cstdint>

struct X86Context;

namespace mc3::economy {

// The retail field is a signed 32-bit integer. Leave headroom for normal race rewards/purchases.
inline constexpr uint32_t kMaxMoney = 999999999;
enum class Status : uint8_t { Unavailable, Ready, Queued, Applied, CareerChanged, InvalidAmount };
struct Snapshot {
    bool ready = false;
    bool pending = false;
    uint32_t money = 0;
    uint64_t revision = 0;
    Status status = Status::Unavailable;
    uint64_t career_token = 0;  // Opaque identity epoch; never an address or save identifier.
};

// UI-thread APIs: these only inspect/queue host state, never guest memory or save files.
Snapshot ReadSnapshot();
// Bind an explicit action to the snapshot the UI actually displayed. A profile
// switch between rendering and queuing must never retarget it to another career.
bool SetMoney(uint32_t target, uint64_t expected_career_token);
const char* StatusText(Status status);

// Called once per original game update. This is the only place an explicit queued balance is applied.
void ApplyCommands();

// Only the original live garage-update hook calls this, on its game thread. Refreshes the two
// existing Flash wallets once after an applied edit; never changes money or requests a save.
bool RefreshGarageWallet(X86Context& context, uint8_t* base, uint32_t garage);

}  // namespace mc3::economy

// Existing frame hook integration, kept independent of the runtime settings type.
void ApplyEconomyCommands();
