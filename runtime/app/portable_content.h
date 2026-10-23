#pragma once
#include <filesystem>
#include <string>

namespace app::portable {
// Optional release-pinned content. The complete archive authenticates before publication; later launches verify
// the installed inventory and repair missing/corrupt media. Without content-source.txt own-ISO discovery remains.
bool InstallBundledContent(const std::filesystem::path& exe_dir, std::string& error);
}  // namespace app::portable
