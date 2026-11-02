// SPDX-License-Identifier: AGPL-3.0-only
// Uses the fake client/connection Harness from test_worldd_session.cpp.
// Guilds through the client's real calls on the pawn's GuildComponent: session/guild.h has the rules, test_worldd_guild tests them.

#include "session/guild_saver.h"

#include <mutex>

namespace {

/// The client's bunch for an RPC of the pawn's GuildComponent: a content block of the subobject, client form.
BunchWriter guild_rpc(const emit::FIntrepidNetworkGUID& component, const char* name, const repl::RpcParams& params = {}) {
    const schema::ClassSchema& cls = class_named("UIntrepidGuildComponent");
    const schema::RpcSchema* rpc = cls.rpc(name);
    BunchWriter out;
    if (!rpc) {
        std::printf("  [FAIL] no RPC %s\n", name);
        return out;
    }
    BunchWriter field;
    bool failed = false;
    if (repl::write_rpc_field(cls, *rpc, params, field, {}, &failed) == 0 || failed) {
        std::printf("  [FAIL] %s does not write\n", name);
        return out;
    }
    repl::BlockHeader header;
    header.is_actor = false;
    header.has_rep_layout = false;
    header.object = component;
    repl::write_content_block(out, header, field, /*server_form=*/false);
    return out;
}

/// The calls of the guild component named `what` that reached this client after `first`.
std::vector<repl::RpcParams> guild_calls(const Harness& h, size_t first, std::string_view what) {
    std::vector<repl::RpcParams> out;
    const auto& cls = class_named("UIntrepidGuildComponent");
    for (const auto* delivered : h.on_channel(h.s->channel_of(bs::Role::Pawn), first)) {
        const auto blocks = repl::read_content_blocks(delivered->content.data(), delivered->content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& block : *blocks) {
            if (block.header.has_rep_layout || block.header.is_actor || !(block.header.object == h.s->guild_component_for_test())) continue;
            size_t pos = block.payload_pos;
            const auto calls = repl::read_rpc_fields(cls, delivered->content.data(), pos + block.payload_bits, pos);
            if (!calls) continue;
            for (const auto& call : *calls) {
                if (std::string_view(call.rpc->name) == what && call.decoded) out.push_back(call.params);
            }
        }
    }
    return out;
}

/// How many PlayerController calls of `what` reached this client after `first`.
size_t pc_calls(const Harness& h, size_t first, std::string_view what) {
    size_t n = 0;
    const auto& cls = class_named("AoCPlayerControllerBP_C");
    for (const auto* delivered : h.on_channel(h.s->channel_of(bs::Role::PlayerController), first)) {
        const auto blocks = repl::read_content_blocks(delivered->content.data(), delivered->content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& block : *blocks) {
            if (block.header.has_rep_layout) continue;
            size_t pos = block.payload_pos;
            const auto calls = repl::read_rpc_fields(cls, delivered->content.data(), pos + block.payload_bits, pos);
            if (!calls) continue;
            for (const auto& call : *calls) n += std::string_view(call.rpc->name) == what;
        }
    }
    return n;
}

}  // namespace

