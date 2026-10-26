#include "gpu/photo_capture_policy.h"
#include <cassert>
#include <iostream>
#include <vector>

namespace {
using namespace gpu::photo_capture;
constexpr uint64_t kLeft = 0x4D145117D9DD0F63ull, kRight = 0xA21582D8BE650E3Full;
constexpr auto kHashes = "4D145117D9DD0F63,A21582D8BE650E3F";

void Parsing() {
    assert(Parse(""));
    assert(!Policy(*Parse("")).Armed());
    assert(Parse(kHashes));
    assert(Parse("4d145117d9dd0f63,a21582d8be650e3f", "1"));
    for (auto hashes : {"4D145117D9DD0F6", "4D145117D9DD0F630", "0000000000000000", "4D145117D9DD0F63,",
                       "4D145117D9DD0F63,4D145117D9DD0F63", "4D145117D9DD0F63,A21582D8BE650E3F,1234567890123456",
                       "4D145117D9DD0F6G", " 4D145117D9DD0F63"}) assert(!Parse(hashes));
    for (auto ordinals : {"", "0", "121", "2,1", "1,1", "1,", ",1", "1,2,3,4,5", "-1", "+1", "1 2",
                         "999999999999999999999999999999999999999"}) assert(!Parse(kHashes, ordinals));
    assert(Parse(kHashes, "120"));
    Config invalid;
    invalid.hash_count = 999;
    assert(!Policy(invalid).Armed());
}

void DisabledAndWrongHashes() {
    Policy inert;
    for (unsigned i = 0; i < 200; ++i) {
        inert.ObserveIssuedTexture(kLeft);
        assert(!inert.FinishPresent().capture);
    }
    Policy policy(*Parse(kHashes));
    for (unsigned i = 0; i < 500; ++i) {
        policy.ObserveIssuedTexture(123);
        assert(!policy.FinishPresent().capture);
    }
    assert(policy.Armed());  // Ordinary menus before the first target do not spend the episode budget.
}

void ExactSameFrameAndSequence() {
    Policy policy(*Parse(kHashes));
    policy.ObserveIssuedTexture(kLeft);
    assert(!policy.FinishPresent().capture);
    policy.ObserveIssuedTexture(kRight);
    assert(!policy.FinishPresent().capture);  // Panels from different presents are not a complete photo match.
    std::vector<uint32_t> selected;
    for (uint32_t i = 1; i <= 40; ++i) {
        policy.ObserveIssuedTexture(kLeft);
        policy.ObserveIssuedTexture(kLeft);  // Repeated binds/draws do not advance the ordinal.
        policy.ObserveIssuedTexture(kRight);
        auto decision = policy.FinishPresent();
        if (decision.capture) {
            assert(decision.matched_ordinal == i);
            assert(decision.observed_mask == 3);
            selected.push_back(i);
        }
    }
    assert((selected == std::vector<uint32_t>{1, 4, 8, 16}));
    assert(!policy.Armed());
}

void OnceAndBudget() {
    Policy once(*Parse(kHashes, "1"));
    once.ObserveIssuedTexture(kLeft); once.ObserveIssuedTexture(kRight);
    assert(once.FinishPresent().capture);  // The issued frame itself, not the following frame.
    assert(!once.FinishPresent().capture);
    Policy partial(*Parse(kHashes));
    partial.ObserveIssuedTexture(kLeft);
    for (uint32_t i = 0; i < kEpisodePresents; ++i) assert(!partial.FinishPresent().capture);
    assert(!partial.Armed());
    partial.ObserveIssuedTexture(kLeft); partial.ObserveIssuedTexture(kRight);
    assert(!partial.FinishPresent().capture);
    Policy boundary(*Parse(kHashes, "120"));
    for (uint32_t i = 1; i <= kEpisodePresents; ++i) {
        boundary.ObserveIssuedTexture(kLeft); boundary.ObserveIssuedTexture(kRight);
        assert(boundary.FinishPresent().capture == (i == kEpisodePresents));
    }
    assert(!boundary.Armed());
}
}  // namespace

int main() {
    Parsing(); DisabledAndWrongHashes(); ExactSameFrameAndSequence(); OnceAndBudget();
    std::cout << "photo capture policy: 4 groups passed (CPU only; no device created)\n";
}
