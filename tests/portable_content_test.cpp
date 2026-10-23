// Include the actual implementation to exercise its bounded parser and streaming extractor without networking.
#include "../runtime/app/portable_content.cpp"
#include <cassert>
#include <cstring>
#include <thread>

namespace logx { void Write(Level, const char*, ...) {} }
namespace {
namespace fs = std::filesystem;
using namespace app::portable;
void Write(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary); file.write(bytes.data(), bytes.size()); assert(bool(file));
}
std::string HashBytes(const std::string& bytes) { Hash hash; hash.Add(bytes.data(), bytes.size()); return hash.Finish(); }
template<class T> void Append(std::string& bytes, T value) { bytes.append(reinterpret_cast<const char*>(&value), sizeof(value)); }
void Entry(std::string& archive, const std::string& path, const std::string& data, bool wrong_hash = false) {
    Append(archive, uint32_t(path.size())); Append(archive, uint64_t(data.size()));
    const auto hash = HashBytes(wrong_hash ? "wrong bytes" : data);
    for (size_t i = 0; i < hash.size(); i += 2) archive += char(std::stoi(hash.substr(i, 2), nullptr, 16));
    archive += path; archive += data;
}
template<class F> bool Throws(F operation) { try { operation(); return false; } catch (const std::exception&) { return true; } }
Manifest Fixture(const fs::path& folder, const std::string& archive, size_t split = 37) {
    Manifest manifest; manifest.size = archive.size(); manifest.hash = HashBytes(archive);
    for (size_t at = 0; at < archive.size(); at += split) {
        const std::string bytes = archive.substr(at, split);
        const std::string name = "fixture-" + std::to_string(manifest.chunks.size()) + ".mc3c";
        Write(folder / name, bytes); manifest.chunks.push_back({name, HashBytes(bytes), bytes.size()});
    }
    return manifest;
}
std::string ManifestText(const Manifest& manifest) {
    std::string text = "MC3-CONTENT\t1\narchive\t" + std::to_string(manifest.size) + "\t" + manifest.hash + "\n";
    for (const auto& chunk : manifest.chunks) text += "chunk\t" + chunk.name + "\t" + std::to_string(chunk.size) + "\t" + chunk.hash + "\n";
    return text;
}
}  // namespace

