// Execute the original chassis-floor classifier, writer/continuation, polygon rebuild and bound setter.
// The shapes are explicit synthetic fixtures, not decoded car assets or a simulated road/contact solver.
#include "cpu/x86_ops.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

#undef X86_FUNC
#undef X86_WEAK_FUNC
#define X86_FUNC(name) static void name(X86Context& __restrict ctx, uint8_t* base)
#define X86_WEAK_FUNC(name, impl)
#include "x86_recomp.16.cpp"
#include "x86_recomp.32.cpp"
#include "x86_recomp.47.cpp"
#include "x86_recomp.64.cpp"
#include "x86_recomp.78.cpp"

namespace {
std::vector<uint8_t> guest(0x600000);
constexpr uint32_t kBody = 0x10000, kSim = 0x11000, kInstance = 0x12000;
constexpr uint32_t kLevel = 0x13000, kArchetype = 0x14000, kBound = 0x15000;
constexpr uint32_t kVertices = 0x16000, kPolygons = 0x17000, kVtable = 0x18000;
constexpr uint32_t kOtherBody = 0x20000, kOtherInstance = 0x21000, kOtherLevel = 0x22000;
constexpr uint32_t kOtherArchetype = 0x23000, kStack = 0x70000, kRebuildTarget = 0xB001;
unsigned continuations = 0, rebuilds = 0, polygons = 0, virtual_rebuilds = 0;

void W(uint32_t p, uint32_t v) { std::memcpy(guest.data() + p, &v, 4); }
uint32_t W(uint32_t p) { uint32_t v; std::memcpy(&v, guest.data() + p, 4); return v; }
void H(uint32_t p, uint16_t v) { std::memcpy(guest.data() + p, &v, 2); }
void F(uint32_t p, float v) { std::memcpy(guest.data() + p, &v, 4); }
float F(uint32_t p) { float v; std::memcpy(&v, guest.data() + p, 4); return v; }
bool Near(float a, float b, float tolerance = 0.00002f) { return std::abs(a - b) < tolerance; }
uint32_t Bits(float v) { uint32_t b; std::memcpy(&b, &v, 4); return b; }

using Vertex = std::array<float, 3>;
constexpr std::array<Vertex, 8> kShape = {{{-.8f, -.4f, -2}, {.8f, -.4f, -2},
    {.8f, -.4f, 2}, {-.8f, -.4f, 2}, {-.9f, .2f, -2}, {1.1f, .2f, -2},
    {1.1f, .2f, 2}, {-.9f, .2f, 2}}};

void Polygon(uint32_t p, std::array<uint16_t, 4> indices) {
    for (unsigned i = 0; i < indices.size(); ++i) H(p + 0x10 + 2 * i, indices[i]);
    guest[p + 0xC] = 0x5A; // Material byte overlays the stored area; the native updater must retain it.
}

void Setup(const std::vector<Vertex>& vertices = {kShape.begin(), kShape.end()}, unsigned polygon_count = 2) {
    std::fill(guest.begin(), guest.end(), 0);
    continuations = rebuilds = polygons = virtual_rebuilds = 0;
    W(kBody + 4, kInstance); W(kInstance + 0x1C, kLevel);
    W(kLevel + 4, kArchetype); W(kArchetype + 0xC, kBound);
    W(kBody + 0xB4, kSim);
    W(kBound, kVtable); W(kVtable + 0xB0, kRebuildTarget);
    W(kBound + 0x4C, static_cast<uint32_t>(vertices.size()));
    W(kBound + 0x50, polygon_count); W(kBound + 0x68, kVertices); W(kBound + 0x6C, kPolygons);
    std::memcpy(guest.data() + kVertices, vertices.data(), vertices.size() * sizeof(Vertex));
    Polygon(kPolygons, {0, 1, 2, 3});
    Polygon(kPolygons + 0x20, {0, 4, 5, 1});
    for (unsigned i = 0; i < 4; ++i) W(kBody + 0x190 + 4 * i, UINT32_MAX);
    // Actual XBE constants read by 121150/1A5730. The constructor's normal skin/taper are explicit inputs.
    F(0x448DA0, 1000); F(0x44AAEC, .05f); F(0x447C18, .5f); F(0x447C1C, 1);
    F(0x44CDCC, .00001f); F(0x44EDCC, .0000001f); F(0x44949C, 10);
    F(kSim + 0xD8, .05f); F(kSim + 0xDC, 0);
}

void Invoke(X86Func* fn, uint32_t self, std::initializer_list<uint32_t> args = {}) {
    X86Context c{};
    c.ecx = self; c.esp = kStack;
    c.ebx = 0x11223344; c.ebp = 0x22334455; c.esi = 0x33445566; c.edi = 0x44556677;
    c.top = 3; c.st[c.top] = 123.5; c.fcw = 0x37F;
    W(kStack, 0x10570C); W(kStack + 4 + 4 * static_cast<uint32_t>(args.size()), 0xC0FFEE);
    unsigned i = 1;
    for (uint32_t v : args) W(kStack + 4 * i++, v);
    fn(c, guest.data());
    assert(c.esp == kStack + 4 * i);
    assert(c.ebx == 0x11223344 && c.ebp == 0x22334455 && c.esi == 0x33445566 && c.edi == 0x44556677);
    assert(c.top == 3 && c.st[c.top] == 123.5);
    assert(W(kStack + 4 + 4 * static_cast<uint32_t>(args.size())) == 0xC0FFEE);
}

void Classify(uint32_t body = kBody) { Invoke(__imp__sub_00121150, body); }
void Floor(float argument, uint32_t body = kBody) { Invoke(__imp__sub_00121440, body, {Bits(argument)}); }

void CheckFloor(float y, float half_width = 1.1f) {
    for (unsigned i = 0; i < 4; ++i) {
        assert(Near(F(kVertices + 12 * i), i == 0 || i == 3 ? -half_width : half_width));
        assert(Near(F(kVertices + 12 * i + 4), y));
        assert(Near(F(kVertices + 12 * i + 8), kShape[i][2]));
    }
    assert(std::memcmp(guest.data() + kVertices + 4 * sizeof(Vertex), kShape.data() + 4, 4 * sizeof(Vertex)) == 0);
    assert(Near(F(kPolygons), 0) && Near(F(kPolygons + 4), -1) && Near(F(kPolygons + 8), 0));
    // Native storage replaces the low float byte with material: at this area (<16 m^2), the lost precision
    // is at most 255 float ULPs, or .000244 m^2. Check the area without rejecting its original packing.
    assert(Near(F(kPolygons + 0xC), 8 * half_width, .00025f));
    assert(guest[kPolygons + 0xC] == 0x5A && guest[kPolygons + 0x2C] == 0x5A);
    assert(continuations == rebuilds && virtual_rebuilds == rebuilds && polygons == 2 * rebuilds);
}

void BasicAndCandidate() {
    Setup(); Classify();
    for (unsigned i = 0; i < 4; ++i) assert(W(kBody + 0x190 + 4 * i) == i);
    assert(Near(F(kBody + 0x1A0), -.4f));
    assert(Near(F(kBody + 0x1A4), .15f));
    assert(Near(F(kBody + 0x1A8), 1.1f));
    const auto cache = std::vector<uint8_t>(guest.begin() + kBody + 0x190, guest.begin() + kBody + 0x1AC);
    const auto tune = std::vector<uint8_t>(guest.begin() + kSim, guest.begin() + kSim + 0xE0);
    Floor(0); CheckFloor(-.35f);
    const auto stock = std::vector<uint8_t>(guest.begin() + kVertices, guest.begin() + kVertices + sizeof(kShape));
    for (const auto heights : {std::array{-.1f, -.1f}, std::array{-.35f, -.35f},
                               std::array{-.35f, 0.0f}, std::array{0.0f, -.35f}}) {
        const float mean = (heights[0] + heights[1]) * .5f;
        // These are proposed arguments only. No production wrapper/policy is included in this test.
        Floor(std::max(0.0f, -mean)); CheckFloor(-.35f - mean);
        Floor(0); CheckFloor(-.35f);
        assert(std::memcmp(stock.data(), guest.data() + kVertices, stock.size()) == 0);
    }
    assert(std::memcmp(cache.data(), guest.data() + kBody + 0x190, cache.size()) == 0);
    const auto shape_and_polygons = std::vector<uint8_t>(guest.begin() + kVertices, guest.begin() + kPolygons + 0x40);
    Floor(0); CheckFloor(-.35f);
    assert(std::memcmp(shape_and_polygons.data(), guest.data() + kVertices, shape_and_polygons.size()) == 0);
    Floor(.9f); CheckFloor(.15f); // The next-layer cap, not the requested argument, wins.
    Floor(.08f); CheckFloor(-.27f); // A smaller current tire retains radius compensation in the candidate.
    Floor(0); CheckFloor(-.35f); // A larger current tire cannot request a negative compensation.
    assert(std::memcmp(tune.data(), guest.data() + kSim, tune.size()) == 0);

    struct Case { float stock_radius, current_radius, mean_ride, original, corrected; };
    for (const Case c : {Case{.35f, .35f, 0, 0, 0}, Case{.35f, .35f, -.35f, .35f, 0},
                        Case{.35f, .27f, -.35f, .43f, .08f}, Case{.35f, .43f, -.35f, .27f, 0},
                        Case{.35f, .27f, .04f, .04f, .04f}, Case{.35f, .27f, .12f, 0, 0}}) {
        const float original = std::max(0.0f, c.stock_radius - c.current_radius - c.mean_ride);
        const float candidate = std::max(0.0f, c.stock_radius - c.current_radius - std::max(0.0f, c.mean_ride));
        assert(Near(original, c.original) && Near(candidate, c.corrected));
        Floor(original); CheckFloor(std::min(-.35f + c.original, .15f));
        Floor(candidate); CheckFloor(std::min(-.35f + c.corrected, .15f));
    }
}

void TaperAndSparseLayers() {
    Setup(); Classify(); F(kSim + 0xDC, .2f);
    Floor(0); CheckFloor(-.35f, 1.1f - std::tan(.2f) * .5f);
    Floor(.9f); CheckFloor(.15f); // At the cap, taper is exactly zero.
    Setup({kShape[0], kShape[1], kShape[4], kShape[5], kShape[6], kShape[7]}, 0);
    Classify();
    assert(W(kBody + 0x190) == 0 && W(kBody + 0x194) == 1);
    assert(W(kBody + 0x198) == UINT32_MAX && W(kBody + 0x19C) == UINT32_MAX);
    const auto upper = std::vector<uint8_t>(guest.begin() + kVertices + 24, guest.begin() + kVertices + 72);
    Floor(.2f);
    assert(Near(F(kVertices + 4), -.15f) && Near(F(kVertices + 16), -.15f));
    assert(std::memcmp(upper.data(), guest.data() + kVertices + 24, upper.size()) == 0);
    assert(continuations == 1 && rebuilds == 1 && virtual_rebuilds == 1 && polygons == 0);
    // Seven vertices exercise 121150's scalar remainder as well as its unrolled four-vertex loop.
    Setup({kShape[0], kShape[4], kShape[1], kShape[5], kShape[6], kShape[7], kShape[2]}, 0);
    Classify();
    assert(W(kBody + 0x190) == 0 && W(kBody + 0x194) == 2 && W(kBody + 0x198) == 6);
    assert(W(kBody + 0x19C) == UINT32_MAX);
    Floor(.1f);
    for (unsigned i : {0u, 2u, 6u}) assert(Near(F(kVertices + 12 * i + 4), -.25f));
}

void AuthoredClearanceAndExtremeRadii() {
    // Original F355 base/mods author 8 cm, SLR base/mods 9 cm. Most other tunes use the constructor's
    // 5 cm default or author 5 cm. These values remain inputs; the floor correction must not flatten them.
    // The geometry here remains the explicit synthetic shape, not either car's visible floorpan.
    for (float skin : {.05f, .08f, .09f}) {
        Setup(); Classify(); F(kSim+0xD8,skin);
        const auto cached=std::vector<uint8_t>(guest.begin()+kBody+0x190,guest.begin()+kBody+0x1AC);
        for (const auto heights : {std::array{-.125f,-.15f}, std::array{-.1375f,-.125f},
                                   std::array{-.15f,-.1375f}, std::array{-.35f,0.0f}}) {
            const float mean=(heights[0]+heights[1])*.5f;
            for(float radius:{(.38100001215934753f/.975f)*.5f,.35f,(.7874000072479248f/.3f)*.5f}) {
                // Same argument transform as guarded 121440, followed here by the real original writer.
                // A small tire intentionally retains stock-minus-current compensation; the cap still wins.
                const float original=std::max(0.0f,.35f-radius-mean);
                const float corrected=std::max(0.0f,original+mean);
                Floor(corrected); CheckFloor(std::min(-.4f+skin+corrected,.15f));
                assert(F(kSim+0xD8)==skin);
                assert(std::memcmp(cached.data(),guest.data()+kBody+0x190,cached.size())==0);
            }
        }
    }
}

void TypeTwoAndAliasWitness() {
    Setup(); W(kBody + 0x18, 2);
    const auto before = guest;
    Classify(); Floor(.35f);
    assert(std::memcmp(before.data() + kBody + 0x190, guest.data() + kBody + 0x190, 0x1C) == 0);
    assert(std::memcmp(before.data() + kVertices, guest.data() + kVertices, sizeof(kShape)) == 0);
    assert(continuations == 0 && rebuilds == 0 && polygons == 0 && virtual_rebuilds == 0);

    Setup();
    // Actual 2A4BB0 retains the supplied pointer; it does not create a unique shape. This deliberately shared
    // fixture demonstrates the hazard, not that real same-model cars necessarily share their resources.
    W(kArchetype + 0xC, 0); guest[0x51BB9C] = 1;
    Invoke(__imp__sub_002A4BB0, kArchetype, {kBound});
    Invoke(__imp__sub_002A4BB0, kOtherArchetype, {kBound});
    assert(W(kArchetype + 0xC) == kBound && W(kOtherArchetype + 0xC) == kBound);
    assert(W(kBound + 0x48) == 2);
    W(kOtherBody + 4, kOtherInstance); W(kOtherInstance + 0x1C, kOtherLevel);
    W(kOtherLevel + 4, kOtherArchetype); W(kOtherBody + 0xB4, kSim);
    for (unsigned i = 0; i < 4; ++i) W(kOtherBody + 0x190 + 4 * i, UINT32_MAX);
    Classify(); Classify(kOtherBody);
    Floor(.35f); CheckFloor(0);
    // Updating only body A has already changed the geometry reached by body B's original getter.
    X86Context c{}; c.ecx = kOtherBody; c.esp = kStack;
    __imp__sub_000A2450(c, guest.data());
    assert(c.eax == kBound && W(c.eax + 0x68) == kVertices && Near(F(W(c.eax + 0x68) + 4), 0));
    Floor(0, kOtherBody); CheckFloor(-.35f);
}
} // namespace

