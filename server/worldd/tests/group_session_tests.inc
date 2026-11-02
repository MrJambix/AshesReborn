// SPDX-License-Identifier: AGPL-3.0-only
// Uses the fake client/connection Harness from test_worldd_session.cpp.
// Parties through the client's real calls: session/group.h has the rules, test_worldd_group tests them.

namespace {

struct GroupFrame {
    repl::RpcParams params;
    const schema::RpcSchema* rpc = nullptr;
    const repl::Value* value(std::string_view path) const { return repl::rpcf::value_of(*rpc, params, path); }
    uint64_t type() const { const auto* v = value("NewGroup.GroupType"); return v && v->get_if<uint64_t>() ? *v->get_if<uint64_t>() : 99; }
    group::Guid guid(const char* base) const {
        group::Guid g{};
        static constexpr const char* kWords[] = {".A", ".B", ".C", ".D"};
        for (size_t i = 0; i < 4; ++i) {
            const auto* v = value(std::string(base) + kWords[i]);
            if (v && v->get_if<int64_t>()) g[i] = static_cast<uint32_t>(*v->get_if<int64_t>());
        }
        return g;
    }
    const repl::ArrayValue* squads() const { const auto* v = value("NewGroup.Squads"); return v ? v->get_if<repl::ArrayValue>() : nullptr; }
    /// The members of the first squad.
    std::vector<std::string> first_squad() const {
        std::vector<std::string> names;
        const auto* list = squads();
        if (!list || list->elements.empty()) return names;
        const auto it = list->elements[0].values.find(2);
        const auto* members = it == list->elements[0].values.end() ? nullptr : it->second.get_if<repl::ArrayValue>();
        if (members) for (const auto& m : members->elements) names.push_back(*m.values.at(1).get_if<std::string>());
        return names;
    }
};

/// The calls named `what` that reached this client after `first`, their parameters decoded.
std::vector<GroupFrame> group_calls(const Harness& h, size_t first, std::string_view what) {
    std::vector<GroupFrame> out;
    const auto& cls = class_named("AoCPlayerControllerBP_C");
    for (const auto* delivered : h.on_channel(h.s->channel_of(bs::Role::PlayerController), first)) {
        const auto blocks = repl::read_content_blocks(delivered->content.data(), delivered->content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& block : *blocks) {
            if (block.header.has_rep_layout) continue;
            size_t pos = block.payload_pos;
            const auto calls = repl::read_rpc_fields(cls, delivered->content.data(), pos + block.payload_bits, pos);
            if (!calls) continue;
            for (const auto& call : *calls) {
                if (std::string_view(call.rpc->name) != what || !call.decoded) continue;
                out.push_back(GroupFrame{call.params, call.rpc});
            }
        }
    }
    return out;
}

}  // namespace

