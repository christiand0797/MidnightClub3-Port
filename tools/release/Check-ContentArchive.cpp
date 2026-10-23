// Console-only interoperability check: use the native installer's actual manifest, chunk, path and stream reader.
// This never creates the setup window, downloads content, installs files, or starts the game.
#include "../../runtime/app/portable_content.cpp"
#include <iostream>

namespace logx { void Write(Level, const char*, ...) {} }

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::cerr << "Usage: Check-ContentArchive.exe <bundle-directory>\n"; return 2; }
    try {
        namespace fs = std::filesystem;
        using namespace app::portable;
        const fs::path folder = fs::absolute(argv[1]);
        const auto manifest = ParseManifest(Metadata(folder / L"content-manifest.tsv"));
        ChunkLocks locks(folder, manifest);
        for (const auto& chunk : manifest.chunks)
            Require(Matches(folder / chunk.name, chunk.size, chunk.hash), "Content chunk size or checksum failed.");
        Archive archive(folder, manifest);
        char magic[8]; archive.Read(magic, sizeof(magic));
        Require(std::string_view(magic, sizeof(magic)) == "MC3C0001", "Unknown content archive format.");
        const auto count = archive.Value<uint32_t>();
        Require(count && count <= 200000, "Invalid number of content files.");
        std::istringstream inventory(Metadata(folder / L"content-files.tsv", kInventoryLimit));
        std::string line;
        Require(bool(std::getline(inventory, line)) && line == "MC3-CONTENT-FILES\t1", "Invalid packaging inventory.");
        std::vector<char> buffer(1 << 20);
        std::set<std::wstring> paths;
        for (uint32_t index = 0; index < count; ++index) {
            const auto path_size = archive.Value<uint32_t>();
            const auto size = archive.Value<uint64_t>();
            Require(path_size && path_size <= 4096 && size <= kArchiveLimit, "Invalid content file header.");
            uint8_t digest[32]; archive.Read(digest, sizeof(digest));
            std::string name(path_size, '\0'); archive.Read(name.data(), name.size());
            auto path = ContentPath(folder / L"verify-only", name).wstring();
            std::transform(path.begin(), path.end(), path.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
            Require(paths.insert(path).second, "Duplicate content path.");
            Hash file_hash;
            for (uint64_t left = size; left;) {
                const auto bytes = size_t(std::min<uint64_t>(left, buffer.size()));
                archive.Read(buffer.data(), bytes); file_hash.Add(buffer.data(), bytes); left -= bytes;
            }
            const auto hash = Hex(digest, sizeof(digest));
            Require(file_hash.Finish() == hash, "Content file checksum failed.");
            Require(bool(std::getline(inventory, line)) && line == name + '\t' + std::to_string(size) + '\t' + hash,
                    "Archive does not match packaging inventory.");
        }
        Require(!std::getline(inventory, line), "Unexpected packaging inventory entries.");
        archive.Finish();
        std::cout << "Native archive reader PASS: " << count << " files, " << manifest.chunks.size()
                  << " chunks, " << manifest.size << " bytes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Archive check failed: " << error.what() << '\n';
        return 1;
    }
}
