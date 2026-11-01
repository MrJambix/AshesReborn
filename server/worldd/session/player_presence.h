// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include "bootstrap/bootstrap.h"
#include "session/move_checker.h"
#include "world/actor_registry.h"

namespace aoc::worldd {
namespace stats { class CharacterStats; }

// The public projection of a player. The owning connection keeps its bootstrap actors;
// nearby connections receive these actors through their normal, budgeted ActorView.
class PlayerPresence {
public:
    explicit PlayerPresence(world::ActorRegistry& registry) : registry_(registry) {}
    ~PlayerPresence();
    PlayerPresence(const PlayerPresence&) = delete;
    PlayerPresence& operator=(const PlayerPresence&) = delete;
    bool update(const bootstrap::PlayerBootstrap& player, const GoodPosition& position,
                const stats::CharacterStats* stats, double now);
    void clear();

private:
    world::ActorRegistry& registry_;
    uint64_t pawn_ = 0, state_ = 0;
    double next_update_ = 0, last_update_ = 0;
    GoodPosition previous_;
};
}