void test_group_wire() {
    std::printf("ClientUpdateGroup as the client reads it\n");
    Harness a;
    CHECK(a.join(), "joined");
    if (!a.s) return;
    a.settle();
    auto& pb = const_cast<bs::PlayerBootstrap&>(a.s->bootstrap());
    // Retail's empty group of a login is the only recording of this call: the encoder must say exactly the same.
    const auto captured = pb.empty_group();
    const auto ours = pb.group_update(group::View{});
    CHECK(captured && ours, "both forms build");
    if (captured && ours) {
        const size_t bits = captured->emission.payload.bit_pos();
        bool same = bits == ours->emission.payload.bit_pos();
        for (size_t i = 0; same && i < (bits + 7) / 8; ++i) same = captured->emission.payload.data()[i] == ours->emission.payload.data()[i];
        CHECK(same, "an empty group is bit for bit retail's (pkt#606, 527 bits)");
    }
    group::View v;
    v.guid = {1, 2, 3, 4};
    v.leader = {5, 6, 7, 8};
    v.type = group::kRaid;
    v.settings.shout_privilege = group::kShoutSquadLeaders;
    group::Person p;
    p.name = "Ann";
    p.guid = {5, 6, 7, 8};
    p.character_class = 3;
    p.vitals.health_percent = 0.25f;
    p.vitals.max_health = 400;
    p.vitals.level = 17;
    p.vitals.dead = true;
    p.vitals.x = 1000; p.vitals.y = -2000; p.vitals.z = 30;
    p.vitals.marker = std::array<double, 3>{1, 2, 3};
    v.people.push_back(p);
    group::SquadView q;
    q.number = 2;
    q.members.push_back(0);
    q.leaders.push_back(p.guid);
    v.squads.push_back(q);
    const auto filled = pb.group_update(v);
    CHECK(filled.has_value(), "a group with a member builds");
    if (!filled) return;
    const auto blocks = repl::read_content_blocks(filled->emission.payload.data(), filled->emission.payload.bit_pos(), 0);
    CHECK(blocks && blocks->size() == 1, "one block");
    if (!blocks || blocks->size() != 1) return;
    size_t pos = (*blocks)[0].payload_pos;
    const auto calls = repl::read_rpc_fields(class_named("AoCPlayerControllerBP_C"), filled->emission.payload.data(), pos + (*blocks)[0].payload_bits, pos);
    CHECK(calls && calls->size() == 1 && (*calls)[0].decoded && pos == (*blocks)[0].payload_pos + (*blocks)[0].payload_bits, "it reads back to its last bit");
    if (!calls || calls->size() != 1 || !(*calls)[0].decoded) return;
    const GroupFrame f{(*calls)[0].params, (*calls)[0].rpc};
    CHECK(f.guid("NewGroup.GroupGuid") == v.guid && f.guid("NewGroup.LeaderGuid") == v.leader && f.type() == group::kRaid, "the group's guid, leader and type");
    const auto* squads = f.squads();
    CHECK(squads && squads->elements.size() == 1 && squads->elements[0].values.at(1) == repl::Value::i64(2), "the squad's number");
    if (!squads || squads->elements.empty()) return;
    const auto* members = squads->elements[0].values.at(2).get_if<repl::ArrayValue>();
    CHECK(members && members->elements.size() == 1, "the squad's member");
    if (members && members->elements.size() == 1) {
        const auto& m = members->elements[0].values;
        CHECK(m.at(1) == repl::Value::str("Ann") && m.at(6) == repl::Value::u64(3) && m.at(7) == repl::Value::boolean(true), "name, class, and online as given");
        CHECK(m.at(8) == repl::Value::f32(0.25f) && m.at(9) == repl::Value::f32(400) && m.at(12) == repl::Value::i64(17) && m.at(13) == repl::Value::u64(1), "health, level and the dead status");
    }
    const auto* leaders = squads->elements[0].values.at(3).get_if<repl::ArrayValue>();
    CHECK(leaders && leaders->elements.size() == 1 && leaders->elements[0].values.at(4) == repl::Value::i64(8), "the squad's leader");
}

