// Existing tuning regressions keep their deliberately global fixture, while
// tuning_profiles_test covers the real per-config broker independently.
#include "../templates/game/src/game/tuning_profiles.h"
namespace mc3::car_tuning {
Bank CaptureBank() {
    std::lock_guard lock(app::UiLock());
    Bank bank;bank.global=app::Get().tuning;return bank;
}
}
