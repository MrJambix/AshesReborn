// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_fall.cpp: fall damage (session/fall_damage.h)
// ============================================================================
#include "session/player_session.h"

#include <spdlog/spdlog.h>

namespace aoc::worldd {

namespace {
constexpr int64_t kFallDamageImmune = 6064634364190851072ll;   // Fall_Damage_Immune: the equation deals nothing to its holder
constexpr auto    kGuardAfterArrival = std::chrono::seconds(10);
}  // namespace

void PlayerSession::note_fall(Connection& conn, const protocol::repl::NetworkMove& move, Clock::time_point now, bool on_mount) {
    // a mount's fall is the mount's, and a character still arriving comes down on ground the client is only now loading
    if (on_mount || dead_ || !pipeline_ || !phase_in_done_) {
        fall_.forget();
        if (!phase_in_done_) fall_guard_until_ = now + kGuardAfterArrival;
        return;
    }
    if (now < fall_guard_until_) {
        fall_.forget();
        return;
    }
    const auto landing = fall_.moved(move.movement_mode, move.location_cm().z, move.time_stamp);
    if (!landing || !(landing->share > 0) || pipeline_->stacks_of(kFallDamageImmune) > 0) return;
    const double amount = landing->share * pipeline_->stat_of(content::kGuidMaxHealth);
    spdlog::info("[session] {}: fell {:.1f} m, landing at {:.1f} m/s: {:.0f} damage", name(), landing->fallen_cm / 100.0, landing->speed_cm / 100.0, amount);
    take_damage(conn, amount, now);
}

}  // namespace aoc::worldd
