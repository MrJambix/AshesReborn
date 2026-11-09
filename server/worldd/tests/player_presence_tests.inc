// SPDX-License-Identifier: AGPL-3.0-only

static bool peer_delivered(const Harness& h, uint32_t channel, bool open, bool close, size_t from = 0) {
    return channel != 0 && std::any_of(h.got.begin() + from, h.got.end(), [&](const auto& d) {
        return d.channel == channel && d.open == open && d.close == close;
    });
}

static void test_guild_on_public_pawn() {
    std::printf("what other players are given of a guild\n");
    Harness h;
    h.start();
    h.settle();
    h.play();
    h.run(milliseconds(3000));
    auto& registry = h.fx.world->registry();
    const auto& guids = h.s->bootstrap().setup().guids;
    const auto component = h.s->guild_component_for_test();
    const auto* pawn = registry.find(guids.pawn.ObjectId);
    CHECK(pawn != nullptr, "the player is published");
    if (!pawn) return;
    CHECK(h.s->bootstrap().pawn().find(component) != nullptr, "his own pawn has the guild component by now");
    CHECK(pawn->actor.find(component) == nullptr, "someone in no guild shows others no guild component");
    h.s->guild_book_for_test()->create(*h.s->guild_member_for_test(), "Iron Wolves", 100);
    h.run(milliseconds(1500));
    pawn = registry.find(guids.pawn.ObjectId);
    const repl::Subobject* c = pawn ? pawn->actor.find(component) : nullptr;
    CHECK(c != nullptr, "someone in a guild does");
    if (!c) return;
    const auto* name = c->obj.state.get(*c->obj.schema, "GuildName");
    CHECK(name && *name == repl::Value::str("Iron Wolves"), "with its name, which is what the client's IsInGuild reads");
    const auto* whole = c->obj.state.get(*c->obj.schema, "GuildData");
    const auto* data = whole ? whole->get_if<repl::GuildDataWire>() : nullptr;
    CHECK(data && data->name == "Iron Wolves" && (data->guid[0] | data->guid[1] | data->guid[2] | data->guid[3]) != 0, "its name and guid in GuildData, which GetGuildName and IsGuildmate read");
    CHECK(data && data->members.empty() && data->ranks.empty(), "and not its members or ranks");
    CHECK(!c->obj.state.get(*c->obj.schema, "GuildCooldown") && !c->obj.state.get(*c->obj.schema, "GuildMemberData.bIsOnline"), "nor the cooldown or the member's own row");
    const auto* own = h.s->bootstrap().pawn().find(component);
    const auto* own_value = own ? own->obj.state.get(*own->obj.schema, "GuildData") : nullptr;
    const auto* own_data = own_value ? own_value->get_if<repl::GuildDataWire>() : nullptr;
    CHECK(own_data && own_data->members.size() == 1 && own_data->ranks.size() == 3, "his own copy keeps all of it");
}

