// SPDX-License-Identifier: AGPL-3.0-only
// Included inside namespace aoc::worldd::bootstrap in bootstrap.cpp.
// Guilds on the wire: the pawn's GuildComponent (UIntrepidGuildComponent), whose name, data and member row are ordinary
// replicated properties and whose replies are its own RPCs.  session/guild.h holds who is in what.
//
// No recording opens this component, so it has no retail checksum.  A checksum of 0 is what the client's GetObjectFromNetGUID
// (0x14426f930) skips its comparison for, and UIntrepidGuildComponent is a default subobject of APlayerCharacter (+0x1968), so
// the client finds it by name.  The client's "is in a guild" is GuildName being non-empty (0x1465e9b30).

namespace {

constexpr uint32_t kChecksumGuildUnchecked = 0;

Value guild_ticks(double unix_seconds) {
    if (unix_seconds <= 0) return Value(repl::Ticks{0});
    return Value(repl::Ticks{static_cast<int64_t>(unix_seconds) * 10000000 + 621355968000000000});   // FDateTime: 100 ns from 0001-01-01
}

}  // namespace

bool PlayerBootstrap::open_guild_component(const emit::FIntrepidNetworkGUID& guid) {
    if (!ok_ || !second_state_added_) return false;
    if (guild_component_guid_.ObjectId != 0) return true;
    repl::Subobject made = make_component(guid, "GuildComponent", kChecksumGuildUnchecked);
    if (!made.obj.schema) return fail("no schema for the guild component (UIntrepidGuildComponent)");
    pawn_.subobjects.push_back(std::move(made));
    guild_component_guid_ = guid;
    return true;
}

bool PlayerBootstrap::set_guild_view(const guild::View& v) {
    repl::Subobject* c = guild_component_guid_.ObjectId ? component(guild_component_guid_) : nullptr;
    if (!c) return false;
    auto& st = c->obj.state;
    const auto& sc = *c->obj.schema;
    bool ok = st.set(sc, "GuildName", Value::str(v.name));
    static constexpr const char* kWords[] = {"A", "B", "C", "D"};
    for (size_t i = 0; i < 4; ++i) {
        const guild::Guid self = v.self ? v.self->member.guid : guild::kNoGuid;
        ok = st.set(sc, std::string("GuildMemberData.PersistedGuildMemberData.CharacterGuid.") + kWords[i], Value::i64(static_cast<int32_t>(self[i]))) && ok;
        ok = st.set(sc, std::string("GuildMemberData.GuildGuid.") + kWords[i], Value::i64(static_cast<int32_t>(v.guid[i]))) && ok;
    }
    repl::GuildDataWire data;
    data.name = v.name;
    data.guid = v.guid;
    data.level = v.level;
    data.experience = v.experience;
    data.capacity = v.capacity;
    data.level_cap = v.level_cap;
    for (const guild::Rank& r : v.ranks) data.ranks.push_back({r.name, r.permissions});
    for (const guild::View::Row& row : v.members) {
        const guild::GuildMember& m = row.member;
        data.members.push_back({m.name, m.guid, m.rank, m.level, m.character_class, m.last_online_unix, m.joined_unix, 0, v.guid, row.online});
    }
    ok = st.set(sc, "GuildData", Value(std::move(data))) && ok;
    const guild::GuildMember self = v.self ? v.self->member : guild::GuildMember{};
    const std::string own = "GuildMemberData.PersistedGuildMemberData.";
    ok = st.set(sc, own + "CharacterName", Value::str(self.name)) && ok;
    ok = st.set(sc, own + "RankIndex", Value::u64(self.rank)) && ok;
    ok = st.set(sc, own + "CharacterLevel", Value::u64(v.self ? self.level : 0)) && ok;
    ok = st.set(sc, own + "CharacterClass", Value::u64(self.character_class)) && ok;
    ok = st.set(sc, own + "LastTimeOnlineRealTime", Value::u64(self.last_online_unix)) && ok;
    ok = st.set(sc, own + "TimeJoinedRealTime", Value::u64(self.joined_unix)) && ok;
    ok = st.set(sc, own + "AssignedGroupIndex", Value::i64(0)) && ok;
    ok = st.set(sc, "GuildMemberData.bIsOnline", Value::boolean(v.self.has_value())) && ok;
    ok = st.set(sc, "GuildCooldown", guild_ticks(v.cooldown_until_unix)) && ok;
    if (!ok) return fail("a value of the guild component was refused");
    return true;
}

