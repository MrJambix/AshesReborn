// SPDX-License-Identifier: AGPL-3.0-only
// Included inside namespace aoc::worldd::bootstrap in bootstrap.cpp.
// Parties and raids on the wire: session/group.h holds who is in what.

namespace {

/// The four words of a guid under a struct's name, as the RPC's commands want them.
bool set_group_guid(const schema::RpcSchema& rpc, repl::RpcParams& params, const std::string& base, const group::Guid& g) {
    static constexpr const char* kWords[] = {".A", ".B", ".C", ".D"};
    bool ok = true;
    for (size_t i = 0; i < 4; ++i) ok = repl::set_rpc_param(rpc, params, base + kWords[i], Value::i64(static_cast<int32_t>(g[i]))) && ok;
    return ok;
}

/// FAoCLootAllocationMethods and its deprecated twin, whose values pkt#606, the empty group of a login, holds at 0.
bool set_group_loot(const schema::RpcSchema& rpc, repl::RpcParams& params, const std::string& base, const group::LootMethods& methods) {
    bool ok = repl::set_rpc_param(rpc, params, base + ".LootAllocationMethods.DefaultLootAllocationMethod", Value::u64(methods[0]));
    size_t at = 1;
    for (const char* tier : {"Common", "Uncommon", "Rare", "Heroic", "Epic", "Legendary", "Artifact"}) {
        ok = repl::set_rpc_param(rpc, params, base + ".LootAllocationMethods." + tier + "LootAllocationMethod", Value::u64(methods[at++])) && ok;
    }
    for (const char* tier : {"Poor", "CommonAndUncommon", "RareAndEpic", "Legendary"}) {
        ok = repl::set_rpc_param(rpc, params, base + ".LootAllocationMethodsDEPRECATED." + tier + "LootAllocationMethod", Value::u64(0)) && ok;
    }
    return repl::set_rpc_param(rpc, params, base + ".LootAllocationMethodsDEPRECATED.ContainerAllocationMethod", Value::u64(0)) && ok;
}

/// One member of a squad, by the element's handles (AAoCPlayerController's kRpcElementCommands: SquadMembers).
repl::ElementState group_person_element(const group::Person& p) {
    repl::ElementState e;
    e.values[1] = Value::str(p.name);
    for (uint16_t i = 0; i < 4; ++i) e.values[static_cast<uint16_t>(i + 2)] = Value::i64(static_cast<int32_t>(p.guid[i]));
    e.values[6] = Value::u64(p.character_class);
    e.values[7] = Value::boolean(p.online);
    e.values[8] = Value::f32(p.vitals.health_percent);
    e.values[9] = Value::f32(p.vitals.max_health);
    e.values[10] = Value::f32(p.vitals.mana_percent);
    e.values[11] = Value::f32(p.vitals.max_mana);
    e.values[12] = Value::i64(p.vitals.level);
    e.values[13] = Value::u64(p.vitals.dead ? 1 : 0);   // EAliveStatusType
    e.values[14] = Value(repl::quantize_vector(1, p.vitals.x, p.vitals.y, p.vitals.z));
    const auto marker = p.vitals.marker.value_or(std::array<double, 3>{});
    e.values[15] = Value(repl::quantize_vector(1, marker[0], marker[1], marker[2]));
    e.values[16] = Value::f32(p.vitals.heading);
    return e;
}

}  // namespace

std::optional<Send> PlayerBootstrap::group_update(const group::View& v) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientUpdateGroup");
    if (!rpc) return std::nullopt;
    repl::RpcParams p;
    bool ok = set_group_guid(*rpc, p, "NewGroup.GroupGuid", v.guid);
    ok = set_group_guid(*rpc, p, "NewGroup.LeaderGuid", v.leader) && ok;
    ok = set_group_guid(*rpc, p, "NewGroup.MasterLooterGuid", v.master_looter) && ok;
    ok = repl::set_rpc_param(*rpc, p, "NewGroup.GroupType", Value::u64(v.type)) && ok;
    repl::ArrayValue squads;
    for (const group::SquadView& q : v.squads) {
        repl::ElementState e;
        e.values[1] = Value::i64(q.number);
        repl::ArrayValue members;
        for (const size_t i : q.members) members.elements.push_back(group_person_element(v.people[i]));
        e.values[2] = Value(std::move(members));
        repl::ArrayValue leaders;
        for (const group::Guid& leader : q.leaders) {
            repl::ElementState one;
            for (uint16_t i = 0; i < 4; ++i) one.values[static_cast<uint16_t>(i + 1)] = Value::i64(static_cast<int32_t>(leader[i]));
            leaders.elements.push_back(std::move(one));
        }
        e.values[3] = Value(std::move(leaders));
        e.values[4] = Value::boolean(v.settings.member_shouts);
        squads.elements.push_back(std::move(e));
    }
    ok = repl::set_rpc_param(*rpc, p, "NewGroup.Squads", Value(std::move(squads))) && ok;
    ok = repl::set_rpc_param(*rpc, p, "NewGroup.RaidSettings.ShoutPrivilege", Value::u64(v.settings.shout_privilege)) && ok;
    ok = repl::set_rpc_param(*rpc, p, "NewGroup.RaidSettings.bOverridePartyLootSettings", Value::boolean(v.settings.override_party_loot)) && ok;
    ok = set_group_loot(*rpc, p, "NewGroup.RaidSettings", v.settings.raid_loot) && ok;
    ok = repl::set_rpc_param(*rpc, p, "NewGroup.PartySettings.bEnableMemberShouts", Value::boolean(v.settings.member_shouts)) && ok;
    ok = set_group_loot(*rpc, p, "NewGroup.PartySettings", v.settings.party_loot) && ok;
    if (!ok) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientUpdateGroup";
    call.params = std::move(p);
    return rpcs(Role::PlayerController, "ClientUpdateGroup", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::group_invitation(const group::Invitation& invitation) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientRequestJoinGroup");
    if (!rpc) return std::nullopt;
    repl::RpcParams p;
    if (!set_group_guid(*rpc, p, "GroupGuid", invitation.group) || !repl::set_rpc_param(*rpc, p, "InviterName", Value::str(invitation.inviter)) ||
        !repl::set_rpc_param(*rpc, p, "GroupType", Value::u64(invitation.type))) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientRequestJoinGroup";
    call.params = std::move(p);
    return rpcs(Role::PlayerController, "ClientRequestJoinGroup", std::span<const repl::RpcRequest>(&call, 1));
}

std::optional<Send> PlayerBootstrap::group_convert_prompt(const std::string& invited) {
    if (!ok_ || !second_state_added_) return std::nullopt;
    const schema::RpcSchema* rpc = pc_.obj.schema->rpc("ClientRequestConvertToRaid");
    if (!rpc) return std::nullopt;
    repl::RpcParams p;
    if (!repl::set_rpc_param(*rpc, p, "InvitedPlayer", Value::str(invited))) return std::nullopt;
    repl::RpcRequest call;
    call.rpc = "ClientRequestConvertToRaid";
    call.params = std::move(p);
    return rpcs(Role::PlayerController, "ClientRequestConvertToRaid", std::span<const repl::RpcRequest>(&call, 1));
}
