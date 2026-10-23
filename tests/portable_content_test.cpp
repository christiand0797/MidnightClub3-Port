// Include the actual implementation to exercise its bounded parser and streaming extractor without networking.
#include "../runtime/app/portable_content.cpp"
#include <cassert>
#include <cstring>

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

    std::string archive = "MC3C0001"; Append(archive, uint32_t(2));
    Entry(archive, "game/game.iso", "synthetic fixture game data");
    Entry(archive, "video_hd/intro.mp4", "synthetic fixture video data");
    const auto manifest = Fixture(root, archive);
    const auto parsed = ParseManifest(ManifestText(manifest));
    assert(parsed.size == manifest.size && parsed.chunks.size() == manifest.chunks.size());
    assert(Throws([&] { ParseManifest(ManifestText(manifest) + "chunk\t../traversal.mc3c\t1\t" + manifest.hash + "\n"); }));
    assert(!Matches(root / manifest.chunks[0].name, manifest.chunks[0].size, std::string(64, '0')));
    assert(Matches(root / manifest.chunks[0].name, manifest.chunks[0].size, manifest.chunks[0].hash));
    const std::string text = ManifestText(manifest), text_hash = HashBytes(text);
    Write(root / "content-manifest.tsv", text);
    Write(root / "content-source.txt", "MC3-CONTENT-SOURCE\t1\nurl\thttps://github.com/example/mc3/releases/download/test/content-manifest.tsv\nsha256\t" + text_hash + "\nsize\t" + std::to_string(text.size()) + "\n");
    std::string error;
    assert(InstallBundledContent(root, error)); // all chunks are local; HTTPS is never contacted
    assert(FileHash(root / "game/game.iso") == HashBytes("synthetic fixture game data"));
    assert(FileHash(root / "video_hd/intro.mp4") == HashBytes("synthetic fixture video data"));
    assert(InstallBundledContent(root, error)); // completion marker: no reinstallation

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
    // Safe cleanup is bounded to the exact unique directory this test created.
    assert(fs::equivalent(root.parent_path(), temp) && root.filename().wstring().starts_with(L"mc3_content_test_"));
    fs::remove_all(root);
}
