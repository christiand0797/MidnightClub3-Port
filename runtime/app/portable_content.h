#pragma once
#include <filesystem>
#include <string>

namespace app::portable {
// Optional release-pinned content. Without content-source.txt this is a no-op, leaving own-ISO discovery intact.
bool InstallBundledContent(const std::filesystem::path& exe_dir, std::string& error);
}  // namespace app::portable
