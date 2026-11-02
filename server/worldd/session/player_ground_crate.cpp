// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_ground_crate.cpp: the crates on the ground (items/ground_crates.h)
//
//  Drop Crate lays the crate in front of the character, where anyone can pick it up: a use of its interactable (ICD_CargoActor: within 2 m, a 1 s channel, the client plays the
//  pick-up) puts it in the cargo slot of whoever used it, when that slot is free.  A crate nobody picks up goes back to whoever laid it down after half an hour, the next time they are
//  in the world with a free cargo slot.  A crate on the ground lives in the world's memory only: a restart of the server loses it (modelled: no recording shows the game's rule).
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/character_setup.h"
#include "protocol/repl/move_packed.h"

#include <spdlog/spdlog.h>

#include <cmath>

#include "session/player_session_util.h"

namespace aoc::worldd {

using protocol::emit::FIntrepidNetworkGUID;

namespace {
constexpr double kLayAheadCm = 80.0;          // the crate lies this far in front of the feet (modelled: the montage puts it down in front)
constexpr double kHalfHeightCm = 88.0;        // the character's capsule: its feet are this far under the position the moves give
constexpr double kReachMarginCm = 300.0;      // the definition's reach, and the margin a step or a lag takes (as a quest object's)
constexpr auto   kBackCheck = std::chrono::seconds(5);
}  // namespace

bool PlayerSession::lay_crate_down(Connection& conn, Clock::time_point now) {
    const svc::InventoryItem* crate = inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold);
    if (!crate || !world_ || !registry_ || !world_->ground_crates_enabled()) return false;
    const svc::InventoryItem held = *crate;
    const GoodPosition at = checker_.good();
    const double yaw = at.yaw_degrees * 3.14159265358979323846 / 180.0;
    const double x = at.x + kLayAheadCm * std::cos(yaw), y = at.y + kLayAheadCm * std::sin(yaw), z = at.z - kHalfHeightCm;
    std::string why;
    const auto actor = world_->lay_crate(*registry_, held, character_id_, name(), x, y, z, at.yaw_degrees, pb_.setup().world_time + session_seconds(now), why);
    if (!actor) {
        spdlog::warn("[crates] {}: the crate cannot lie on the ground ({}): it goes into the item bag", name(), why);
        return false;
    }
    items::Outcome o = inventory_.remove_cargo(held.instance_guid);
    if (!o.ok) {   // (the slot changed under us: the world gives the crate back)
        world_->take_crate(*registry_, *actor);
        spdlog::warn("[crates] {}: the cargo slot does not give the crate up ({}): it stays on the back", name(), o.refusal);
        return true;
    }
    apply_outcome(conn, o, "a crate was laid on the ground");
    ++crates_laid_;
    spdlog::info("[crates] {}: laid a crate (item {}, quality {}) on the ground at ({:.0f}, {:.0f}, {:.0f}); {} on the ground in the world", name(), held.item_record_id, held.quality, x, y, z,
                 world_->ground_crates());
    return true;
}

void PlayerSession::on_crate_interact(Connection& conn, const world::WorldActor& a, Clock::time_point now) {
    const auto ignore = [&](const std::string& why) {
        ++interact_ignored_;
        spdlog::log(level_for(prop_logged_, 60), "[crates] {}: a pick-up of a crate is ignored: {}", name(), why);
    };
    if (view_->state_of(a.id) != world::ClientState::Awake) return ignore("the client does not have that crate open");
    if (crate_pick_actor_ != 0) {
        if (crate_pick_actor_ != a.id) return ignore("another pick-up runs");
        return;   // (the same crate again: the pick-up runs)
    }
    if (inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold)) {
        say(conn, "You are already carrying a crate.", now);
        return ignore("a crate is carried already");
    }
    double radius = 200.0, channel = 1.0;   // ICD_CargoActor's, when the data does not say
    if (const content::ObjectData* objects = world_->objects()) {
        if (const auto def = objects->interactables.find(items::kIcdCargoActor); def != objects->interactables.end()) {
            radius = def->second.radius;
            channel = def->second.channel;
        }
    }
    const GoodPosition at = checker_.good();
    const double distance = std::hypot(a.x - at.x, a.y - at.y);
    if (distance > radius + kReachMarginCm) return ignore("it is " + std::to_string(static_cast<int>(distance / 100.0)) + " m away");
    crate_pick_actor_ = a.id;
    crate_pick_end_ = now + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(channel));
    crate_pick_x_ = at.x;
    crate_pick_y_ = at.y;
    spdlog::log(level_for(prop_logged_, 60), "[crates] {}: starts to pick up a crate ({:.0f} m away): {:.1f} s", name(), distance / 100.0, channel);
}