void test_party_session() {
    std::printf("a party through the client's calls\n");
    Harness a, b;
    CHECK(a.join() && b.join(second_record(), a.fx.world.get()), "two players in one world");
    if (!a.s || !b.s) return;
    a.settle(); b.settle();
    const auto& cls = class_named("AoCPlayerControllerBP_C");
    auto send = [&](Harness& who, const char* what, repl::RpcParams params) {
        who.t += seconds(1);
        who.client_sends({who.client.actor(who.s->channel_of(bs::Role::PlayerController), client_rpc(cls, what, std::move(params)), true)});
        a.s->tick(a.conn, a.t);   // the timed tick the live endpoint runs: it is what drains the group
        if (b.s) b.s->tick(b.conn, b.t);
        a.settle();
        if (b.s) b.settle();
    };
    auto text = [&](const char* what, const char* param, const std::string& value) {
        repl::RpcParams p;
        repl::set_rpc_param(*cls.rpc(what), p, param, repl::Value::str(value));
        return p;
    };
    const std::string b_name = b.s->name(), a_name = a.s->name();

    size_t ai = a.got.size(), bi = b.got.size();
    send(a, "ServerInviteToGroup", text("ServerInviteToGroup", "CharacterName", b_name));
    const auto asked = group_calls(b, bi, "ClientRequestJoinGroup");
    CHECK(asked.size() == 1, "the invited player is asked");
    if (asked.size() != 1) return;
    const auto* inviter = asked[0].value("InviterName");
    CHECK(inviter && *inviter == repl::Value::str(a_name), "by name");
    repl::RpcParams accept;
    static constexpr const char* kWords[] = {"GroupGuid.A", "GroupGuid.B", "GroupGuid.C", "GroupGuid.D"};
    for (const char* word : kWords) {
        const auto* v = asked[0].value(word);
        CHECK(v != nullptr, "the invitation names its group");
        if (v) repl::set_rpc_param(*cls.rpc("ServerAcceptJoinGroup"), accept, word, *v);
    }
    repl::set_rpc_param(*cls.rpc("ServerAcceptJoinGroup"), accept, "InviterName", repl::Value::str(a_name));
    ai = a.got.size(); bi = b.got.size();
    send(b, "ServerAcceptJoinGroup", accept);
    auto seen_a = group_calls(a, ai, "ClientUpdateGroup"), seen_b = group_calls(b, bi, "ClientUpdateGroup");
    CHECK(!seen_a.empty() && !seen_b.empty(), "both hear of the group");
    if (seen_a.empty() || seen_b.empty()) return;
    CHECK(seen_a.back().first_squad() == (std::vector<std::string>{a_name, b_name}), "the leader sees both, in order");
    CHECK(seen_b.back().first_squad() == (std::vector<std::string>{a_name, b_name}), "the new member sees both");
    CHECK(seen_b.back().guid("NewGroup.LeaderGuid") == a.s->bootstrap().setup().character_guid, "and who leads");
    CHECK(seen_a.back().type() == group::kParty, "as a party");

    bi = b.got.size();
    a.client_sends({a.client.actor(a.s->channel_of(bs::Role::PlayerController),
                    client_rpc(cls, "SendChatMessage", chat_params(*cls.rpc("SendChatMessage"), "to the party", group::kGroupSay)), true)});
    a.t += seconds(1);
    a.s->tick(a.conn, a.t); b.s->tick(b.conn, b.t);
    a.settle(); b.settle();
    const auto heard = received_chat(b, bi);
    CHECK(heard.size() == 1 && heard[0].values.at(1) == repl::Value::str("to the party") && heard[0].values.at(2) == repl::Value::str(a_name), "party chat reaches the party with its sender's name");

    ai = a.got.size(); bi = b.got.size();
    send(a, "ServerKickFromGroup", text("ServerKickFromGroup", "CharacterNameToKick", "nobody-here"));
    CHECK(group_calls(b, bi, "ClientUpdateGroup").empty(), "a kick naming a stranger changes nothing");
    bi = b.got.size();
    send(b, "ServerKickFromGroup", text("ServerKickFromGroup", "CharacterNameToKick", a_name));
    CHECK(a.s->group_book_for_test()->same_group(a.s->bootstrap().setup().character_guid, b.s->bootstrap().setup().character_guid), "a member cannot kick the leader");

    ai = a.got.size(); bi = b.got.size();
    send(b, "ServerLeaveGroup", repl::RpcParams{});
    seen_a = group_calls(a, ai, "ClientUpdateGroup");
    seen_b = group_calls(b, bi, "ClientUpdateGroup");
    CHECK(!seen_a.empty() && seen_a.back().guid("NewGroup.GroupGuid") == group::kNoGuid, "leaving a party of two ends it for the leader, with a zero guid");
    CHECK(!seen_b.empty() && seen_b.back().guid("NewGroup.GroupGuid") == group::kNoGuid, "and for the one who left");

    // Going away from the world keeps the place, and the session's end must tell the Book.
    send(a, "ServerInviteToGroup", text("ServerInviteToGroup", "CharacterName", b_name));
    const auto again = group_calls(b, bi, "ClientRequestJoinGroup");
    CHECK(!again.empty(), "invited again");
    if (again.empty()) return;
    accept = {};
    for (const char* word : kWords) repl::set_rpc_param(*cls.rpc("ServerAcceptJoinGroup"), accept, word, *again.back().value(word));
    repl::set_rpc_param(*cls.rpc("ServerAcceptJoinGroup"), accept, "InviterName", repl::Value::str(a_name));
    send(b, "ServerAcceptJoinGroup", accept);
    const auto book = a.s->group_book_for_test();
    const auto a_guid = a.s->bootstrap().setup().character_guid, b_guid = b.s->bootstrap().setup().character_guid;
    CHECK(book->same_group(a_guid, b_guid), "together again");
    ai = a.got.size();
    b.s.reset();
    a.t += seconds(1);
    a.s->tick(a.conn, a.t);
    a.settle();
    seen_a = group_calls(a, ai, "ClientUpdateGroup");
    CHECK(book->same_group(a_guid, b_guid), "a member whose session ended keeps his place for now");
    CHECK(!seen_a.empty() && !seen_a.back().first_squad().empty(), "and the leader hears of it");
    if (!seen_a.empty()) {
        const auto* squads = seen_a.back().squads();
        const auto* members = squads && !squads->elements.empty() ? squads->elements[0].values.at(2).get_if<repl::ArrayValue>() : nullptr;
        CHECK(members && members->elements.size() == 2 && members->elements[1].values.at(7) == repl::Value::boolean(false), "listed as offline");
    }
}
static void test_ignore_list() {
    std::printf("an ignore list for chat\n");
    chat::Room room;
    auto a = room.connect("Ann"), b = room.connect("Bob"), c = room.connect("Cat");
    CHECK(room.ignore(*a, "ann") != "", "nobody ignores himself");
    CHECK(room.ignore(*a, "bOB").empty() && room.ignores("ann", "Bob"), "a name in any case");
    CHECK(room.send(*b, "hello", chat::kGlobal, "").empty() && a->inbox.empty() && c->inbox.size() == 1, "public chat from him does not reach the one who ignores him, and reaches others");
    c->inbox.clear(); b->inbox.clear();
    CHECK(room.send(*b, "psst", chat::kTell, "Ann") != "" && a->inbox.empty(), "a whisper is refused, and the sender is told");
    CHECK(room.send(*a, "hi", chat::kTell, "Bob").empty() && b->inbox.size() == 1, "ignoring is one way");
    CHECK(room.ignored_by(*a).size() == 1, "the list holds him");
    CHECK(room.ignored_by(*a)[0] == "Bob", "...under his name as the online character spells it (the client's ignore window shows it)");
    CHECK(room.ignore(*a, "Somebody").empty() && room.ignored_by(*a).size() == 2 && room.ignored_by(*a)[1] == "Somebody", "a name nobody online has is kept as typed");
    CHECK(room.unignore(*a, "somebody"), "...and taken back in any case");
    auto a2 = room.connect("ANN");
    CHECK(room.ignores("Ann", "bob") && room.ignored_by(*a2).size() == 1, "and outlives the session");
    CHECK(room.unignore(*a, "bob") && !room.unignore(*a, "bob") && !room.ignores("Ann", "Bob"), "it can be taken back once");
}

