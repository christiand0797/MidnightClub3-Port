#include "gpu/d3d12_diagnostics.h"
#include <cassert>
#include <cstdint>
#include <limits>

int main() {
    using gpu::diagnostics::Budget;
    Budget budget;
    unsigned submissions = 0, presents = 0, pending = 0;
    for (unsigned i = 0; i < 100000; ++i) {
        submissions += budget.Submission();
        presents += budget.Present();
        pending += budget.Pending();
    }
    assert(submissions == 16 && presents == 16 && pending == 8);
    // A flood of warnings cannot consume the separately reserved corruption/error observations.
    unsigned warnings = 0, errors = 0;
    for (unsigned i = 0; i < 100000; ++i) warnings += budget.Message(2);
    for (unsigned i = 0; i < 100000; ++i) errors += budget.Message(i & 1);
    assert(warnings == 32 && errors == 32);
    assert(!budget.Message(3) && !budget.Message(std::numeric_limits<uint32_t>::max()));
    assert(Budget::ScanCount(0) == 0 && Budget::ScanCount(1) == 1);
    assert(Budget::ScanCount(std::numeric_limits<uint64_t>::max()) == 256);
    Budget mixed_queue;
    unsigned errors_after_warnings = 0;
    for (uint64_t i = 0; i < Budget::ScanCount(256); ++i) {
        const uint32_t severity = i < 200 ? 2 : 1;
        const bool emitted = mixed_queue.Message(severity);
        if (severity == 1 && emitted) ++errors_after_warnings;
    }
    assert(errors_after_warnings == 32);
    assert(!Budget::MessageSize(0, 32) && !Budget::MessageSize(31, 32));
    assert(Budget::MessageSize(32, 32) && Budget::MessageSize(8192, 32));
    assert(!Budget::MessageSize(8193, 32) && !Budget::MessageSize(std::numeric_limits<size_t>::max(), 32));
    assert(!Budget::MessageSize(32, 0));
    assert(Budget::DescriptionSize(100, 12) == 12);
    assert(Budget::DescriptionSize(12, 100) == 12);
    assert(Budget::DescriptionSize(std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max()) == 4096);
    assert(Budget::DescriptionSize(10, 0) == 0);
}
