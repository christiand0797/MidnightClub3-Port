#pragma once
#include "app/app.h"
#include <string>
#include <vector>

// PC metadata only. These IDs are never inserted into the original game save.
namespace app::tuning_profiles {
struct Car {
    std::string model, signature;
    bool operator==(const Car&) const = default;
};
struct View {
    bool ready = false, override_enabled = false, global_editing = false, storage_read_only = false;
    bool profiles_enabled = true;
    uint32_t slot = 0;
    std::string profile, model, key, status;
};
void Load();
bool Save();
bool Dirty();
View Snapshot();
// Caller holds UiLock while using this reference. Commit only after a widget actually changes it.
Settings::Tuning& Editor();
bool CommitEditor();
Settings::Tuning Effective();
Settings::Tuning EffectiveForKey(const std::string& key);
bool SetGlobalEditing(bool enabled);
bool SetProfilesEnabled(bool enabled);
bool CreateOverride();
bool UseGlobal();
bool DeleteOverride();
bool ResetOverride();

// Game-thread adapter supplies an authenticated current-career roster, never a guest pointer as a saved key.
void Observe(const std::string& career, const std::string& label, const std::vector<Car>& cars, uint32_t selected);
void ClearSelection();
std::string KeyForSlot(uint32_t slot);
bool Removed(const std::string& career, uint32_t slot, const std::vector<Car>& before,
             const std::vector<Car>& after, uint32_t selected);
}
