// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).

static const repl::ArrayValue* summoned_pet_array(const PlayerSession& s) {
    for (const auto& component : s.bootstrap().pawn().subobjects) {
        if (component.name != "InteractInfo" || !component.obj.schema) continue;
        const auto* value = component.obj.state.get(*component.obj.schema, "PetArray");
        return value ? value->get_if<repl::ArrayValue>() : nullptr;
    }
    return nullptr;
}

static void test_summoner_pet_session() {
    std::printf("Summoner pets: cast completion creates and replicates the owned pet\n");
    const svc::Character rec = class25(6, "Summoner", 17753);
    const auto prepare = [](PlayerWorld& world) {
        std::string error;
        CHECK(world.load_blueprints(NPC_MOTION_PATH, error), "the pet Blueprints load");
        spawns::AreaConfig area;
        area.radius_cm = 1;
        CHECK(world.load_area(area, error), "the world's actor templates load");
    };
    Run r(0, true, &rec, prepare);
    const auto bytes = capture_blocks::bytes_of(capture_blocks::kClientReadyToPhaseIn.hex);
    BunchWriter phase;
    phase.write_bit_range(bytes.data(), 0, capture_blocks::kClientReadyToPhaseIn.bits);
    r.send_reliable(phase);
    r.play(1.0, false);
    uint64_t previous = 0;
    // A summon costs 150 x ManaCostMult (853 Mana at level 25), more than a Summoner has left after one.
    const auto refill_mana = [&r] { r.h.s->set_current_value(kManaStat, r.m.stats[kMaxManaStat]); };
    for (const auto& d : pets::kDefinitions) {
        r.h.s->teach_ability(d.ability);
        refill_mana();
        r.trigger(d.ability);
        r.play(7.0, false);
        CHECK(r.h.s->summoned_pet() == previous, "a new pet is not created before completion");
        r.play(1.8, false);
        const uint64_t id = r.h.s->summoned_pet();
        const auto* pet = r.h.fx.world->registry().find(id);
        CHECK(id && id != previous && pet, "completion creates the pet actor");
        CHECK(!previous || !r.h.fx.world->registry().find(previous), "replacement removes the previous pet");
        if (!pet) return;
        CHECK(pet->npc.archetype == d.archetype && pet->npc.level == 25 && pet->never_dormant,
              "the pet has the right Blueprint and owner level and stays awake");
        const auto& object = pet->actor.obj;
        const auto* instigator = object.state.get(*object.schema, "Instigator");
        CHECK(instigator && instigator->get_if<emit::FIntrepidNetworkGUID>() &&
              *instigator->get_if<emit::FIntrepidNetworkGUID>() == r.h.s->bootstrap().setup().guids.pawn,
              "the pet names its summoner as instigator");
        const auto* array = summoned_pet_array(*r.h.s);
        CHECK(array && array->elements.size() == 1, "the player's replicated PetArray contains one pet");
        if (array && array->elements.size() == 1) {
            const auto& values = array->elements[0].values;
            const auto ptr = values.find(1);
            CHECK(ptr != values.end() && ptr->second.get_if<emit::FIntrepidNetworkGUID>() &&
                  *ptr->second.get_if<emit::FIntrepidNetworkGUID>() == pet->actor.guid, "PetArray points to the spawned actor");
        }
        CHECK(r.m.has_effect(d.effect), "the client received the pet's summon effect");
        CHECK(r.h.s->actor_view()->channel_of(id) != 0, "the pet opened on an actor channel for the client");
        previous = id;
    }
    r.h.s->set_current_value(kManaStat, 10.0f);
    r.trigger(pets::kDefinitions[0].ability);
    r.play(8.8, false);
    CHECK(r.h.s->summoned_pet() == previous && r.h.fx.world->registry().find(previous), "a summon the Summoner cannot pay for leaves the current pet");
    repl::RpcParams params;
    const auto& pawn = class_named("PlayerPawn_C");
    const auto* command = pawn.rpc("ServerSendPetCommand");
    CHECK(command && repl::set_rpc_param(*command, params, "PetCommandId", repl::Value::i64(pets::kDismiss)), "the dismiss command writes");
    r.send_reliable(actor_bunch(pawn, "ServerSendPetCommand", params));
    r.play(0.5, false);
    CHECK(!r.h.s->summoned_pet() && !r.h.fx.world->registry().find(previous), "dismiss removes the pet from the world");
    const auto* array = summoned_pet_array(*r.h.s);
    CHECK(array && array->elements.empty(), "dismiss clears the player's replicated PetArray");
    CHECK(!r.m.has_effect(pets::kDefinitions[2].effect), "dismiss clears the client's summon effect");

    refill_mana();
    r.trigger(pets::kDefinitions[0].ability);
    r.play(8.8, false);
    const uint64_t final_pet = r.h.s->summoned_pet();
    CHECK(final_pet != 0, "a dismissed pet can be summoned again");
    r.h.s->finish("pet cleanup test");
    CHECK(!r.h.fx.world->registry().find(final_pet), "logout removes the summoned pet");
    CHECK(!r.h.s->abilities_failed() && r.m.unreadable == 0, "all owner replication updates read successfully");

    Run death(0, true, &rec, prepare);
    death.h.s->teach_ability(pets::kDefinitions[0].ability);
    death.play(0.5, false);
    death.trigger(pets::kDefinitions[0].ability);
    death.play(8.8, false);
    const uint64_t dead_pet = death.h.s->summoned_pet();
    CHECK(dead_pet != 0, "the death test starts with a summoned pet");
    death.h.s->take_damage(death.h.conn, 1.0e9, death.h.t);
    death.play(0.5, false);
    CHECK(!death.h.s->summoned_pet() && !death.h.fx.world->registry().find(dead_pet), "the owner's death removes the pet");
    CHECK(!death.m.has_effect(pets::kDefinitions[0].effect), "the owner's death clears the summon effect");
}

static void test_summoner_spawn_failure() {
    const svc::Character rec = class25(6, "Summoner", 17753);
    Run r(0, true, &rec);
    const auto& d = pets::kDefinitions[0];
    r.h.s->teach_ability(d.ability);
    r.play(0.5, false);
    r.trigger(d.ability);
    r.play(8.8, false);
    CHECK(!r.h.s->summoned_pet() && !r.m.has_effect(d.effect), "a missing pet template leaves no phantom pet effect");
    CHECK(!r.h.s->abilities_failed(), "a missing pet template does not disable abilities");
}
