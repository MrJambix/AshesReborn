// SPDX-License-Identifier: AGPL-3.0-only
// ============================================================================
//  server/worldd/session/crowd_state.h
//
//  What a player's live effects say of UAoCStatsComponent.CrowdControlState.
//
//  From the client (`UAoCStatsComponent` +0x13D0): 0x14612d740 reads bit 1 (DisableMovement), 0x14612da70 bit 2
//  (DisableRotation), and 0x146c25530 asks both to decide whether the character is locked.  Bit 0x20 starts
//  UpdatePushVelocity 0x146141910, which needs fields nothing sends, so a player is sent only bits 1, 2, 4 and 16:
//  knockbacks stay out.  Worked out from the live effects each time, so a lock cannot outlast its effect, which
//  is the guard against freezing a player.  Modelled: no recording shows a player's CrowdControlState.
// ============================================================================
#pragma once

#include "combat/crowd_control_table.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace aoc::worldd::crowd {

inline constexpr uint8_t kPlayerBits = 1 | 2 | 4 | 16;
inline constexpr uint8_t kAnimationNone = 255;

/// A live effect's crowd control and its times on the pipeline's clock; `end` is infinity for an infinite effect.
struct Held {
    const combat::CrowdControlRow* row = nullptr;
    double start = 0, end = 0;
};

struct State {
    uint8_t status = 0;
    uint8_t animation = kAnimationNone;
    int64_t record = 0;
    float snare = 0;
    double start = 0, end = 0;   // of what is shown; 0: nothing shown, or no end
    bool operator==(const State&) const = default;
};

inline State combine(const std::vector<Held>& held) {
    State out;
    const Held* shown = nullptr;
    for (const Held& h : held) {
        if (!h.row) continue;
        out.status = static_cast<uint8_t>(out.status | (h.row->status & kPlayerBits));
        out.snare = std::max(out.snare, static_cast<float>(h.row->snare));
        if (!shown || h.row->priority > shown->row->priority) shown = &h;
    }
    if (shown) {
        out.animation = shown->row->animation;
        out.record = shown->row->guid;
        out.start = shown->start;
        out.end = std::isinf(shown->end) ? 0 : shown->end;
    }
    return out;
}

}  // namespace aoc::worldd::crowd