extern "C" void sub_000A2450(X86Context& c, uint8_t* b) { __imp__sub_000A2450(c, b); }
extern "C" void sub_001214F0(X86Context& c, uint8_t* b) { ++continuations; __imp__sub_001214F0(c, b); }
extern "C" void sub_002395F0(X86Context& c, uint8_t* b) { ++rebuilds; __imp__sub_002395F0(c, b); }
extern "C" void sub_001A5730(X86Context& c, uint8_t* b) { ++polygons; __imp__sub_001A5730(c, b); }
extern "C" void sub_00233DB0(X86Context&, uint8_t*) {
    std::cerr << "unexpected old-bound release; fixture only attaches initially empty archetypes\n"; std::abort();
}
void x86_icall(X86Context& c, uint8_t*, uint32_t target) {
    // The only fixture boundary is the shape-specific virtual acceleration/bounds rebuild. Polygon normals
    // and areas were already rebuilt by real 2395F0 -> 1A5730; no collision response is simulated here.
    assert(target == kRebuildTarget && c.ecx == kBound && W(c.esp) == 0x12153F);
    ++virtual_rebuilds;
    c.esp += 4;
}

int main() {
    BasicAndCandidate(); TaperAndSparseLayers(); AuthoredClearanceAndExtremeRadii(); TypeTwoAndAliasWitness();
    std::cout << "actual 121150 / 121440 / 1214F0 / 2395F0 / 1A5730 chassis floor: PASS\n"
                 "synthetic shared-bound setter witness: PASS (real per-car uniqueness remains unproved)\n";
}
