// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  items/ground_crates.h
//
//  Crates on the ground: a crate that Drop Crate took off a back lies where it fell until someone picks it up.  The book holds the crate item itself (its instance, quality and
//  contents, as the cargo slot held it) under the world actor that shows it; the item is in no inventory meanwhile, so it cannot be had twice.
//
//  What the client is shown (read from the client, AOCClient-Win64-Shipping.exe, and the game's data): retail lays a crate down as an AAoCCargoActor
//  (/Script/GameSystemsPlugin.AoCCargoActor, no Blueprint: its constructor makes CollisionComp, CargoMesh, CrateMovement, Stats, LootStorage, LootComp and the
//  InteractableComponent; OnRep_CargoRecord loads the crate record's WorldModel; BeginPlay on a client binds the InteractableComponent to OnInteracted), with the interaction
//  ICD_CargoActor (2 m, a 1 s channel, the All_PickUpCrate montage, used once).  No recording here opens a cargo actor, and a wrong open of a new class closes the connection of every
//  client that comes near (FGuildData did that on 1 November).  So the crate is shown as the game's own Prop_Despawning_Crate Blueprint, whose parent is
//  /Script/GameSystemsPlugin.InteractableStateMachineBase (its BlueprintGeneratedClass's SuperStruct), the native class of the recorded quest objects: the recorded open of a quest
//  object with that Blueprint's path and ICD_CargoActor with its player interaction put in.  It looks like a plain crate, not the crate's own model.
//
//  Nothing here reads a clock or touches the registry: PlayerWorld makes and removes the actors, and the sessions ask the book.  PlayerWorld also keeps the book in a file
//  (--ground-crates-file) after every change and lays the crates out again when the server starts, so a restart loses none.
// ============================================================================
#pragma once

#include "svc/services.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aoc::worldd::items {

/// The game's Blueprint the crate on the ground is shown as (see above), and the interactable definition and player interaction the game gives a crate on the ground.
inline constexpr const char* kGroundCrateArchetype = "/Game/DesignAssets/World/Props/Interactables/Prop_Despawning_Crate.Default__Prop_Despawning_Crate_C";
inline constexpr int64_t     kIcdCargoActor = 6064633945792250998ll;   // ICD_CargoActor
inline constexpr int64_t     kPidCargoActor = 6064633770605478014ll;   // its PlayerInteractionDef
/// How long a crate lies before it goes back to whoever laid it down (modelled: the game's lifespan is the server's, ServerSetLifeSpan, and no recording shows it).
inline constexpr double kGroundCrateBackSeconds = 30.0 * 60.0;

struct GroundCrate {
    uint64_t           actor = 0;        // the world actor that shows it
    svc::InventoryItem item;             // the crate as the cargo slot held it
    std::string        character;        // the character id of whoever laid it down
    std::string        name;             // ... and their name, for the log
    double             x = 0, y = 0, z = 0;
    double             yaw = 0;          // degrees
    double             back_at = 0;      // the world clock at which it goes back to them
};

class GroundCrates {
public:
    /// Holds a crate. False for an actor or an item instance the book holds already.
    bool add(GroundCrate crate) {
        if (crate.actor == 0 || crates_.count(crate.actor)) return false;
        for (const auto& [id, c] : crates_) {
            if (c.item.instance_guid == crate.item.instance_guid) return false;
        }
        const uint64_t id = crate.actor;
        crates_.emplace(id, std::move(crate));
        return true;
    }
    const GroundCrate* find(uint64_t actor) const {
        const auto it = crates_.find(actor);
        return it == crates_.end() ? nullptr : &it->second;
    }
    /// The crate leaves the book (picked up, or gone back): the one who asks has it.  nullopt when the book does not hold it (someone else was first).
    std::optional<GroundCrate> take(uint64_t actor) {
        const auto it = crates_.find(actor);
        if (it == crates_.end()) return std::nullopt;
        GroundCrate c = std::move(it->second);
        crates_.erase(it);
        return c;
    }
    /// The actors of the crates `character` laid down whose time to go back has come at `now`.
    std::vector<uint64_t> due_for(const std::string& character, double now) const {
        std::vector<uint64_t> out;
        for (const auto& [id, c] : crates_) {
            if (c.character == character && now >= c.back_at) out.push_back(id);
        }
        return out;
    }
    size_t size() const { return crates_.size(); }
    /// When the crate goes back to whoever laid it down (a crate laid out again at a restart keeps the time it had left).  False for an actor the book does not hold.
    bool set_back_at(uint64_t actor, double back_at) {
        const auto it = crates_.find(actor);
        if (it == crates_.end()) return false;
        it->second.back_at = back_at;
        return true;
    }
    const std::map<uint64_t, GroundCrate>& all() const { return crates_; }

private:
    std::map<uint64_t, GroundCrate> crates_;
};

}  // namespace aoc::worldd::items
