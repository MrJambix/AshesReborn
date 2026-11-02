// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/fall_damage.h: how far a player fell, read from the moves it sends, and the share of its maximum Health the landing takes.
//  The law is the game's FallDamage equation (StatEquationType, the game's own numbers): the distance is the smaller of the one the speed at impact gives (v*v / 2g, with
//  g = 9.8 x 2.5, plus what the time at terminal velocity added) and the one fallen; nothing under 10 m, all of it at 40 m.  The speed is measured from the moves'
//  heights and times [modelled: the client's GetFallVelocity is not on the wire]; 40 m/s is the engine's default terminal velocity [modelled].
//  A slide down a steep slope is "falling" for a long way at a low speed: the speed is what keeps that from hurting.
// ============================================================================
#pragma once

#include "protocol/repl/move_packed.h"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace aoc::worldd::fall {

inline constexpr double kGravity = 9.8 * 2.5;      // m/s/s: "g is -980 cm, but PlayerPawn also has a 2.5 grav mult"
inline constexpr double kMinDistance = 10.0;       // m
inline constexpr double kLethalDistance = 40.0;    // m
inline constexpr double kTerminalCm = 4000.0;      // cm/s
inline constexpr double kWindow = 0.1;             // s: the speed is read over at least this long (a frame's height is too rough)

/// The share of the maximum Health (0 to 1) for a fall of `fallen_cm`, landing at `speed_cm` per second after `terminal_seconds` at terminal velocity.
inline double share_of(double fallen_cm, double speed_cm, double terminal_seconds) {
    const double v = speed_cm / 100.0 + kGravity * terminal_seconds;
    const double distance = std::min(v * v / (2.0 * kGravity), fallen_cm / 100.0);
    return std::clamp((distance - kMinDistance) / (kLethalDistance - kMinDistance), 0.0, 1.0);
}

struct Landing {
    double fallen_cm = 0, speed_cm = 0, share = 0;
};

/// One player's current fall, fed its accepted moves.
class Track {
public:
    /// A move: a Landing when it ends a fall on the ground (any other mode, water or flight, ends it with none).
    std::optional<Landing> moved(uint8_t mode, double z, float stamp) {
        if (mode == protocol::repl::kMoveFalling) {
            if (!falling_) {
                falling_ = true;
                top_ = z;
                speed_ = terminal_ = 0;
                sample_z_ = z;
                sample_at_ = stamp;
                return std::nullopt;
            }
            top_ = std::max(top_, z);
            const double dt = stamp - sample_at_;
            if (dt <= 0) {   // (the client's clock restarted)
                sample_z_ = z;
                sample_at_ = stamp;
            } else if (dt >= kWindow) {
                speed_ = speed_over(z, dt);
                if (speed_ >= kTerminalCm * 0.98) terminal_ += dt;
                sample_z_ = z;
                sample_at_ = stamp;
            }
            return std::nullopt;
        }
        if (!falling_) return std::nullopt;
        falling_ = false;
        if (mode != protocol::repl::kMoveWalking) return std::nullopt;
        // (the last stretch, the landing move's own height included: the speed at impact)
        if (const double dt = stamp - sample_at_; dt > 0.01) speed_ = std::max(speed_, speed_over(z, dt));
        Landing l;
        l.fallen_cm = std::max(0.0, top_ - z);
        l.speed_cm = speed_;
        l.share = share_of(l.fallen_cm, speed_, terminal_);
        return l;
    }
    void forget() { falling_ = false; }
    bool falling() const { return falling_; }

private:
    /// The speed at the end of a stretch: its average, which is half its gravity behind.
    double speed_over(double z, double dt) const { return std::max(0.0, (sample_z_ - z) / dt + kGravity * 100.0 * dt / 2.0); }

    bool   falling_ = false;
    double top_ = 0, speed_ = 0, terminal_ = 0, sample_z_ = 0;
    float  sample_at_ = 0;
};

}  // namespace aoc::worldd::fall
