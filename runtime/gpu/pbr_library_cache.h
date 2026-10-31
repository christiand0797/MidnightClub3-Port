#pragma once
// One owned CPU loader. No directory scanning, filename adoption, GPU work or detached workers.
#include "gpu/pbr_controls.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace gpu::pbr {

enum class CacheStatus { Disabled, WaitingForApproval, Loading, Rejected, Ready };
struct LibrarySnapshot {
    CacheStatus status = CacheStatus::Disabled;
    uint64_t revision = 0;
    std::shared_ptr<const Library> library;
    std::string message;
};
class LibraryCache {
public:
    using Loader = std::function<Library(const std::filesystem::path&, const LoadPolicy&)>;
    explicit LibraryCache(Loader loader = LoadReviewedManifest);
    ~LibraryCache();
    LibraryCache(const LibraryCache&) = delete;
    LibraryCache& operator=(const LibraryCache&) = delete;
    // Disabled/invalid returns without starting a thread or touching the filesystem. A pending request
    // replaces only the bounded one-slot queue. An in-progress older result can never become current.
    void Request(bool enabled, const std::filesystem::path& root, std::string approved_digest);
    LibrarySnapshot Snapshot() const;
private:
    struct RequestData { uint64_t revision=0; std::filesystem::path root; std::string digest; };
    void Run();
    Loader loader_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::thread worker_;
    bool stop_=false, pending_=false, have_request_=false, requested_enabled_=false;
    RequestData request_;
    LibrarySnapshot snapshot_;
};

// Exact bounded settings comparison for a render-thread cached PreparedSettings. No material key can
// accidentally inherit another texture/VS/PS/stage/UV/surface/cutout or another approved pack's edits.
bool SameSettings(const controls::Settings&,const controls::Settings&);

} // namespace gpu::pbr
