#pragma once
namespace app {
struct Settings;
// Inside the F1 tab body, UiLock held. Returns only persistent preference changes.
bool DrawPbrSettings(Settings& settings);
}