int main() {
    namespace fs = std::filesystem;
    using namespace app::portable;
    const fs::path temp = fs::absolute(fs::temp_directory_path());
    const fs::path root = temp / ("mc3_content_test_" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
    assert(fs::equivalent(root.parent_path(), temp) && fs::create_directory(root));
    assert(Throws([&] { ContentPath(root, "../outside.iso"); }));
    assert(Throws([&] { ContentPath(root, "game/../outside.iso"); }));
    assert(Throws([&] { ContentPath(root, "game/C:/outside.iso"); }));
    assert(Throws([&] { ContentPath(root, "game/evil.iso:stream"); }));
    assert(Throws([&] { ContentPath(root, "video_hd/../../outside.mp4"); }));
    assert(Throws([&] { ContentPath(root, "video_hd/./intro.mp4"); }));
    assert(Throws([&] { ContentPath(root, "hdd/player.save"); }));
    assert(Throws([&] { ContentPath(root, "config/settings.toml"); }));
    assert(Throws([&] { ContentPath(root, "/game/game.iso"); }));
    assert(Throws([&] { ContentPath(root, "video_hd/bad /intro.mp4"); }));
    assert(Throws([&] { ContentPath(root, "game/CON.iso"); }));
    assert(Throws([&] { ContentPath(root, "video_hd//intro.mp4"); }));
    assert(Throws([&] { ParseManifest("MC3-CONTENT\t2\n"); }));

    std::string archive = "MC3C0001"; Append(archive, uint32_t(3));
    Entry(archive, "game/game.iso", "synthetic fixture game data");
    Entry(archive, "video_hd/intro.mp4", "synthetic fixture video data");
    Entry(archive, "textures/replace/fixture.png", "synthetic fixture texture data");
    const auto manifest = Fixture(root, archive);
    const auto parsed = ParseManifest(ManifestText(manifest));
    assert(parsed.size == manifest.size && parsed.chunks.size() == manifest.chunks.size());
    assert(Throws([&] { ParseManifest(ManifestText(manifest) + "chunk\t../traversal.mc3c\t1\t" + manifest.hash + "\n"); }));
    assert(!Matches(root / manifest.chunks[0].name, manifest.chunks[0].size, std::string(64, '0')));
    assert(Matches(root / manifest.chunks[0].name, manifest.chunks[0].size, manifest.chunks[0].hash));
    {
        ChunkLocks locks(root, manifest);
        HANDLE writer = CreateFileW((root / manifest.chunks[0].name).c_str(), GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        assert(writer == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION);
    }
    const std::string text = ManifestText(manifest), text_hash = HashBytes(text);
    Write(root / "content-manifest.tsv", text);
    Write(root / "content-source.txt", "MC3-CONTENT-SOURCE\t1\nurl\thttps://github.com/example/mc3/releases/download/test/content-manifest.tsv\nsha256\t" + text_hash + "\nsize\t" + std::to_string(text.size()) + "\n");
    std::string error;
    assert(InstallBundledContent(root, error)); // all chunks are local; HTTPS is never contacted
    assert(FileHash(root / "game/game.iso") == HashBytes("synthetic fixture game data"));
    assert(FileHash(root / "video_hd/intro.mp4") == HashBytes("synthetic fixture video data"));
    assert(fs::is_regular_file(root / "config/content-inventory.tsv"));
    assert(Metadata(root / "config/content-installed.txt").starts_with("MC3-CONTENT-INSTALLED\t2\n"));
    assert(InstallBundledContent(root, error)); // every installed file is authenticated before the early return

    // A surviving ISO/marker cannot hide missing video or same-sized corrupt texture bytes. Valid offline
    // chunks remain beside the exe and repair both failures without contacting HTTPS.
    assert(fs::remove(root / "video_hd/intro.mp4"));
    assert(InstallBundledContent(root, error));
    assert(FileHash(root / "video_hd/intro.mp4") == HashBytes("synthetic fixture video data"));
    Write(root / "textures/replace/fixture.png", std::string(std::strlen("synthetic fixture texture data"), 'X'));
    assert(InstallBundledContent(root, error));
    assert(FileHash(root / "textures/replace/fixture.png") == HashBytes("synthetic fixture texture data"));

    // Once installed, launch stays offline even when no archive chunks remain available. Stale owned download
    // partials are cleared on this success path, including when an offline complete part won over an old .part.
    for (const auto& chunk : manifest.chunks) assert(fs::remove(root / chunk.name));
    Write(root / (manifest.chunks[0].name + ".part"), "stale offline partial");
    Write(root / ".content-cache" / (manifest.chunks[0].name + ".part"), "stale cached partial");
    Write(root / "content-manifest.tsv.part", "stale manifest partial");
    assert(InstallBundledContent(root, error));
    assert(!fs::exists(root / (manifest.chunks[0].name + ".part")));
    assert(!fs::exists(root / ".content-cache" / (manifest.chunks[0].name + ".part")));
    assert(!fs::exists(root / "content-manifest.tsv.part"));
    Fixture(root, archive);

    // A second first-launch process/thread cannot race the same *.part/*.installing files. The installer itself
    // takes this OS mutex before opening its progress window or mutating the cache.
    {
        InstallLock held(root);
        bool rejected = false;
        std::thread second([&] {
            std::string concurrent_error;
            rejected = !InstallBundledContent(root, concurrent_error) && concurrent_error.find("already running") != std::string::npos;
        });
        second.join(); assert(rejected);
        rejected = false;
        const fs::path extended_root(L"\\\\?\\" + root.wstring());
        std::thread alias([&] {
            std::string concurrent_error;
            rejected = !InstallBundledContent(extended_root, concurrent_error) && concurrent_error.find("already running") != std::string::npos;
        });
        alias.join(); assert(rejected); // an extended-path spelling must not evade the same directory lock
    }

    // Legacy or corrupted inventory metadata is rebuilt from the pinned archive rather than silently trusted.
    Write(root / "config/content-installed.txt", text_hash);
    assert(InstallBundledContent(root, error));
    Write(root / "config/content-inventory.tsv", "damaged inventory");
    assert(InstallBundledContent(root, error));
    assert(Metadata(root / "config/content-inventory.tsv").starts_with("MC3-CONTENT-INVENTORY\t1\n"));

    // All entry hashes can be valid while the signed whole-archive digest is wrong. Nothing already working
    // may be replaced, even the first entry, before the complete archive has authenticated.
    std::string wrong_whole = "MC3C0001"; Append(wrong_whole, uint32_t(2));
    Entry(wrong_whole, "game/game.iso", "a different game payload");
    Entry(wrong_whole, "video_hd/intro.mp4", "a different movie payload");
    auto wrong_whole_manifest = Fixture(root / "whole-bad-cache", wrong_whole);
    wrong_whole_manifest.hash = std::string(64, '0');
    {
        Progress progress;
        assert(Throws([&] { Extract(root, root / "whole-bad-cache", wrong_whole_manifest, progress); }));
        assert(FileHash(root / "game/game.iso") == HashBytes("synthetic fixture game data"));
        assert(FileHash(root / "video_hd/intro.mp4") == HashBytes("synthetic fixture video data"));
        assert(!fs::exists(root / "game/game.iso.installing"));
    }

    // A bad later per-file hash also leaves the earlier working entry untouched despite a correct archive hash.
    std::string late_bad = "MC3C0001"; Append(late_bad, uint32_t(2));
    Entry(late_bad, "game/game.iso", "a different game payload");
    Entry(late_bad, "video_hd/intro.mp4", "bad late replacement", true);
    auto late_manifest = Fixture(root / "late-bad-cache", late_bad);
    {
        Progress progress;
        assert(Throws([&] { Extract(root, root / "late-bad-cache", late_manifest, progress); }));
        assert(FileHash(root / "game/game.iso") == HashBytes("synthetic fixture game data"));
        assert(FileHash(root / "video_hd/intro.mp4") == HashBytes("synthetic fixture video data"));
    }

    // A file hash failure must preserve an already working installed file.
    std::string bad_archive = "MC3C0001"; Append(bad_archive, uint32_t(1));
    Entry(bad_archive, "video_hd/intro.mp4", "bad replacement", true);
    const auto bad_manifest = Fixture(root / "bad-cache", bad_archive);
    {
        Progress progress;
        assert(Throws([&] { Extract(root, root / "bad-cache", bad_manifest, progress); }));
        assert(FileHash(root / "video_hd/intro.mp4") == HashBytes("synthetic fixture video data"));
    }
    // A truncated part stops before publishing its incomplete destination.
    const auto truncated = Fixture(root / "truncated-cache", archive, archive.size());
    Write(root / "truncated-cache" / truncated.chunks[0].name, archive.substr(0, 70));
    {
        Progress progress;
        assert(Throws([&] { Extract(root / "truncated-output", root / "truncated-cache", truncated, progress); }));
        assert(!fs::exists(root / "truncated-output/game/game.iso"));
    }
    // The portable executable may live beyond MAX_PATH under a non-ASCII Windows username. Extended paths
    // reject embedded forward slashes; archive-relative paths and config constants must use native separators.
    const fs::path long_folder = fs::path(L"\\\\?\\" + root.wstring()) / std::wstring(190, L'x') / L"\u6e38\u620f\u30c6\u30b9\u30c8";
    assert(long_folder.wstring().size() > 260);
    const auto long_manifest = Fixture(long_folder, archive);
    const std::string long_text = ManifestText(long_manifest), long_hash = HashBytes(long_text);
    Write(long_folder / "content-manifest.tsv", long_text);
    Write(long_folder / "content-source.txt", "MC3-CONTENT-SOURCE\t1\nurl\thttps://github.com/example/mc3/releases/download/test/content-manifest.tsv\nsha256\t" + long_hash + "\nsize\t" + std::to_string(long_text.size()) + "\n");
    assert(InstallBundledContent(long_folder, error));
    assert(FileHash(ContentPath(long_folder, "video_hd/intro.mp4")) == HashBytes("synthetic fixture video data"));
    assert(InstallBundledContent(long_folder, error));
    // Safe cleanup is bounded to the exact unique directory this test created.
    assert(fs::equivalent(root.parent_path(), temp) && root.filename().wstring().starts_with(L"mc3_content_test_"));
    fs::remove_all(fs::path(L"\\\\?\\" + root.wstring()));
}
