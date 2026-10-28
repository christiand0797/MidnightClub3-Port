// CPU-only preflight for a PBR pack. Does not start a game, graphics device or asset conversion.
#include "gpu/pbr_material.h"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

namespace {
std::string Json(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 32) out << "\\u" << std::hex << std::setfill('0') << std::setw(4) << unsigned(c) << std::dec;
                else out << char(c);
        }
    }
    out << '"';
    return out.str();
}
}

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path root;
    std::string digest;
    for (int i = 1; i < argc; ++i) {
        const std::wstring option = argv[i];
        if (option == L"--root" && i + 1 < argc && root.empty()) root = argv[++i];
        else if (option == L"--manifest-sha256" && i + 1 < argc && digest.empty()) {
            const std::wstring text = argv[++i];
            if (text.size() != 64 || text.find_first_not_of(L"0123456789abcdefABCDEF") != std::wstring::npos) {
                std::cerr << "Expected a 64-digit manifest SHA256.\n";
                return 2;
            }
            digest.assign(text.begin(), text.end());
        } else {
            std::cerr << "Usage: Check-PbrMaterials --root PACK --manifest-sha256 SHA256\n";
            return 2;
        }
    }
    if (root.empty() || digest.empty()) {
        std::cerr << "Usage: Check-PbrMaterials --root PACK --manifest-sha256 SHA256\n";
        return 2;
    }
    gpu::pbr::LoadPolicy policy;
    policy.enabled = true;
    policy.approved_manifest_sha256 = digest;
    const auto library = gpu::pbr::LoadReviewedManifest(root, policy);
    const bool ready = library.status == gpu::pbr::LoadStatus::Ready;
    std::cout << "{\"ready\":" << (ready ? "true" : "false")
              << ",\"manifest_sha256\":" << Json(library.manifest_sha256)
              << ",\"materials\":" << library.materials.size() << ",\"errors\":[";
    for (size_t i = 0; i < library.errors.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << Json(library.errors[i]);
    }
    std::cout << "],\"renderer_activated\":false,\"assets_generated\":false}\n";
    return ready ? 0 : 3;
}
