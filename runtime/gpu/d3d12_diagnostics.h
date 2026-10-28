#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace gpu::diagnostics {

// Diagnostic work must stay bounded even if a driver repeats a warning indefinitely.
// This policy never grants permission to reset a GPU allocator or reuse an upload buffer.
struct Budget {
    static constexpr uint32_t kInitialSubmissions = 16, kInitialPresents = 16, kPendingPolls = 8;
    // Scan the entire bounded queue: clearing an unscanned warning tail could hide a later error.
    static constexpr uint32_t kStoredMessages = 256, kMessagesPerPoll = kStoredMessages;
    static constexpr size_t kMessageBytes = 8192, kDescriptionBytes = 4096;

    bool Submission() { return Take(submissions_, kInitialSubmissions); }
    bool Present() { return Take(presents_, kInitialPresents); }
    bool Pending() { return Take(pending_, kPendingPolls); }
    // D3D12 corruption/error/warning severities are 0/1/2. Reserve an independent error budget.
    bool Message(uint32_t severity) {
        if (severity <= 1) return Take(errors_, 32);
        if (severity == 2) return Take(warnings_, 32);
        return false;
    }
    static uint64_t ScanCount(uint64_t available) { return std::min<uint64_t>(available, kMessagesPerPoll); }
    static bool MessageSize(size_t bytes, size_t header_bytes) {
        return header_bytes != 0 && bytes >= header_bytes && bytes <= kMessageBytes;
    }
    static size_t DescriptionSize(size_t reported, size_t available) {
        return std::min({reported, available, kDescriptionBytes});
    }

private:
    static bool Take(uint32_t& value, uint32_t limit) {
        if (value == limit) return false;
        ++value;
        return true;
    }
    uint32_t submissions_ = 0, presents_ = 0, pending_ = 0, errors_ = 0, warnings_ = 0;
};
}  // namespace gpu::diagnostics
