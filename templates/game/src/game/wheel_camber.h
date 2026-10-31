// Camber tilt for the original wheel palette builder (0xF7170).
// F7170 scales the wheel matrix rows first (axle row x 2*width at 0xF7405, the two diameter rows x diameter),
// then rotates the axle and vertical rows by the camber angle with 0x869D0 (0xF7714 first side, 0xF7D39 opposite
// side): a' = c*a + s*b, b' = c*b - s*a. With rows of different length that mix is a shear, not a rotation. The
// stock -2.5/-1 degrees hide it; a large PC camber visibly bends the tire. Rotate the unit axes instead and give
// each row its own length back, which is the scaled wheel tilted as a rigid body.
#pragma once
#include <array>
#include <cmath>

namespace wheel_camber {

// rows: the axle row then the vertical row, three floats each. False leaves them alone for the original to rotate.
inline bool Rotate(std::array<float, 6>& rows, float radians) {
    const double la = std::sqrt(double(rows[0]) * rows[0] + double(rows[1]) * rows[1] + double(rows[2]) * rows[2]);
    const double lb = std::sqrt(double(rows[3]) * rows[3] + double(rows[4]) * rows[4] + double(rows[5]) * rows[5]);
    if (!std::isfinite(radians) || !std::isfinite(la) || !std::isfinite(lb) || la < 1e-6 || lb < 1e-6) return false;
    const double c = std::cos(double(radians)), s = std::sin(double(radians));
    std::array<float, 6> out{};
    for (int i = 0; i < 3; ++i) {
        const double a = rows[i] / la, b = rows[3 + i] / lb;
        out[i] = float(la * (c * a + s * b));
        out[3 + i] = float(lb * (c * b - s * a));
    }
    rows = out;
    return true;
}

// the original 0x869D0 mix, for comparison
inline void RotateOriginal(std::array<float, 6>& rows, float radians) {
    const float c = float(std::cos(double(radians))), s = float(std::sin(double(radians)));
    std::array<float, 6> out{};
    for (int i = 0; i < 3; ++i) {
        out[i] = c * rows[i] + s * rows[3 + i];
        out[3 + i] = c * rows[3 + i] - s * rows[i];
    }
    rows = out;
}

}  // namespace wheel_camber