static void test_crowd_state() {
    std::printf("stuns and roots as a player's stats say them\n");
    using combat::CrowdControlRow;
    using combat::CrowdStops;
    CHECK(crowd::combine({}) == crowd::State{}, "no effect: no lock, and no animation");
    const CrowdControlRow stun{1, 7, 0, 0.0, 300, CrowdStops::All, 0.0}, snare{2, 16, 255, 0.4, 0, CrowdStops::None, 0.0}, push{3, 0x28, 242, 0.0, 5, CrowdStops::None, 400.0};
    crowd::State s = crowd::combine({{&snare, 1, 2}, {&stun, 3, 4}});
    CHECK(s.status == (7 | 16) && s.snare == 0.4f, "two effects add their locks, and the strongest snare counts");
    CHECK(s.animation == 0 && s.record == 1 && s.start == 3 && s.end == 4, "the highest priority is what shows");
    CHECK(crowd::combine({{&push, 1, 2}}).status == 0, "the knockback bits are not sent: they start client code that needs fields nothing sends");
    CHECK(crowd::combine({{&stun, 3, std::numeric_limits<double>::infinity()}}).end == 0, "an infinite effect has no end to show");
    Harness a;
    CHECK(a.join(), "joined");
    if (!a.s) return;
    a.settle();
    auto& pb = const_cast<bs::PlayerBootstrap&>(a.s->bootstrap());
    CHECK(pb.set_crowd_control(s, 100, 104), "the stats component takes it");
    const auto* stats = pb.pawn().find(pb.setup().guids.stats);
    if (stats) {
        const auto* flags = stats->obj.state.get(*stats->obj.schema, "CrowdControlState.StatusFlags");
        const auto* end = stats->obj.state.get(*stats->obj.schema, "CrowdControlState.EndAnimTime");
        CHECK(flags && *flags == repl::Value::u64(7 | 16) && end && *end == repl::Value::f64(104), "the flags and the end time");
    }
    CHECK(pb.set_crowd_control(crowd::State{}, 0, 0), "and takes it away");
    if (stats) {
        const auto* flags = stats->obj.state.get(*stats->obj.schema, "CrowdControlState.StatusFlags");
        const auto* anim = stats->obj.state.get(*stats->obj.schema, "CrowdControlState.CurrentAnimationRequest");
        CHECK(flags && *flags == repl::Value::u64(0) && anim && *anim == repl::Value::u64(crowd::kAnimationNone), "leaving no lock and animation None");
    }
}

