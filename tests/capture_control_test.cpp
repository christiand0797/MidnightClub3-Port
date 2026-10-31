#include "gpu/capture_control.h"
#include <cassert>

// The F1 Capture tab and the automatic mode's timing, without a window, GPU or game.
int main() {
    using gpu::capture::AutoSchedule;
    {
        AutoSchedule s;
        // Off: never due. Turning it on starts the interval rather than capturing at once.
        assert(!s.Due(0, false, 10, 0, 5, false));
        assert(!s.Due(100, true, 10, 0, 5, false));
        assert(!s.Due(109.9, true, 10, 0, 5, false));
        assert(s.Due(110, true, 10, 0, 5, false));
        s.Taken(110);
        assert(!s.Due(115, true, 10, 1, 5, false));
        // Menu open: play is paused, nothing is captured; once it closes an overdue capture is taken.
        assert(!s.Due(125, true, 10, 1, 5, true));
        assert(s.Due(126, true, 10, 1, 5, false));
        s.Taken(126);
        // The session limit stops it.
        assert(!s.Due(500, true, 10, 5, 5, false));
        // Off then on again restarts the interval from the moment it is enabled.
        assert(!s.Due(600, false, 10, 2, 5, false));
        assert(!s.Due(601, true, 10, 2, 5, false));
        assert(s.Due(611, true, 10, 2, 5, false));
        // Non-finite or tiny intervals are clamped to the minimum rather than capturing every frame.
        AutoSchedule t;
        assert(!t.Due(0, true, 0, 0, 5, false));
        assert(!t.Due(4.9, true, 0, 0, 5, false));
        assert(t.Due(5, true, 0, 0, 5, false));
    }
    {
        // "Capture in N seconds" from the menu: taken once, after the delay, and never while the menu is open.
        gpu::capture::Delayed d;
        assert(!d.Take(0, false));
        d.Request(10, 3);
        assert(!d.Take(12.9, false));
        assert(!d.Take(13, true));
        assert(d.Take(13.5, false));
        assert(!d.Take(20, false));
        d.Request(30, 3);
        d.Cancel();
        assert(!d.Take(40, false));
    }
    {
        // Status reported back to the menu.
        gpu::capture::Status s;
        s.Finished("C:/x/captures/f1", true);
        s.Finished("C:/x/captures/f2", false);
        const auto v = s.Snapshot();
        assert(v.automatic == 1 && v.manual == 1 && v.last == "C:/x/captures/f2" && v.error.empty());
        s.Failed("disk full");
        assert(s.Snapshot().error == "disk full" && s.Automatic() == 1);
    }
}
