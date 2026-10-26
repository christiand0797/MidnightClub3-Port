#pragma once
// CPU-only selection policy. The backend supplies observations from issued draws and completed frames.
// Observe only textures used by an issued draw; FinishPresent selects that same completed frame.
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace gpu::photo_capture {

constexpr uint32_t kMaxTextures = 2;
constexpr uint32_t kMaxCaptures = 4;
constexpr uint32_t kMaxOrdinal = 120;
constexpr uint32_t kEpisodePresents = 120;

struct Config {
    std::array<uint64_t, kMaxTextures> hashes{};
    std::array<uint32_t, kMaxCaptures> ordinals{};
    uint32_t hash_count = 0, ordinal_count = 0;
};

inline bool Valid(const Config& c) {
    if (!c.hash_count) return !c.ordinal_count;
    if (c.hash_count > kMaxTextures || !c.ordinal_count || c.ordinal_count > kMaxCaptures) return false;
    for (uint32_t i = 0; i < c.hash_count; ++i) {
        if (!c.hashes[i]) return false;
        for (uint32_t j = 0; j < i; ++j) if (c.hashes[i] == c.hashes[j]) return false;
    }
    for (uint32_t i = 0; i < c.ordinal_count; ++i)
        if (!c.ordinals[i] || c.ordinals[i] > kMaxOrdinal || (i && c.ordinals[i] <= c.ordinals[i - 1])) return false;
    return true;
}

// Exact 16-digit hashes; positive increasing decimal ordinals. No signs, spaces, partial values or overflow.
// An absent hash list is the inert default. Malformed opt-in configuration returns nullopt.
inline std::optional<Config> Parse(std::string_view hashes, std::string_view ordinals = "1,4,8,16") {
    Config c;
    if (hashes.empty()) return c;
    while (!hashes.empty()) {
        const size_t end = hashes.find(',');
        const auto part = hashes.substr(0, end);
        if (c.hash_count == kMaxTextures || part.size() != 16) return std::nullopt;
        uint64_t value = 0;
        for (char ch : part) {
            uint32_t digit;
            if (ch >= '0' && ch <= '9') digit = uint32_t(ch - '0');
            else if (ch >= 'A' && ch <= 'F') digit = uint32_t(ch - 'A' + 10);
            else if (ch >= 'a' && ch <= 'f') digit = uint32_t(ch - 'a' + 10);
            else return std::nullopt;
            value = (value << 4) | digit;
        }
        c.hashes[c.hash_count++] = value;
        if (end == std::string_view::npos) break;
        hashes.remove_prefix(end + 1);
        if (hashes.empty()) return std::nullopt;
    }
    while (!ordinals.empty()) {
        const size_t end = ordinals.find(',');
        const auto part = ordinals.substr(0, end);
        if (c.ordinal_count == kMaxCaptures || part.empty()) return std::nullopt;
        uint32_t value = 0;
        for (char ch : part) {
            if (ch < '0' || ch > '9') return std::nullopt;
            value = value * 10 + uint32_t(ch - '0');
            if (value > kMaxOrdinal) return std::nullopt;  // Bound each step before another multiplication.
        }
        c.ordinals[c.ordinal_count++] = value;
        if (end == std::string_view::npos) break;
        ordinals.remove_prefix(end + 1);
        if (ordinals.empty()) return std::nullopt;
    }
    return Valid(c) ? std::optional<Config>(c) : std::nullopt;
}

struct Decision {
    bool capture = false;
    uint32_t matched_ordinal = 0;
    uint32_t observed_mask = 0;
};

class Policy {
public:
    Policy() = default;
    explicit Policy(Config c) : config_(Valid(c) ? c : Config{}) {}

    bool Armed() const { return config_.hash_count && !done_; }

    bool Wants(uint64_t hash) const {
        if (!Armed()) return false;
        for (uint32_t i = 0; i < config_.hash_count; ++i) if (config_.hashes[i] == hash) return true;
        return false;
    }

    void ObserveIssuedTexture(uint64_t hash) {
        if (!Armed()) return;
        for (uint32_t i = 0; i < config_.hash_count; ++i)
            if (config_.hashes[i] == hash) seen_ |= 1u << i;
    }

    // Call exactly once for every Present, after all title draws for it. Missing panels do not carry over.
    // The budget begins at the first requested texture, so a partial/wrong match cannot remain active forever.
    Decision FinishPresent() {
        Decision decision;
        const uint32_t mask = seen_;
        seen_ = 0;
        if (!Armed()) return decision;
        started_ |= mask != 0;
        if (!started_) return decision;
        ++episode_presents_;
        decision.observed_mask = mask;
        if (mask == ((1u << config_.hash_count) - 1u)) {
            decision.matched_ordinal = ++matched_;
            if (matched_ == config_.ordinals[next_]) {
                decision.capture = true;
                if (++next_ == config_.ordinal_count) done_ = true;
            }
        }
        if (episode_presents_ == kEpisodePresents) done_ = true;
        return decision;  // A failed readback still consumes its request; output attempts stay bounded.
    }

private:
    Config config_;
    uint32_t seen_ = 0, matched_ = 0, next_ = 0, episode_presents_ = 0;
    bool started_ = false, done_ = false;
};

}  // namespace gpu::photo_capture
