// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// Included inside player_session.cpp's namespace.

void PlayerSession::clear_pet() {
    if (registry_ && pet_actor_) registry_->remove(pet_actor_);
    if (pet_actor_) pet_dirty_ = true;
    pet_actor_ = 0;
    pet_effect_ = -1;
}

void PlayerSession::dismiss_pet(Clock::time_point now) {
    if (pipeline_) {
        for (const auto& d : pets::kDefinitions) pipeline_->remove_effect(d.effect, ability_seconds(now));
        if (abilities_) take_ability_events();
    }
    pet_effects_.clear();
    clear_pet();
}

void PlayerSession::run_pets(Connection& conn, Clock::time_point now) {
    if (!world_ || !registry_ || !abilities_) return;
    const double sec = ability_seconds(now);
    const int64_t wanted = dead_ || pet_effects_.empty() ? -1 : pet_effects_.rbegin()->first;
    if (pet_actor_ && (!registry_->find(pet_actor_) || wanted != pet_effect_)) clear_pet();
    if (wanted >= 0 && !pet_actor_) {
        const pets::Definition* def = pets::for_effect(pet_effects_.at(wanted));
        const spawns::GenericNpcs* generic = world_->generic_npcs();
        content::NpcAssetRow asset;
        asset.archetype_path = def->archetype;
        const spawns::OpenTemplate* open = generic ? generic->template_for(asset) : nullptr;
        std::string error;
        std::optional<world::WorldActor> made;
        const auto motion = world_->blueprints().find(def->archetype);
        const double foot = motion ? motion->half_height * motion->default_scale : 88.0;
        if (open) {
            const GoodPosition at = checker_.good();
            const double yaw = at.yaw_degrees * 3.14159265358979323846 / 180.0;
            spawns::InstanceParams p;
            // SpawnNPCRecordBase.Location is a relative offset (200, 400, 0).
            p.x = at.x + 200.0 * std::cos(yaw) - 400.0 * std::sin(yaw);
            p.y = at.y + 200.0 * std::sin(yaw) + 400.0 * std::cos(yaw);
            p.z = at.z - 88.0 + foot;
            p.yaw_degrees = at.yaw_degrees;
            p.world_time = pb_.setup().world_time + session_seconds(now);
            p.server_id = world_->server_id();
            p.level = stats_ ? stats_->level() : 1;
            p.name = def->name;
            p.clear_population = true;
            made = spawns::instantiate(*open, [this] { return world_->mint_guid(); }, p, error);
        } else {
            error = "the pet Blueprint template is unavailable (load npc_motion.tsv and the area)";
        }
        if (made) {
            auto& actor = made->actor;
            actor.obj.state.set(*actor.obj.schema, "Instigator", repl::Value(pb_.setup().guids.pawn));
            made->never_dormant = true;
            made->npc.source = world::Source::Runtime;
            made->npc.aggro_off = true;
            const auto& words = pb_.setup().character_guid;
            for (auto& s : actor.subobjects) {
                if (!s.obj.schema) continue;
                auto set = [&](const char* path, repl::Value value) { return s.obj.state.set(*s.obj.schema, path, std::move(value)); };
                if (std::string(s.obj.schema->name) == "UNPCInfo") set("CreatureOwnerName", repl::Value::str(name()));
                if (std::string(s.obj.schema->name) == "UNPCSecondaryInfo") {
                    const char* paths[] = {"CreatureOwnerGuid.A", "CreatureOwnerGuid.B", "CreatureOwnerGuid.C", "CreatureOwnerGuid.D"};
                    for (size_t i = 0; i < 4; ++i) set(paths[i], repl::Value::i64(static_cast<int32_t>(words[i])));
                    set("PetSpawnerEffectId", repl::Value::i64(wanted));
                }
            }
            const uint64_t id = made->id;
            if (registry_->add(std::move(*made))) {
                pet_actor_ = id;
                pet_effect_ = wanted;
                pet_dirty_ = true;
                pet_follow_ = true;
                pet_follow_at_ = sec + 0.5;
                if (auto* npcs = npc_world_ ? npc_world_->npcs() : nullptr) {
                    npc::NpcMotionSetup setup;
                    setup.foot_offset = foot;
                    setup.home_yaw = checker_.good().yaw_degrees;
                    npcs->add(id, setup);
                }
                spdlog::info("[pets] {}: {} summoned (actor {}, effect {})", name(), def->name, id, wanted);
            } else {
                error = "the actor registry refused the pet";
            }
        }
        if (!pet_actor_) {
            spdlog::warn("[pets] {}: {} could not be summoned: {}", name(), def->name, error);
            // Do not leave the UI claiming a pet exists after a failed spawn.
            pipeline_->remove_effect(def->effect, sec);
            take_ability_events();
        }
    }
    if (pet_dirty_ && room_for(1)) {
        if (!interact_info_) interact_info_ = world_->mint_guid();
        const auto info = *interact_info_;
        const auto* pet = registry_->find(pet_actor_);
        const auto guid = pet ? pet->actor.guid : FIntrepidNetworkGUID{};
        const int64_t effect = pet_effect_;
        queue("the pawn's combat pet", [info, guid, effect](bootstrap::PlayerBootstrap& pb) -> std::optional<bootstrap::Send> {
            if (!pb.ensure_interact_info(info)) return std::nullopt;
            return pb.pawn_combat_pet(guid, effect);
        }, conn, /*must_build=*/true);
        pet_dirty_ = false;
    }
    if (pet_actor_ && pet_follow_ && sec >= pet_follow_at_) {
        pet_follow_at_ = sec + 0.5;
        if (auto* npcs = npc_world_ ? npc_world_->npcs() : nullptr) {
            const GoodPosition at = checker_.good();
            const auto* pet = registry_->find(pet_actor_);
            if (pet && std::hypot(pet->x - at.x, pet->y - at.y) > 350.0)
                npcs->order_move(pet_actor_, nav::Vec3{at.x, at.y, at.z - 88.0}, 720.0, 250.0);
        }
    }
}

void PlayerSession::on_pet_command(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (dead_ || !pet_actor_ || !rpc.call.decoded || !within_budget(quiet_budget_, "ServerSendPetCommand", now)) return;
    const auto command = repl::rpcf::int_or(repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "PetCommandId"), 0);
    if (!command) return;
    if (*command == pets::kDismiss) {
        dismiss_pet(now);
        run_pets(conn, now);
    } else if (*command == pets::kFollow) {
        pet_follow_ = true;
        pet_follow_at_ = 0;
    } else if (*command == pets::kStay) {
        pet_follow_ = false;
        if (auto* npcs = npc_world_ ? npc_world_->npcs() : nullptr) npcs->halt(pet_actor_);
    }
}