static void test_guild_session() {
    std::printf("a guild through the client's calls\n");
    const schema::ClassSchema& gc = class_named("UIntrepidGuildComponent");
    CHECK(gc.rpc("ServerCreateGuild") && gc.rpc("ClientInvitePlayerToGuild") && gc.command("GuildName"), "the schema has the guild's calls and name");
    CHECK(schema::gen::find_component_schema("GuildComponent") == &gc || (schema::gen::find_component_schema("GuildComponent") && std::string_view(schema::gen::find_component_schema("GuildComponent")->name) == "UIntrepidGuildComponent"),
          "the pawn's GuildComponent finds it");
    Harness a, b;
    CHECK(a.join() && b.join(second_record(), a.fx.world.get()), "two players in one world");
    if (!a.s || !b.s) return;
    a.settle(); b.settle();
    const auto tick = [&] {
        a.t += seconds(1); b.t += seconds(1);
        a.s->tick(a.conn, a.t); b.s->tick(b.conn, b.t);
        a.settle(); b.settle();
    };
    tick();
    tick();
    tick();   // the component opens two seconds after the join
    const auto book = a.s->guild_book_for_test();
    const auto a_guid = a.s->bootstrap().setup().character_guid, b_guid = b.s->bootstrap().setup().character_guid;
    const std::string a_name = a.s->name(), b_name = b.s->name();
    const auto& pc = class_named("AoCPlayerControllerBP_C");
    auto send = [&](Harness& who, const char* what, repl::RpcParams params = {}) {
        who.client_sends({who.client.actor(who.s->channel_of(bs::Role::Pawn), guild_rpc(who.s->guild_component_for_test(), what, params), true)});
        tick();
    };
    auto text = [&](const char* what, const char* param, const std::string& value) {
        repl::RpcParams p;
        repl::set_rpc_param(*gc.rpc(what), p, param, repl::Value::str(value));
        return p;
    };
    auto say = [&](Harness& who, const std::string& line, uint8_t category) {
        who.client_sends({who.client.actor(who.s->channel_of(bs::Role::PlayerController), client_rpc(pc, "SendChatMessage", chat_params(*pc.rpc("SendChatMessage"), line, category)), true)});
        tick();
    };

    size_t ai = a.got.size();
    say(a, ".guild", chat::kSay);
    CHECK(pc_calls(a, ai, "ClientOpenCreateGuildPanel") == 1, ".guild opens the window to found a guild");

    ai = a.got.size();
    send(a, "ServerCreateGuild", text("ServerCreateGuild", "InGuildName", "x1"));
    CHECK(guild_calls(a, ai, "ClientGuildRequestFailed").size() == 1 && !book->in_guild(a_guid), "a bad name is refused through the client's call");
    ai = a.got.size();
    send(a, "ServerCreateGuild", text("ServerCreateGuild", "InGuildName", "Iron Wolves"));
    const auto founded = guild_calls(a, ai, "ClientGuildRequestSucceeded");
    CHECK(founded.size() == 1 && book->in_guild(a_guid), "a good one founds the guild, and says so");
    if (!founded.empty()) {
        const auto* rpc = gc.rpc("ClientGuildRequestSucceeded");
        const auto* type = repl::rpcf::value_of(*rpc, founded[0], "RequestType");
        CHECK(type && *type == repl::Value::u64(guild::kReqCreate), "as a CreateGuild");
    }
    ai = a.got.size();
    say(a, ".guild", chat::kSay);
    CHECK(pc_calls(a, ai, "ClientOpenCreateGuildPanel") == 0, "someone in a guild is not offered another");

    // The state itself: the component's properties on the pawn as the replicator holds them.
    const auto* comp = a.s->bootstrap().pawn().find(a.s->guild_component_for_test());
    CHECK(comp != nullptr, "the pawn has the component");
    if (comp) {
        const auto* gn = comp->obj.state.get(*comp->obj.schema, "GuildName");
        CHECK(gn && *gn == repl::Value::str("Iron Wolves"), "GuildName, which is what the client's IsInGuild reads");
        const auto* data_value = comp->obj.state.get(*comp->obj.schema, "GuildData");
        const auto* data = data_value ? data_value->get_if<repl::GuildDataWire>() : nullptr;
        CHECK(data && data->name == "Iron Wolves" && data->capacity == 50 && data->level_cap == 10, "GuildData, as one value");
        CHECK(data && data->ranks.size() == 3 && data->ranks[0].name == "Guild Leader", "the three ranks");
        CHECK(data && data->ranks.size() == 3 && data->ranks[2].permissions[guild::kWriteChat] && !data->ranks[2].permissions[guild::kKickPlayer], "whose permissions are in the schema's order");
        CHECK(data && data->members.size() == 1 && data->members[0].name == a_name && data->members[0].online, "the founder, online");
    }

    size_t bi = b.got.size();
    ai = a.got.size();
    send(a, "ServerInvitePlayerToGuild", text("ServerInvitePlayerToGuild", "InviteeName", b_name));
    const auto asked = guild_calls(b, bi, "ClientInvitePlayerToGuild");
    CHECK(asked.size() == 1, "the invited player is asked");
    if (asked.size() == 1) {
        const auto* rpc = gc.rpc("ClientInvitePlayerToGuild");
        CHECK(*repl::rpcf::value_of(*rpc, asked[0], "SourceCharacterName") == repl::Value::str(a_name) && *repl::rpcf::value_of(*rpc, asked[0], "InGuildName") == repl::Value::str("Iron Wolves"), "by whom and to what");
    }
    send(b, "ServerAcceptInviteToGuild");
    CHECK(book->same_guild(a_guid, b_guid), "accepting joins the guild");
    if (const auto* bc = b.s->bootstrap().pawn().find(b.s->guild_component_for_test())) {
        const auto* gn = bc->obj.state.get(*bc->obj.schema, "GuildName");
        CHECK(gn && *gn == repl::Value::str("Iron Wolves"), "and the new member's client is told");
    }

    bi = b.got.size();
    say(a, "to the guild", guild::kGuildSay);
    const auto heard = received_chat(b, bi);
    CHECK(heard.size() == 1 && heard[0].values.at(1) == repl::Value::str("to the guild") && heard[0].values.at(2) == repl::Value::str(a_name) && heard[0].values.at(3) == repl::Value::u64(guild::kGuildSay), "guild chat reaches the guild");

    repl::RpcParams kick;
    static constexpr const char* kWords[] = {"MemberGuidToKick.A", "MemberGuidToKick.B", "MemberGuidToKick.C", "MemberGuidToKick.D"};
    for (size_t i = 0; i < 4; ++i) repl::set_rpc_param(*gc.rpc("ServerKickPlayerFromGuild"), kick, kWords[i], repl::Value::i64(static_cast<int32_t>(a_guid[i])));
    bi = b.got.size();
    send(b, "ServerKickPlayerFromGuild", kick);
    CHECK(book->same_guild(a_guid, b_guid) && guild_calls(b, bi, "ClientGuildRequestFailed").size() == 1, "a member cannot remove the leader, and is told");
    kick = {};
    for (size_t i = 0; i < 4; ++i) repl::set_rpc_param(*gc.rpc("ServerKickPlayerFromGuild"), kick, kWords[i], repl::Value::i64(static_cast<int32_t>(b_guid[i])));
    send(a, "ServerKickPlayerFromGuild", kick);
    CHECK(!book->in_guild(b_guid), "the leader can remove a member");
    if (const auto* bc = b.s->bootstrap().pawn().find(b.s->guild_component_for_test())) {
        const auto* gn = bc->obj.state.get(*bc->obj.schema, "GuildName");
        const auto* cooldown = bc->obj.state.get(*bc->obj.schema, "GuildCooldown");
        CHECK(gn && *gn == repl::Value::str(""), "whose client hears he has no guild");
        CHECK(cooldown && cooldown->get_if<repl::Ticks>() && cooldown->get_if<repl::Ticks>()->ticks > 0, "and of his cooldown");
    }
    send(a, "ServerDisbandGuild");
    CHECK(!book->in_guild(a_guid) && book->guilds() == 0, "the leader can disband it");

    // A chat channel of the guild with no guild is refused privately, and never broadcast.
    bi = b.got.size();
    say(a, "nobody", guild::kGuildSay);
    CHECK(received_chat(b, bi).empty(), "guild chat with no guild reaches nobody");
}

