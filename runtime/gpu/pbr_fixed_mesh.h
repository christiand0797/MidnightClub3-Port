#pragma once
// A bounded CPU bridge for an explicitly proved, ordinary fixed-function object-space draw.
// It neither approves a material nor creates resources, guesses normals or enables PBR/displacement.
// A stream without a normal is packed only under face_normals: shading then uses each triangle's own plane.
#include "gpu/pbr_pipeline.h"
#include "gpu/vertex.h"

#include <span>
#include <vector>

namespace gpu::pbr::fixed_mesh {

enum class DrawKind { Unproven, ObjectSpaceFixedFunction };
struct FixedProof {
    DrawKind kind = DrawKind::Unproven;
    bool authored_normals = false;
    bool assigned_uv = false;
    // Exactly one of authored_normals/face_normals. Face normals require register 2 to be ABSENT.
    bool face_normals = false;
    // Pack the actual register-3 diffuse into Vertex::color (baked scene mode).
    bool vertex_diffuse = false;
    // The actual stage-1 TFACTOR factor (r,g,b,a in [0,1]) multiplied into the packed diffuse; one when absent.
    std::array<float,4> diffuse_factor{1,1,1,1};
    // Whether the cascade's alpha reads the vertex diffuse alpha (else only the factor alpha is packed).
    bool alpha_from_diffuse = true;
    // The window layer's coordinate set (stage 1's texcoord index), decoded into Vertex::uv1; -1 without a layer.
    int32_t layer_uv = -1;
};
struct RasterTarget {
    uint32_t width = 0, height = 0, guest_width = 0, guest_height = 0;
    pipeline::TessellationPolicy tessellation_policy = pipeline::TessellationPolicy::UniformSharedEdges;
    float target_edge_pixels = 16;
};
struct Budget {
    uint32_t vertices = pipeline::kMaxMeshVertices, indices = pipeline::kMaxMeshIndices;
};
struct Mesh {
    std::vector<pipeline::Vertex> vertices;
    std::vector<uint32_t> indices;
    pipeline::GeometryConstants geometry;
    bool ready = false;
    pipeline::NormalSource normals = pipeline::NormalSource::Authored;
    // Every referenced vertex diffuse has alpha exactly one (only meaningful with vertex_diffuse).
    bool vertex_diffuse_alpha_one = false;
    // The actual view matrix preserves lengths. Distances are established draw-coordinate units, not metres.
    bool height_units_proven = false;
    // Packing a triangle list establishes none of the additional true-displacement obligations.
    bool shared_edges_and_uv_continuity_proven = false;
    bool displacement_bounds_and_culling_proven = false;
    bool displacement_depth_and_multipass_proven = false;
    bool displacement_budget_proven = false;
};
enum class Status {
    Ready, UnprovenDraw, UnsupportedDraw, MissingInput, UnsupportedFormat, InvalidBuffer,
    InvalidIndex, InvalidTransform, InvalidMesh, BudgetExceeded, AllocationFailure
};
const char* StatusText(Status);

// index_bytes is the exact bounded index upload for this assembled draw (empty for unindexed).
// Buffers and index_bytes must remain readable for the call. No pointers/spans survive in the result.
// The state/assembly/assignment/proof must all describe the same actual draw; proof is never inferred from
// a model name, texture name or the presence of a material pack. Failure empties the output atomically.
// A mesh Build returned for the same vertex/index bytes and proof, recomputed for this draw's state: the draw-state
// gate, transforms/constants and per-vertex projection checks run again; vertices and indices are reused. Failure
// empties the mesh. The caller owns proving the bytes are the same (a content hash of the exact spans).
Status Refresh(const State&, const Assembled&, const Assignment&, const FixedProof&, const RasterTarget&, Mesh&);
// detail, when given, receives the mesh validator's reason on InvalidMesh.
Status Build(const State&, const Assembled&, const Assignment&, const FixedProof&, const RasterTarget&,
             std::span<const uint8_t> index_bytes, Mesh&, Budget = {}, std::string* detail = nullptr);

} // namespace gpu::pbr::fixed_mesh