bool PlayerSession::pick_up_crate(Connection& conn, uint64_t actor, const char* why, bool answer, Clock::time_point now) {
    const world::WorldActor* a = registry_ ? registry_->find(actor) : nullptr;
    const items::GroundCrate* lying = world_ ? world_->ground_crate(actor) : nullptr;
    if (!a || !lying || inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold) || !room_for(24)) return false;
    // the crate answers the use (its actor goes right after: the interaction is single-use and deleted when done)
    const FIntrepidNetworkGUID component = component_guid_of(*a, a->npc.interact_component);
    const FIntrepidNetworkGUID guid = a->actor.guid;
    auto taken = world_->take_crate(*registry_, actor);
    if (!taken) return false;
    items::Outcome put = inventory_.grant_cargo(taken->item, item_catalog());
    if (!put.ok) {
        // it does not fit: it lies where it was, as it was
        std::string e;
        world_->lay_crate(*registry_, taken->item, taken->character, taken->name, taken->x, taken->y, taken->z, 0, pb_.setup().world_time + session_seconds(now), e);
        spdlog::warn("[crates] {}: the cargo slot does not take the crate ({}): it stays on the ground{}", name(), put.refusal, e.empty() ? "" : " (" + e + ")");
        return false;
    }
    if (answer) queue("ClientOnDoInteract", [component, guid](bootstrap::PlayerBootstrap& pb) { return pb.interact_done(component, guid); }, conn, /*must_build=*/true);
    apply_outcome(conn, put, why);
    ++crates_picked_;
    spdlog::info("[crates] {}: {} (item {}, laid down by {}); {} on the ground in the world", name(), why, taken->item.item_record_id, taken->name, world_->ground_crates());
    return true;
}

void PlayerSession::run_ground_crates(Connection& conn, Clock::time_point now) {
    if (!world_ || finished_ || failed_ || !phase_in_done_) return;
    if (crate_pick_actor_ != 0) {
        const auto stop = [&](const char* why) {
            crate_pick_actor_ = 0;
            spdlog::log(level_for(prop_logged_, 60), "[crates] {}: the pick-up of a crate stops: {}", name(), why);
        };
        const GoodPosition at = checker_.good();
        if (!world_->ground_crate(crate_pick_actor_)) {
            stop("the crate is gone");
        } else if (dead_) {
            stop("the player died");
        } else if (mounted()) {
            stop("the player mounted");
        } else if (std::hypot(at.x - crate_pick_x_, at.y - crate_pick_y_) > 150.0) {
            stop("the player stepped away");
        } else if (now >= crate_pick_end_) {
            const uint64_t actor = crate_pick_actor_;
            crate_pick_actor_ = 0;
            if (inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold)) {
                say(conn, "You are already carrying a crate.", now);
            } else {
                pick_up_crate(conn, actor, "picked up a crate", /*answer=*/true, now);
            }
        }
    }
    // the character's own crates that lay too long come back, one at a time, when the cargo slot is free
    if (now < crate_back_check_) return;
    crate_back_check_ = now + kBackCheck;
    if (dead_ || inventory_.at(svc::ItemContainer::Equipment, items::equip::CargoHold)) return;
    const std::vector<uint64_t> due = world_->crates_back_for(character_id_);
    if (!due.empty() && pick_up_crate(conn, due.front(), "a crate that lay on the ground too long came back", /*answer=*/false, now)) {
        say(conn, "A crate you left on the ground has been returned to you.", now);
    }
}

}  // namespace aoc::worldd