/// The bytes of FGuildData, checked against the client's serializers one field at a time (0x145a6ac30, 0x145a66a70, 0x145a66600, 0x145a68b40, 0x145a6adb0).
static void test_guild_data_wire() {
    std::printf("FGuildData as the client's NetSerialize reads it\n");
    const schema::ClassSchema& gc = class_named("UIntrepidGuildComponent");
    const auto* cmd = gc.command("GuildData");
    CHECK(cmd && cmd->handle == 4 && cmd->type == schema::PropType::NetSerializeStruct, "GuildData is one command, handle 4: the client's Cmd=3, ReadHandle=4");
    CHECK(gc.command("GuildMemberData.bIsOnline") != nullptr && gc.command("GuildData.Members") == nullptr, "its members are not commands of their own, FGuildMemberData's are");
    repl::GuildDataWire g;
    g.name = "AB";
    g.guid = {1, 2, 3, 4};
    repl::GuildDataWire::Rank rank;
    rank.name = "C";
    rank.permissions[0] = true;
    rank.permissions[34] = true;
    g.ranks.push_back(rank);
    g.level = 7;
    g.experience = 0x01020304;
    g.capacity = 0x0506;
    g.level_cap = 9;
    g.members.push_back({"D", {5, 6, 7, 8}, 2, 12, 3, 100, 200, -1, {1, 2, 3, 4}, true});
    BunchWriter out;
    CHECK(repl::write_guild_data(out, g), "it writes");
    wire::PacketReader r(out.data(), out.byte_size(), out.bit_pos());
    const auto str = [&](const char* want) {
        const auto s = repl::read_fstring(r);
        return s && *s == want;
    };
    const auto word = [&](uint32_t want) { return r.read_bits(32) == want; };
    bool ok = str("AB");
    for (uint32_t w : {1u, 2u, 3u, 4u}) ok = word(w) && ok;
    CHECK(ok, "the name and the guid");
    ok = word(1) && str("C");
    for (size_t i = 0; i < repl::GuildDataWire::kPermissions; ++i) ok = word(i == 0 || i == 34 ? 1 : 0) && ok;
    CHECK(ok, "a count, then each rank: its name and 35 permissions of 32 bits each");
    ok = r.read_bits(8) == 7 && word(0x01020304) && word(0) && r.read_bits(16) == 0x0506 && r.read_bits(8) == 9;
    CHECK(ok, "level (8), experience (32), no skill points, capacity (16), level cap (8)");
    ok = word(0) && word(0) && word(0);
    CHECK(ok, "no purchased ranks, no war charges, no unlocked groups");
    ok = word(1) && str("D");
    for (uint32_t w : {5u, 6u, 7u, 8u}) ok = word(w) && ok;
    ok = r.read_bits(8) == 2 && r.read_bits(8) == 12 && r.read_bits(8) == 3 && r.read_bits(64) == 100 && r.read_bits(64) == 200 && r.read_bits(8) == 0xff && ok;
    for (uint32_t w : {1u, 2u, 3u, 4u}) ok = word(w) && ok;
    ok = word(1) && ok;
    CHECK(ok, "a count, then each member: name, guid, rank, level, class, two times, group, the guild's guid and online as 32 bits");
    CHECK(word(0), "and an empty map of wars");
    CHECK(r.pos() == out.bit_pos(), "to the last bit");
    wire::PacketReader again(out.data(), out.byte_size(), out.bit_pos());
    const auto back = repl::read_guild_data(again);
    CHECK(back && *back == g, "and it reads back the same");
}