static void test_party_masks() {
    std::printf("Ally, Raid and Party in a hit's mask\n");
    combat::TargetInfo other;
    other.relation = combat::Relation::Friendly;
    other.other_player = true;
    constexpr int kAlly = 4, kParty = 8, kRaid = 16, kSelf = 1, kEnemy = 2;
    CHECK(combat::mask_reaches(kAlly, other), "Ally reaches any other player");
    CHECK(!combat::mask_reaches(kParty, other) && !combat::mask_reaches(kRaid, other) && !combat::mask_reaches(kSelf, other), "Party, Raid and Self do not reach a stranger");
    other.group = 1;
    CHECK(combat::mask_reaches(kRaid, other) && !combat::mask_reaches(kParty, other), "Raid reaches the raid, Party not another squad");
    other.group = 2;
    CHECK(combat::mask_reaches(kParty, other) && combat::mask_reaches(kRaid, other), "both reach the squad");
    combat::TargetInfo me;
    me.relation = combat::Relation::Friendly;
    CHECK(combat::mask_reaches(kParty, me) && combat::mask_reaches(kSelf, me), "the caster is in his own party");
    combat::TargetInfo foe;
    foe.relation = combat::Relation::Hostile;
    CHECK(combat::mask_reaches(kEnemy, foe) && !combat::mask_reaches(kAlly | kParty, foe), "an enemy only by Enemy");
}

static void test_party_loot_session() {
    std::printf("the leader's loot rules through the client's call\n");
    Harness a, b;
    CHECK(a.join() && b.join(second_record(), a.fx.world.get()), "two players in one world");
    if (!a.s || !b.s) return;
    a.settle(); b.settle();
    const auto& cls = class_named("AoCPlayerControllerBP_C");
    const auto book = a.s->group_book_for_test();
    auto send = [&](Harness& who, const char* what, repl::RpcParams params) {
        who.t += seconds(1);
        who.client_sends({who.client.actor(who.s->channel_of(bs::Role::PlayerController), client_rpc(cls, what, std::move(params)), true)});
        a.s->tick(a.conn, a.t);
        b.s->tick(b.conn, b.t);
        a.settle(); b.settle();
    };
    repl::RpcParams invite;
    repl::set_rpc_param(*cls.rpc("ServerInviteToGroup"), invite, "CharacterName", repl::Value::str(b.s->name()));
    send(a, "ServerInviteToGroup", invite);
    repl::RpcParams accept;
    const auto asked = group_calls(b, 0, "ClientRequestJoinGroup");
    CHECK(asked.size() == 1, "invited");
    if (asked.size() != 1) return;
    for (const char* word : {"GroupGuid.A", "GroupGuid.B", "GroupGuid.C", "GroupGuid.D"}) repl::set_rpc_param(*cls.rpc("ServerAcceptJoinGroup"), accept, word, *asked[0].value(word));
    repl::set_rpc_param(*cls.rpc("ServerAcceptJoinGroup"), accept, "InviterName", repl::Value::str(a.s->name()));
    send(b, "ServerAcceptJoinGroup", accept);
    repl::RpcParams rules;
    uint64_t v = 1;
    for (const char* path : {"Methods.DefaultLootAllocationMethod", "Methods.CommonLootAllocationMethod", "Methods.UncommonLootAllocationMethod", "Methods.RareLootAllocationMethod",
                             "Methods.HeroicLootAllocationMethod", "Methods.EpicLootAllocationMethod", "Methods.LegendaryLootAllocationMethod", "Methods.ArtifactLootAllocationMethod"}) {
        repl::set_rpc_param(*cls.rpc("ServerTrySetPartyLootAllocationMethods"), rules, path, repl::Value::u64(v));
        v = 3;
    }
    size_t bi = b.got.size();
    send(a, "ServerTrySetPartyLootAllocationMethods", rules);
    const auto seen = group_calls(b, bi, "ClientUpdateGroup");
    CHECK(!seen.empty(), "the member hears of them");
    if (!seen.empty()) {
        const auto* def = seen.back().value("NewGroup.PartySettings.LootAllocationMethods.DefaultLootAllocationMethod");
        const auto* rare = seen.back().value("NewGroup.PartySettings.LootAllocationMethods.RareLootAllocationMethod");
        CHECK(def && *def == repl::Value::u64(1) && rare && *rare == repl::Value::u64(3), "the group now says them");
    }
}

AOC_REGISTER_TEST(test_ignore_list);
AOC_REGISTER_TEST(test_crowd_state);
AOC_REGISTER_TEST(test_party_masks);
AOC_REGISTER_TEST(test_party_loot_session);
AOC_REGISTER_TEST(test_group_wire);
AOC_REGISTER_TEST(test_party_session);
