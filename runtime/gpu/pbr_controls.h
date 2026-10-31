#pragma once
#include "gpu/pbr_settings.h"

#include <memory>
#include <string>
#include <vector>

namespace gpu::pbr::controls {

// Facts supplied by actual renderer bindings, never inferred from an enabled preference or a pack.
struct Capabilities {
    bool pipeline = false, geometry = false, view = false, lighting = false;
    bool linear_output = false, material_bindings = false;
    bool parallax = false, displacement = false, tessellation = false, height_units = false;
};
inline bool ShadingReady(const Capabilities& c) {
    return c.pipeline&&c.geometry&&c.view&&c.lighting&&c.linear_output&&c.material_bindings;
}

enum class Reason {
    Disabled, InvalidSettings, LibraryUnavailable, UnapprovedManifest, NoAssignment, MaterialDisabled,
    UnsupportedDraw, MissingGeometry, MissingLinearLighting, AlphaMismatch, RendererBindingsUnavailable,
    MissingHeightMap, ParallaxUnavailable, DisplacementUnavailable, Ready
};
const char* ReasonText(Reason reason);
struct Resolution {
    const Material* material = nullptr; // Borrowed from the supplied Library; caller retains that library.
    Reason reason = Reason::Disabled;
    Quality quality = Quality::High;
    Parameters parameters;
    bool active() const { return material&&reason==Reason::Ready; }
};
// Renderer-owned immutable settings copy, rebuilt on preference changes rather than per draw.
// Bounded metadata preparation; no map loading/approval and no settings references retained.
class PreparedSettings {
public:
    explicit PreparedSettings(Settings settings);
    const Settings& settings() const { return settings_; }
    const MaterialOverride* Find(const Assignment& assignment,std::string_view manifest) const;
private:
    Settings settings_;
    std::vector<size_t> order_;
};
// No file I/O. Disabled returns before inspecting the library or allocating a material key.
// Library comes ONLY from LoadReviewedManifest; settings cannot certify its own manifest.
Resolution Resolve(const Settings& settings,const Library& accepted_library,const SurfaceEvidence& actual_draw,
                   const Capabilities& actual_bindings);
Resolution Resolve(const PreparedSettings& settings,const Library& accepted_library,const SurfaceEvidence& actual_draw,
                   const Capabilities& actual_bindings);

enum class RuntimeStatus { Disabled, WaitingForPack, Loading, PackRejected, BindingsUnavailable, Ready, Error };
const char* RuntimeStatusText(RuntimeStatus status);
struct CatalogMaterial {
    ExactMaterialKey key;
    bool has_height = false, has_emission = false;
    Reason reason = Reason::NoAssignment;
    uint64_t observed_draws = 0, rendered_draws = 0;
    bool ready = false; // Derived by publication, never a caller's assertion.
};
struct CatalogSnapshot {
    uint64_t revision = 0;
    RuntimeStatus status = RuntimeStatus::Disabled;
    std::string manifest_sha256, message;
    Capabilities capabilities;
    uint32_t ready_materials = 0; // Derived; positive only after actual rendered-draw observations.
    std::vector<CatalogMaterial> materials;
};
// Deep owned immutable snapshots: readers retain a stable copy while renderer publishes a later one.
// This is telemetry, not material approval. No maps/pointers to mutable libraries are published.
void PublishCatalog(CatalogSnapshot snapshot);
std::shared_ptr<const CatalogSnapshot> GetCatalog();

} // namespace gpu::pbr::controls