static void test_guild_store_form() {
    std::printf("a guild as the database keeps it\n");
    guild::Guild g;
    g.guid = {1, 2, 3, 0xdeadbeef};
    g.name = "Iron Wolves";
    g.level = 4;
    g.experience = 1234;
    g.ranks = guild::default_ranks();
    g.members.push_back({"Ann", {7, 8, 9, 10}, 0, 12, 3, 1000, 2000});
    g.members.push_back({"Cat", {0xffffffff, 0, 1, 2}, 2, 5, 8, 1500, 2500});
    const auto back = guild::from_json(guild::guid_hex(g.guid), g.name, guild::to_json(g));
    CHECK(back && *back == g, "it reads back the same");
    CHECK(guild::guid_hex(g.guid).size() == 32 && guild::guid_from_hex(guild::guid_hex(g.guid)) == g.guid, "a guid as hex and back");
    CHECK(!guild::guid_from_hex("nope") && !guild::guid_from_hex(std::string(32, 'G')), "a bad guid is refused");
    CHECK(!guild::from_json(guild::guid_hex(g.guid), g.name, "not json"), "data that does not read is refused");
    CHECK(!guild::from_json(guild::guid_hex(g.guid), g.name, R"({"level":1,"experience":0,"ranks":[],"members":[]})"), "a guild with no ranks is refused");
    CHECK(!guild::from_json("short", g.name, guild::to_json(g)), "as is a row with a bad guid");
}

static void test_guild_saver() {
    std::printf("the worker that writes guilds\n");
    struct FakeStore : svc::CharacterApi {
        std::mutex m;
        std::vector<std::string> calls;
        int unavailable = 0;
        svc::CharacterResult get_character(const std::string&) override { return {}; }
        svc::CallStatus save_position(const std::string&, const svc::Position&) override { return svc::CallStatus::Ok; }
        svc::CallStatus save_guild(const std::string& guid, const std::string& name, const std::string&) override {
            std::lock_guard<std::mutex> lock(m);
            if (unavailable > 0) { --unavailable; return svc::CallStatus::Unavailable; }
            calls.push_back("save " + guid + " " + name);
            return svc::CallStatus::Ok;
        }
        svc::CallStatus delete_guild(const std::string& guid) override {
            std::lock_guard<std::mutex> lock(m);
            calls.push_back("delete " + guid);
            return svc::CallStatus::Ok;
        }
    };
    FakeStore store;
    store.unavailable = 1;
    guild::Guild g;
    g.guid = {0, 0, 0, 1};
    g.name = "Iron Wolves";
    g.ranks = guild::default_ranks();
    {
        guild::GuildSaver saver(store, std::chrono::milliseconds(1));
        saver.save(g);
        guild::Guild other = g;
        other.guid = {0, 0, 0, 2};
        saver.remove(other.guid);
    }   // the destructor writes what is left
    const std::string hex = guild::guid_hex(g.guid);
    const bool saved = std::find(store.calls.begin(), store.calls.end(), "save " + hex + " Iron Wolves") != store.calls.end();
    const bool removed = std::find(store.calls.begin(), store.calls.end(), "delete " + guild::guid_hex({0, 0, 0, 2})) != store.calls.end();
    CHECK(removed, "a delete reaches the service before the worker stops");
    CHECK(saved || store.unavailable == 0, "a save the service could not take is tried again unless the worker was stopping");
}

AOC_REGISTER_TEST(test_guild_session);
AOC_REGISTER_TEST(test_guild_data_wire);
AOC_REGISTER_TEST(test_guild_store_form);
AOC_REGISTER_TEST(test_guild_saver);
