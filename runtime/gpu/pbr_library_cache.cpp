#include "gpu/pbr_library_cache.h"
#include <exception>

namespace gpu::pbr {
LibraryCache::LibraryCache(Loader loader) : loader_(std::move(loader)) {}
LibraryCache::~LibraryCache() {
    { std::lock_guard lock(mutex_); stop_=true; pending_=false; }
    changed_.notify_all();
    if(worker_.joinable())worker_.join();
}
void LibraryCache::Request(bool enabled,const std::filesystem::path& root,std::string digest) {
    if(controls::ValidDigest(digest))for(char& c:digest)c=controls::LowerHex(c);
    std::lock_guard lock(mutex_);
    if(have_request_&&enabled==requested_enabled_&&root==request_.root&&digest==request_.digest)return;
    have_request_=true;requested_enabled_=enabled;
    request_={request_.revision+1,root,std::move(digest)};
    pending_=false;
    snapshot_={};snapshot_.revision=request_.revision;
    if(!enabled)return;
    if(root.empty()||!controls::ValidDigest(request_.digest)) {
        snapshot_.status=CacheStatus::WaitingForApproval;
        snapshot_.message="Select a reviewed pack and independently approved materials-v1.toml SHA256.";
        return;
    }
    snapshot_.status=CacheStatus::Loading;
    snapshot_.message="Checking exact manifest, assignments, map hashes and bounded decoded mips.";
    pending_=true;
    if(!worker_.joinable()) {
        try { worker_=std::thread([this]{Run();}); }
        catch(...) { pending_=false;snapshot_.status=CacheStatus::Rejected;snapshot_.message="Cannot start the owned CPU material loader.";return; }
    }
    changed_.notify_one();
}
LibrarySnapshot LibraryCache::Snapshot() const {std::lock_guard lock(mutex_);return snapshot_;}
void LibraryCache::Run() {
    for(;;) {
        RequestData next;
        {
            std::unique_lock lock(mutex_);
            changed_.wait(lock,[this]{return stop_||pending_;});
            if(stop_)return;
            next=request_;pending_=false;
        }
        std::shared_ptr<const Library> library;
        std::string error;
        try {
            LoadPolicy policy;policy.enabled=true;policy.approved_manifest_sha256=next.digest;
            auto loaded=loader_(next.root,policy);
            if(loaded.status!=LoadStatus::Ready||!controls::DigestEqual(loaded.manifest_sha256,next.digest)) {
                error=loaded.errors.empty()?"Reviewed material loading rejected the selected pack.":loaded.errors.front();
            } else library=std::make_shared<const Library>(std::move(loaded));
        } catch(const std::exception&) {error="Material loader failed; original shading remains active.";}
        catch(...) {error="Material loader failed; original shading remains active.";}
        {
            std::lock_guard lock(mutex_);
            if(stop_)return;
            if(next.revision!=request_.revision)continue;
            snapshot_.status=library?CacheStatus::Ready:CacheStatus::Rejected;
            snapshot_.library=std::move(library);
            snapshot_.message=std::move(error);
        }
    }
}
bool SameSettings(const controls::Settings& a,const controls::Settings& b) {
    if(a.enabled!=b.enabled||a.pack_path!=b.pack_path||a.approved_manifest_sha256!=b.approved_manifest_sha256||
       a.quality!=b.quality||a.parameters!=b.parameters||a.materials.size()!=b.materials.size()||
       a.materials.size()>controls::kMaxMaterials)return false;
    for(size_t i=0;i<a.materials.size();++i) {
        const auto& x=a.materials[i];const auto& y=b.materials[i];
        if(!controls::KeyEqual(x.key,y.key)||x.enabled!=y.enabled||x.use_global_parameters!=y.use_global_parameters||x.parameters!=y.parameters)return false;
    }
    return true;
}
} // namespace gpu::pbr
