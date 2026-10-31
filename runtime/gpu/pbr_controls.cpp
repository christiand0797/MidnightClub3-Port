#include "gpu/pbr_controls.h"

#include <atomic>
#include <numeric>
#include <utility>

namespace gpu::pbr::controls {
namespace {
std::atomic<std::shared_ptr<const CatalogSnapshot>> catalog;
Reason FromSelection(SelectionReason r) {
    switch(r) {
    case SelectionReason::Disabled:return Reason::LibraryUnavailable;
    case SelectionReason::NoAssignment:return Reason::NoAssignment;
    case SelectionReason::UnsupportedDraw:return Reason::UnsupportedDraw;
    case SelectionReason::MissingGeometry:return Reason::MissingGeometry;
    case SelectionReason::MissingLinearLighting:return Reason::MissingLinearLighting;
    case SelectionReason::AlphaMismatch:return Reason::AlphaMismatch;
    case SelectionReason::Ready:return Reason::Ready;
    }
    return Reason::LibraryUnavailable;
}
}
const char* ReasonText(Reason reason) {
    switch(reason) {
    case Reason::Disabled:return "PBR off; original rendering";
    case Reason::InvalidSettings:return "Select a valid pack path and reviewed manifest digest";
    case Reason::LibraryUnavailable:return "Reviewed material library unavailable";
    case Reason::UnapprovedManifest:return "Loaded library does not match the approved manifest";
    case Reason::NoAssignment:return "No exact reviewed texture/shader/stage/UV/surface assignment";
    case Reason::MaterialDisabled:return "PBR disabled for this exact material";
    case Reason::UnsupportedDraw:return "This draw must use original rendering";
    case Reason::MissingGeometry:return "Verified position, normal and tangent basis required";
    case Reason::MissingLinearLighting:return "Verified linear lighting required";
    case Reason::AlphaMismatch:return "Current accepted colour size/alpha proof does not match";
    case Reason::RendererBindingsUnavailable:return "Actual PBR pipeline/geometry/view/light/output/material bindings unavailable";
    case Reason::MissingHeightMap:return "Requested height mode needs a reviewed height map";
    case Reason::ParallaxUnavailable:return "Verified view/UV derivatives and height units required for parallax";
    case Reason::DisplacementUnavailable:return "True geometry displacement and tessellation bindings unavailable";
    case Reason::Ready:return "PBR bindings ready for this exact draw";
    }
    return "Unknown PBR state";
}
PreparedSettings::PreparedSettings(Settings settings):settings_(std::move(settings)) {
    Sanitize(settings_);order_.resize(settings_.materials.size());std::iota(order_.begin(),order_.end(),size_t(0));
    std::sort(order_.begin(),order_.end(),[&](size_t a,size_t b){return ExactMaterialKeyLess{}(settings_.materials[a].key,settings_.materials[b].key);});
}
const MaterialOverride* PreparedSettings::Find(const Assignment& assignment,std::string_view manifest) const {
    if(!ValidDigest(manifest)||!ValidAssignment(assignment))return nullptr;
    std::array<char,64> lower{};for(size_t i=0;i<64;++i)lower[i]=LowerHex(manifest[i]);
    const std::string_view digest(lower.data(),lower.size());
    size_t lo=0,hi=order_.size();
    while(lo<hi) {
        size_t mid=lo+(hi-lo)/2;const auto& key=settings_.materials[order_[mid]].key;
        bool less=AssignmentLess(key.assignment,assignment)||(!AssignmentLess(assignment,key.assignment)&&
            key.approved_manifest_sha256.compare(digest)<0);
        if(less)lo=mid+1;else hi=mid;
    }
    if(lo==order_.size())return nullptr;const auto& row=settings_.materials[order_[lo]];
    return AssignmentEqual(row.key.assignment,assignment)&&DigestEqual(row.key.approved_manifest_sha256,manifest)?&row:nullptr;
}
namespace {
Resolution ResolveImpl(const Settings& settings,const Library& library,const SurfaceEvidence& draw,const Capabilities& bindings,
                       const PreparedSettings* prepared) {
    Resolution out;
    if(!settings.enabled)return out; // No library inspection, string allocation, pack IO or approval side effect.
    out.reason=Reason::InvalidSettings;
    if(!ValidPackPath(settings.pack_path)||!ValidDigest(settings.approved_manifest_sha256))return out;
    out.reason=Reason::LibraryUnavailable;
    if(library.status!=LoadStatus::Ready||library.materials.size()>LoadPolicy{}.max_materials)return out;
    out.reason=Reason::UnapprovedManifest;
    if(!DigestEqual(library.manifest_sha256,settings.approved_manifest_sha256))return out;
    out.reason=Reason::NoAssignment;
    if(!ValidAssignment(draw.assignment))return out;
    const Material* exact=nullptr;
    for(const auto& m:library.materials)if(AssignmentEqual(m.assignment,draw.assignment)) {
        if(exact)return out; // Ambiguous forged/in-memory library never selects the first arbitrary match.
        exact=&m;
    }
    if(!exact)return out;
    // The loader enforces these shapes. Guard the public policy boundary before Select reads level zero.
    if(exact->albedo.levels.empty()||exact->normal.levels.empty()||exact->orm.levels.empty()) {
        out.reason=Reason::LibraryUnavailable;return out;
    }
    const auto* row=prepared?prepared->Find(draw.assignment,library.manifest_sha256):FindOverride(settings,draw.assignment,library.manifest_sha256);
    if(row&&!row->enabled){out.reason=Reason::MaterialDisabled;return out;}
    out.parameters=row&&!row->use_global_parameters?row->parameters:settings.parameters;Sanitize(out.parameters);
    out.quality=static_cast<uint32_t>(settings.quality)<=static_cast<uint32_t>(Quality::Ultra)?settings.quality:Quality::High;
    auto selected=Select(library,draw);out.reason=FromSelection(selected.reason);
    if(selected.reason!=SelectionReason::Ready||selected.material!=exact)return out;
    out.reason=Reason::RendererBindingsUnavailable;
    if(!ShadingReady(bindings))return out;
    auto mode=out.parameters.height_mode;
    if(mode!=HeightMode::None) {
        out.reason=Reason::MissingHeightMap;
        if(!exact->has_height||exact->height.levels.empty())return out;
        if(mode==HeightMode::Parallax) {
            out.reason=Reason::ParallaxUnavailable;if(!bindings.parallax||!bindings.height_units)return out;
        }else {
            out.reason=Reason::DisplacementUnavailable;
            if(!bindings.displacement||!bindings.tessellation||!bindings.height_units)return out;
        }
    }
    out.material=selected.material;out.reason=Reason::Ready;return out;
}
} // namespace
Resolution Resolve(const Settings& settings,const Library& library,const SurfaceEvidence& draw,const Capabilities& bindings) {
    return ResolveImpl(settings,library,draw,bindings,nullptr);
}
Resolution Resolve(const PreparedSettings& settings,const Library& library,const SurfaceEvidence& draw,const Capabilities& bindings) {
    return ResolveImpl(settings.settings(),library,draw,bindings,&settings);
}
const char* RuntimeStatusText(RuntimeStatus status) {
    switch(status) {
    case RuntimeStatus::Disabled:return "PBR off";
    case RuntimeStatus::WaitingForPack:return "Select an independently reviewed PBR pack";
    case RuntimeStatus::Loading:return "Preparing reviewed PBR materials";
    case RuntimeStatus::PackRejected:return "PBR pack rejected; original rendering retained";
    case RuntimeStatus::BindingsUnavailable:return "Waiting for verified PBR draw bindings";
    case RuntimeStatus::Ready:return "PBR rendered for verified exact materials";
    case RuntimeStatus::Error:return "PBR preparation failed; original rendering retained";
    }
    return "Unknown PBR status";
}
void PublishCatalog(CatalogSnapshot snapshot) {
    if(static_cast<uint32_t>(snapshot.status)>static_cast<uint32_t>(RuntimeStatus::Error))snapshot.status=RuntimeStatus::Error;
    snapshot.message.resize(std::min(snapshot.message.size(),size_t(2048)));
    snapshot.ready_materials=0;
    if(snapshot.materials.size()>kMaxMaterials)snapshot.materials.resize(kMaxMaterials);
    std::set<ExactMaterialKey,ExactMaterialKeyLess> seen;
    size_t kept=0;
    for(size_t i=0,n=snapshot.materials.size();i<n;++i) {
        auto row=std::move(snapshot.materials[i]);
        if(!ValidAssignment(row.key.assignment)||!DigestEqual(row.key.approved_manifest_sha256,snapshot.manifest_sha256))continue;
        for(char& c:row.key.approved_manifest_sha256)c=LowerHex(c);
        if(!seen.insert(row.key).second)continue;
        row.rendered_draws=std::min(row.rendered_draws,row.observed_draws);
        row.ready=row.reason==Reason::Ready&&row.observed_draws&&row.rendered_draws&&ShadingReady(snapshot.capabilities);
        if(!row.ready&&row.reason==Reason::Ready)row.reason=Reason::RendererBindingsUnavailable;
        if(row.ready)++snapshot.ready_materials;
        snapshot.materials[kept++]=std::move(row);
    }
    snapshot.materials.resize(kept);
    if(snapshot.status==RuntimeStatus::Ready&&!snapshot.ready_materials)snapshot.status=RuntimeStatus::BindingsUnavailable;
    // An inactive/loading/rejected state cannot advertise ready draws from stale prior-frame counters.
    if(snapshot.status!=RuntimeStatus::Ready) {
        snapshot.ready_materials=0;for(auto& row:snapshot.materials) {
            row.ready=false;
            if(row.reason==Reason::Ready)row.reason=snapshot.status==RuntimeStatus::Disabled?Reason::Disabled:Reason::RendererBindingsUnavailable;
        }
    }
    catalog.store(std::make_shared<const CatalogSnapshot>(std::move(snapshot)),std::memory_order_release);
}
std::shared_ptr<const CatalogSnapshot> GetCatalog() {
    auto value=catalog.load(std::memory_order_acquire);
    if(value)return value;
    static const auto initial=std::make_shared<const CatalogSnapshot>();return initial;
}
} // namespace gpu::pbr::controls
