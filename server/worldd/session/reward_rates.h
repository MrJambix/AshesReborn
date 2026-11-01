// SPDX-License-Identifier: AGPL-3.0-only
// Deployment rates apply to newly earned XP, never saved totals or level scrolls.
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

namespace aoc::worldd::rates {
inline bool valid(double multiplier) {
    return std::isfinite(multiplier) && multiplier >= 0.0 && multiplier <= 1000.0;
}

inline int64_t experience(int64_t base, double multiplier) {
    if (base <= 0 || multiplier == 1.0 || !valid(multiplier)) return base;
    const long double result = std::floor(static_cast<long double>(base) * multiplier + 0.5L);
    if (result >= static_cast<long double>(std::numeric_limits<int64_t>::max()))
        return std::numeric_limits<int64_t>::max();
    return static_cast<int64_t>(result);
}
}
