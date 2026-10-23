#include "app/resolution.h"
#include <cassert>
#include <cmath>
#include <cstdint>

int main() {
    using namespace app::resolution;
    const Extent guest{640, 480};
    // Full and proportional effect/depth buffers get the exact requested native widescreen size.
    for (Extent native : {Extent{640, 480}, {854, 480}, {1280, 720}, {1920, 1080}, {2560, 1440},
                          {3840, 2160}, {7680, 4320}, {15360, 8640}}) {
        assert(ValidScene(native));
        assert(Scene(native, {800, 600}, 100) == native);
        assert(Target(guest, guest, native, double(native.height) / guest.height) == native);
        Extent half = Target({320, 240}, guest, native, double(native.height) / guest.height);
        assert(half.width == (native.width + 1) / 2 && half.height == native.height / 2);
        auto full_rect = MapRect(guest, native, {0, 0, 640, 480});
        assert(full_rect.right == native.width && full_rect.bottom == native.height);
    }
    // A former X-scale-for-Y bug clipped 1080p scissors and clears 360 lines past the target.
    auto centre = MapRect(guest, {1920, 1080}, {160, 120, 480, 360});
    assert(centre.left == 480 && centre.top == 270 && centre.right == 1440 && centre.bottom == 810);
    auto clipped = MapRect(guest, {1920, 1080}, {-100, -100, 800, 900});
    assert(clipped.left == 0 && clipped.top == 0 && clipped.right == 1920 && clipped.bottom == 1080);
    assert((Target({128, 128}, guest, {1920, 1080}, 2.25) == Extent{288, 288}));
    assert((Target({1024, 1024}, guest, {15360, 8640}, 18) == Extent{16384, 16384}));
    // 16K supersampling saturates at the D3D12 dimension limit while preserving 16:9.
    Extent capped = Scene({15360, 8640}, {1280, 720}, 400);
    assert(capped.width == 16384 && capped.height == 9216);
    assert((Scene({}, {1920, 1080}, 150) == Extent{2880, 1620}));
    assert((Scale({UINT32_MAX, UINT32_MAX}, 400.0) == Extent{16384, 16384}));
    assert((Scale({1920, 1080}, NAN) == Extent{1, 1}));
    assert(!ValidScene({0, 1080}) && !ValidScene({16385, 8640}));
    assert(SameScale({640, 480}, {1920, 1080}, {320, 240}, {960, 540}));
    assert(!SameScale({640, 480}, {1920, 1080}, {320, 240}, {960, 720}));
    assert((FitWindow({15360, 8640}, {1900, 1000}) == Extent{1777, 1000}));
    assert((FitWindow({640, 480}, {1900, 1000}) == Extent{640, 480}));
}