std::optional<Send> PlayerBootstrap::guild_call(const char* what, repl::RpcParams params) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const repl::Subobject* c = guild_component_guid_.ObjectId ? component(guild_component_guid_) : nullptr;
    if (!c) return std::nullopt;
    repl::RpcRequest call;
    call.target = c;
    call.rpc = what;
    call.params = std::move(params);
    return rpcs(Role::Pawn, what, std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::guild_invitation(const guild::Invitation& invitation) {
    const repl::Subobject* c = guild_component_guid_.ObjectId ? component(guild_component_guid_) : nullptr;
    const schema::RpcSchema* rpc = c ? c->obj.schema->rpc("ClientInvitePlayerToGuild") : nullptr;
    if (!rpc) return std::nullopt;
    repl::RpcParams p;
    if (!repl::set_rpc_param(*rpc, p, "GuildRequestUUID", Value::str(invitation.uuid)) ||
        !repl::set_rpc_param(*rpc, p, "SourceCharacterName", Value::str(invitation.inviter)) ||
        !repl::set_rpc_param(*rpc, p, "InGuildName", Value::str(invitation.guild_name))) return std::nullopt;
    return guild_call("ClientInvitePlayerToGuild", std::move(p));
}

std::optional<Send> PlayerBootstrap::guild_reply(const guild::Reply& reply) {
    const repl::Subobject* c = guild_component_guid_.ObjectId ? component(guild_component_guid_) : nullptr;
    const char* what = reply.success ? "ClientGuildRequestSucceeded" : "ClientGuildRequestFailed";
    const schema::RpcSchema* rpc = c ? c->obj.schema->rpc(what) : nullptr;
    if (!rpc) return std::nullopt;
    repl::RpcParams p;
    if (!repl::set_rpc_param(*rpc, p, "RequestType", Value::u64(reply.request)) || !repl::set_rpc_param(*rpc, p, "Message", Value::str(reply.message))) return std::nullopt;
    return guild_call(what, std::move(p));
}

std::optional<Send> PlayerBootstrap::guild_list(const std::vector<std::pair<std::string, guild::Guid>>& guilds) {
    const repl::Subobject* c = guild_component_guid_.ObjectId ? component(guild_component_guid_) : nullptr;
    const schema::RpcSchema* rpc = c ? c->obj.schema->rpc("ClientGetUpdatedGuildsList") : nullptr;
    if (!rpc) return std::nullopt;
    repl::ArrayValue list;
    for (const auto& [name, guid] : guilds) {
        repl::ElementState e;
        e.values[1] = Value::str(name);
        for (uint16_t i = 0; i < 4; ++i) e.values[static_cast<uint16_t>(i + 2)] = Value::i64(static_cast<int32_t>(guid[i]));
        list.elements.push_back(std::move(e));
    }
    repl::RpcParams p;
    if (!repl::set_rpc_param(*rpc, p, "GuildNameGuidPairs", Value(std::move(list)))) return std::nullopt;
    return guild_call("ClientGetUpdatedGuildsList", std::move(p));
}

bool PlayerBootstrap::set_crowd_control(const crowd::State& v, double sync_start, double sync_end) {
    repl::Subobject* stats = component(setup_.guids.stats);
    if (!ok_ || !second_state_added_ || !stats) return false;
    auto& st = stats->obj.state;
    const auto& sc = *stats->obj.schema;
    bool ok = st.set(sc, "CrowdControlState.StatusFlags", Value::u64(v.status));
    ok = st.set(sc, "CrowdControlState.CurrentAnimationRequest", Value::u64(v.animation)) && ok;
    ok = st.set(sc, "CrowdControlState.CrowdControlRecordToAnimate", Value::i64(v.record)) && ok;
    ok = st.set(sc, "CrowdControlState.SnareValue", Value::f32(v.snare)) && ok;
    ok = st.set(sc, "CrowdControlState.SyncStartAnimTime", Value::f64(sync_start)) && ok;
    ok = st.set(sc, "CrowdControlState.Duration", Value::f32(v.end > 0 ? static_cast<float>(v.end - v.start) : 0.0f)) && ok;
    ok = st.set(sc, "CrowdControlState.EndAnimTime", Value::f64(sync_end)) && ok;
    return ok;
}

std::optional<Send> PlayerBootstrap::tax_details(const repl::StationTax& tax) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientReceiveTaxDetails");
    if (!rpc) return std::nullopt;
    repl::RpcParams p;
    if (!repl::set_rpc_param(*rpc, p, "TaxDetails.TaxDetailRecordId", Value::i64(tax.record)) || !repl::set_rpc_param(*rpc, p, "TaxDetails.FlatRateAmount", Value::i64(tax.flat)) ||
        !repl::set_rpc_param(*rpc, p, "TaxDetails.TaxPercent", Value::f32(tax.percent)) || !repl::set_rpc_param(*rpc, p, "TaxDetails.OwningNodeTaxPercent", Value::f32(tax.owning_share)) ||
        !repl::set_rpc_param(*rpc, p, "TaxDetails.bIsUnlocked", Value::boolean(tax.unlocked)) || !repl::set_rpc_param(*rpc, p, "TaxDetails.bUseOverride", Value::boolean(tax.use_override))) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientReceiveTaxDetails";
    call.params = std::move(p);
    return rpcs(Role::PlayerController, "ClientReceiveTaxDetails", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::open_create_guild_panel() {
    if (!ok_ || !second_state_added_) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientOpenCreateGuildPanel";
    return rpcs(Role::PlayerController, "ClientOpenCreateGuildPanel", std::span<const repl::RpcRequest>(&call, 1));
}