static void test_player_presence() {
    std::printf("players: public projection, two connections, movement, range and logout\n");
    Harness first;
    auto other = cyb_record(); other.id = guid_n(801); other.name = "AshesTester";
    Harness second(other);
    first.start(); second.start(first.fx.world.get());
    first.settle(); second.settle();
    auto& registry = first.fx.world->registry();
    const auto g1 = first.s->bootstrap().setup().guids, g2 = second.s->bootstrap().setup().guids;
    CHECK(registry.count(world::ActorKind::PlayerPawn) == 0 && registry.count(world::ActorKind::PlayerState) == 0,
          "joining players are not published before phase-in");
    CHECK(!first.s->welcome_line_sent(), "no welcome line before the phase-in");
    first.play(); second.play();
    first.run(milliseconds(1200)); second.run(milliseconds(1200));
    {   // the welcome line (core's 29a800f, on our chat): once the phase-in is built, from "Server" as a server broadcast (category 124)
        size_t welcomes = 0;
        for (const repl::RpcCall& c : pc_calls_since(first, 0)) {
            if (!c.rpc || std::string(c.rpc->name) != "ClientReceiveChatMessages") continue;
            const auto it = c.params.values.find(1);
            const auto* list = it == c.params.values.end() ? nullptr : it->second.get_if<repl::ArrayValue>();
            for (const auto& m : list ? list->elements : std::vector<repl::ElementState>{}) {
                welcomes += m.values.count(1) && m.values.at(1) == repl::Value::str("Welcome to Ashes Reborn.") && m.values.count(2) &&
                            m.values.at(2) == repl::Value::str("Server") && m.values.count(3) && m.values.at(3) == repl::Value::u64(124);
            }
        }
        CHECK(first.s->welcome_line_sent() && welcomes == 1, "after the phase-in the player gets the welcome line once, from the server");
    }
    const auto* p1 = registry.find(g1.pawn.ObjectId);
    const auto* p2 = registry.find(g2.pawn.ObjectId);
    const auto* ps2 = registry.find(g2.player_state.ObjectId);
    CHECK(p1 && p2 && ps2 && registry.count(world::ActorKind::PlayerPawn) == 2,
          "both phased-in players have public pawn and player-state actors");
    if (!p1 || !p2 || !ps2) return;
    const auto* v1 = first.s->actor_view(); const auto* v2 = second.s->actor_view();
    CHECK(v1->state_of(g2.pawn.ObjectId) == world::ClientState::Awake && v2->state_of(g1.pawn.ObjectId) == world::ClientState::Awake,
          "both players see the other player's pawn through their independent actor views");
    CHECK(v1->state_of(g1.pawn.ObjectId) == world::ClientState::None && v1->state_of(g1.player_state.ObjectId) == world::ClientState::None
          && v2->state_of(g2.pawn.ObjectId) == world::ClientState::None,
          "owners never open duplicate copies of their own pawn or player state");
    const auto remote = v1->channel_of(g2.pawn.ObjectId), remote_ps = v1->channel_of(g2.player_state.ObjectId);
    CHECK(peer_delivered(first, remote, true, false) && peer_delivered(first, remote_ps, true, false),
          "a fake client receives actual open packets for the peer's pawn and player state");
    const auto& obj = p2->actor.obj;
    const auto* role = obj.state.get(*obj.schema, "RemoteRole");
    const auto* ps = obj.state.get(*obj.schema, "PlayerState");
    CHECK(role && role->get_if<uint64_t>() && *role->get_if<uint64_t>() == 1 && ps && ps->get_if<emit::FIntrepidNetworkGUID>()
          && *ps->get_if<emit::FIntrepidNetworkGUID>() == g2.player_state,
          "the peer is a simulated proxy linked to its public player state");
    CHECK(!obj.state.get(*obj.schema, "Owner") && !obj.state.get(*obj.schema, "Controller")
          && !ps2->actor.obj.state.get(*ps2->actor.obj.schema, "UniqueId"),
          "public actors do not expose the private controller or login identity");
    const auto* name = ps2->actor.obj.state.get(*ps2->actor.obj.schema, "PlayerNamePrivate");
    CHECK(name && name->get_if<std::string>() && *name->get_if<std::string>() == other.name,
          "the public player state uses the character's display name");
    CHECK(p2->actor.find(g2.base_character_info) && p2->actor.find(g2.appearance) && p2->actor.find(g2.combat_info)
          && p2->actor.find(g2.stats) && p2->actor.find(g2.ability),
          "the public pawn includes appearance, equipment, combat state, stats and active abilities");
    bool private_component = false, private_stat = false, private_ability = false;
    size_t public_emote_records = 0;
    for (const auto& sub : p2->actor.subobjects) {
        const std::string name = sub.name;
        if (name.find("Storage") != std::string::npos || name == "Backpack" || name == "Equipment" || name == "OwnerInfo" || name == "XPTracker") private_component = true;
        for (const auto& [field, array] : sub.obj.arrays.all()) {
            const std::string field_name = sub.obj.schema->field(field)->name;
            if (field_name == "StatRepInt32Owner" || field_name == "StatRepFloatArrayOwner") private_stat = true;
        }
        if (sub.guid == g2.ability && sub.obj.state.get(*sub.obj.schema, "KnownAbilities")) private_ability = true;
        // the records to load go only as far as the emote abilities (the other clients need them to play the owner's emotes, session/player_presence.cpp)
        if (sub.guid == g2.ability) {
            if (const auto* records = sub.obj.state.get(*sub.obj.schema, "ReplicatedRecordsToLoad")) {
                const auto* list = records->get_if<repl::ArrayValue>();
                if (!list) private_ability = true;
                for (const auto& e : list ? list->elements : std::vector<repl::ElementState>{}) {
                    const auto g = e.values.find(1);
                    if (g == e.values.end() || !g->second.get_if<int64_t>() || !content::is_emote_ability(*g->second.get_if<int64_t>())) private_ability = true;
                    else ++public_emote_records;
                }
            }
        }
    }
    CHECK(!private_component && !private_stat && !private_ability,
          "inventory, currencies, XP, owner-only stats and learned-ability lists are absent");
    {   // ... but the emote records the owner's component loads are there, and only they
        size_t owner_emote_records = 0;
        if (const auto* own = second.s->bootstrap().pawn().find(g2.ability)) {
            if (const auto* records = own->obj.state.get(*own->obj.schema, "ReplicatedRecordsToLoad")) {
                if (const auto* list = records->get_if<repl::ArrayValue>()) {
                    for (const auto& e : list->elements) {
                        const auto g = e.values.find(1);
                        owner_emote_records += g != e.values.end() && g->second.get_if<int64_t>() && content::is_emote_ability(*g->second.get_if<int64_t>());
                    }
                }
            }
        }
        std::printf("  [info] the owner's component loads %zu emote records; the public pawn names %zu\n", owner_emote_records, public_emote_records);
        CHECK(public_emote_records == owner_emote_records, "the public pawn names the emote records the owner's component loads, for the other clients to play its emotes");
    }
    const uint32_t version = p2->version, ps_version = ps2->version, reliable_version = p2->reliable_version;
    second.run(milliseconds(1000));
    CHECK(registry.find(g2.pawn.ObjectId)->version == version && registry.find(g2.player_state.ObjectId)->version == ps_version,
          "an idle player does not continuously dirty public actors");

    const GoodPosition start = second.s->last_position();
    second.s->gm_teleport(second.conn, start.x + 120, start.y + 40, start.z, second.t, "presence test");
    second.run(milliseconds(100));
    const size_t before_move = first.got.size();
    first.run(milliseconds(500));
    const auto* moved = registry.find(g2.pawn.ObjectId);
    bool movement_read = false, unreliable = false;
    for (size_t i = before_move; i < first.got.size(); ++i) {
        const auto& packet = first.got[i];
        if (packet.channel != remote || packet.open || packet.close) continue;
        const auto blocks = repl::read_content_blocks(packet.content.data(), packet.content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& b : *blocks) {
            if (!b.header.is_actor || !b.header.has_rep_layout) continue;
            size_t position = b.payload_pos;
            const auto decoded = repl::read_rep_block(*obj.schema, packet.content.data(), b.payload_pos + b.payload_bits, position, obj.opts);
            const auto* value = decoded ? decoded->get(*obj.schema, "ReplicatedMovement") : nullptr;
            const auto* movement = value ? value->get_if<repl::RepMovement>() : nullptr;
            if (movement) {
                const auto location = repl::dequantize_location(obj.opts, movement->location);
                movement_read = std::abs(location.x - start.x - 120) < 1 && std::abs(location.y - start.y - 40) < 1;
                unreliable = unreliable || !packet.reliable;
            }
        }
    }
    CHECK(moved && moved->x == start.x + 120 && moved->y == start.y + 40 && movement_read && unreliable,
          "validated movement reaches the other client as a readable, unreliable position update");
    CHECK(moved && moved->reliable_version == reliable_version,
          "movement retains the reliable state counter");

    second.s->gm_teleport(second.conn, start.x + 100000, start.y, start.z, second.t, "presence range test");
    second.run(milliseconds(100)); first.run(milliseconds(1000));
    CHECK(v1->state_of(g2.pawn.ObjectId) == world::ClientState::None && peer_delivered(first, remote, false, true),
          "leaving relevancy closes the peer's old channel");
    second.s->gm_teleport(second.conn, start.x, start.y, start.z, second.t, "presence return test");
    second.run(milliseconds(100)); first.run(milliseconds(1000));
    const auto returned = v1->channel_of(g2.pawn.ObjectId);
    CHECK(returned && v1->state_of(g2.pawn.ObjectId) == world::ClientState::Awake && peer_delivered(first, returned, true, false),
          "returning to range opens the player again with a current snapshot");
    const auto returned_ps = v1->channel_of(g2.player_state.ObjectId);
    second.s->finish("presence logout test"); first.run(milliseconds(1000));
    CHECK(!registry.find(g2.pawn.ObjectId) && !registry.find(g2.player_state.ObjectId)
          && peer_delivered(first, returned, false, true) && peer_delivered(first, returned_ps, false, true),
          "logout removes both public actors and closes both channels for the remaining player");
    second.s->finish("idempotent logout test");
    Harness reconnect(other); reconnect.start(first.fx.world.get()); reconnect.play();
    reconnect.run(milliseconds(100)); first.run(milliseconds(500));
    const auto again = reconnect.s->bootstrap().setup().guids;
    CHECK(again.pawn != g2.pawn && registry.count(world::ActorKind::PlayerPawn) == 2
          && v1->state_of(again.pawn.ObjectId) == world::ClientState::Awake && !registry.find(g2.pawn.ObjectId),
          "reconnecting uses fresh actor IDs and leaves no ghost player");
}
