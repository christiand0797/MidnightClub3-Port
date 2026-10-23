#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "kernel/disc.h"

namespace app::portable {

// A relative entry travels with the extracted portable folder. External discs retain their absolute path.
std::filesystem::path ReadDiscPath(const std::filesystem::path& exe_dir);
bool RememberDiscPath(const std::filesystem::path& exe_dir, const std::filesystem::path& disc_path);
std::vector<std::filesystem::path> DiscCandidates(const std::filesystem::path& folder);
bool OpenDisc(const std::filesystem::path& path, std::string_view expected_sha1,
              std::vector<uint8_t>& xbe, std::unique_ptr<kernel::Disc>& disc, std::string& error);

// The native Windows picker returns an empty path when cancelled; it never downloads or copies game data.
std::filesystem::path PickDisc(const std::filesystem::path& initial_dir, const wchar_t* title);
bool WriteShortcut(const std::filesystem::path& exe, const std::filesystem::path& shortcut);
void EnsureDesktopShortcut();

}  // namespace app::portable
