// Real filesystem coverage of the production capture-session helper. No window, GPU or game is created.
#include "gpu/capture_directory.h"

#include <array>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <thread>

namespace fs = std::filesystem;
std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
void Write(const fs::path& path, const char* text) { std::ofstream(path, std::ios::binary) << text; }

int main() {
    const auto parent = fs::weakly_canonical(fs::current_path());
    const auto root = parent / ("capture-directory-fixture-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    assert(fs::create_directory(root));
    gpu::CaptureStamp stamp{123, 122, 1792968000123456789ull, 1234567890123ull, 9876};
    // Even identical timestamp/frame/PID inputs must allocate independent evidence sessions.
    const auto first = gpu::CreateUniqueCaptureDirectory(root / "same-second", stamp);
    assert(!first.error && first.collision_attempt == 0);
    std::error_code error;
    assert(gpu::WriteCaptureMetadata(first, stamp, error));
    const auto metadata = Read(first.path / "capture.json");
    assert(metadata.find("\"armed_frame\": 123") != std::string::npos);
    assert(metadata.find("\"armed_completed_presents\": 122") != std::string::npos);
    assert(metadata.find("1792968000123456789") != std::string::npos);
    assert(metadata.find("1234567890123") != std::string::npos);
    FILE* log = gpu::OpenCaptureFileNew(first.path / "frame.txt");
    assert(log); std::fputs("first frame", log); assert(std::fclose(log) == 0);
    Write(first.path / "old-texture.bmp", "first texture");
    const auto second = gpu::CreateUniqueCaptureDirectory(root / "same-second", stamp);
    assert(!second.error && second.path != first.path && second.collision_attempt == 1);
    assert(gpu::WriteCaptureMetadata(second, stamp, error));
    Write(second.path / "new-texture.bmp", "second texture");
    assert(!fs::exists(second.path / "frame.txt") && !fs::exists(second.path / "old-texture.bmp"));
    assert(!fs::exists(first.path / "new-texture.bmp"));
    assert(Read(first.path / "frame.txt") == "first frame");
    assert(Read(first.path / "capture.json") == metadata);
    assert(!gpu::OpenCaptureFileNew(first.path / "frame.txt"));
    assert(!gpu::WriteCaptureMetadata(first, stamp, error) && error);
    assert(Read(first.path / "frame.txt") == "first frame" && Read(first.path / "capture.json") == metadata);
    // Unknown directory/file collisions are preserved, including retry exhaustion.
    fs::create_directory(root / "foreign");
    const auto foreign = root / "foreign" / gpu::CaptureDirectoryName(stamp, 0);
    Write(foreign, "unrelated file at candidate name");
    const auto after_file = gpu::CreateUniqueCaptureDirectory(root / "foreign", stamp);
    assert(!after_file.error && after_file.collision_attempt == 1);
    assert(Read(foreign) == "unrelated file at candidate name");
    Write(after_file.path / "keep.txt", "foreign directory evidence");
    const auto exhausted = gpu::CreateUniqueCaptureDirectory(root / "foreign", stamp, 2);
    assert(exhausted.error == std::errc::file_exists && exhausted.path.empty());
    assert(Read(after_file.path / "keep.txt") == "foreign directory evidence");
    // Actual create failure does not truncate the obstructing parent file or return an active session.
    Write(root / "not-a-directory", "parent file must survive");
    const auto failed = gpu::CreateUniqueCaptureDirectory(root / "not-a-directory" / "child", stamp);
    assert(failed.error && failed.path.empty());
    assert(Read(root / "not-a-directory") == "parent file must survive");
    const auto invalid = gpu::CreateUniqueCaptureDirectory(root / "invalid", stamp, 0);
    assert(invalid.error == std::errc::invalid_argument && !fs::exists(root / "invalid"));
    // Atomic directory creation also separates simultaneous requests with identical IDs.
    std::array<gpu::CaptureDirectory, 8> parallel;
    std::array<std::thread, 8> threads;
    for (size_t i = 0; i < threads.size(); ++i)
        threads[i] = std::thread([&, i] { parallel[i] = gpu::CreateUniqueCaptureDirectory(root / "parallel", stamp); });
    for (auto& thread : threads) thread.join();
    std::set<fs::path> unique;
    for (const auto& item : parallel) { assert(!item.error && unique.insert(item.path).second); }
    // Remove only this fixture's verified absolute child directory under the CTest working directory.
    assert(fs::weakly_canonical(root).parent_path() == parent);
    assert(root.filename().string().starts_with("capture-directory-fixture-"));
    fs::remove_all(root);
    std::cout << "Capture directory isolation/exclusive files/metadata/failure tests passed\n";
}
