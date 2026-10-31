#include "gpu/pbr_library_cache.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <future>

// The default argument's symbol is resolved here, but this test injects a bounded CPU loader.
namespace gpu::pbr { Library LoadReviewedManifest(const std::filesystem::path&,const LoadPolicy&){assert(false);return{};} }
using namespace gpu::pbr;
template<class Pred>void Wait(Pred pred) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    while(!pred()){assert(std::chrono::steady_clock::now()<end);std::this_thread::yield();}
}
int main() {
    std::atomic<int> calls=0;
    std::promise<void> entered,release;auto released=release.get_future().share();
    const std::string a(64,'a'),b(64,'b');
    LibraryCache cache([&](const std::filesystem::path& p,const LoadPolicy& policy){
        const int n=++calls;
        assert(policy.enabled&&policy.max_materials==4096&&policy.max_decoded_bytes==(1536ull<<20));
        if(n==1){entered.set_value();released.wait();assert(p=="old");}
        Library l;l.status=LoadStatus::Ready;l.manifest_sha256=policy.approved_manifest_sha256;return l;
    });
    cache.Request(false,"foreign/path",a);assert(cache.Snapshot().status==CacheStatus::Disabled&&calls==0);
    cache.Request(true,"",a);assert(cache.Snapshot().status==CacheStatus::WaitingForApproval&&calls==0);
    cache.Request(true,"old","invalid");assert(cache.Snapshot().status==CacheStatus::WaitingForApproval&&calls==0);
    cache.Request(true,"old",a);entered.get_future().wait();assert(calls==1);
    cache.Request(true,"intermediate",a);
    cache.Request(true,"new",b); // A single queue slot, no duplicate parallel CPU loader.
    release.set_value();Wait([&]{return cache.Snapshot().status==CacheStatus::Ready;});
    assert(calls==2&&cache.Snapshot().library->manifest_sha256==b);
    auto kept=cache.Snapshot().library;
    cache.Request(true,"new",b);assert(calls==2);
    cache.Request(false,"new",b);assert(cache.Snapshot().status==CacheStatus::Disabled&&!cache.Snapshot().library);
    assert(kept->manifest_sha256==b); // Readers own immutable old snapshots independently.
    LibraryCache wrong([](const auto&,const auto&){Library l;l.status=LoadStatus::Ready;l.manifest_sha256=std::string(64,'f');return l;});
    wrong.Request(true,"root",a);Wait([&]{return wrong.Snapshot().status==CacheStatus::Rejected;});
    assert(!wrong.Snapshot().library);
    controls::Settings s,t;assert(SameSettings(s,t));t.enabled=true;assert(!SameSettings(s,t));
    s=t;s.materials.push_back({controls::ExactKey({1,2,3,0,0,SurfaceClass::World,false},a)});t=s;assert(SameSettings(s,t));
    t.materials[0].key.assignment.vertex_shader=4;assert(!SameSettings(s,t));
    t=s;t.materials[0].enabled=false;assert(!SameSettings(s,t));
    t=s;t.materials[0].key.approved_manifest_sha256=b;assert(!SameSettings(s,t));
}
