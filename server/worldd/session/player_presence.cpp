// SPDX-License-Identifier: AGPL-3.0-only
#include "session/player_presence.h"

#include "content/emote_commands.h"
#include "protocol/repl/move_packed.h"
#include "stats/character_stats.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <initializer_list>
#include <string_view>

namespace aoc::worldd {
namespace {
namespace repl = protocol::repl;
using repl::Value;

/// The movement mode the other players' clients are shown (from core 736185f): a move's mode is the byte the client sent and the move checker keeps it as sent, so a modified client
/// could put any number into every nearby client's copy of its pawn.  The modes a character moves in are walking, falling, swimming, flying and the custom modes 16, 17 and 18; the
/// client's 0 (none) and any other number are shown as walking.
uint8_t shown_movement_mode(uint8_t mode) {
    switch (mode) {
        case repl::kMoveWalking:
        case repl::kMoveFalling:
        case repl::kMoveSwimming:
        case repl::kMoveFlying:
        case 16:
        case 17:
        case 18:
            return mode;
        default:
            return repl::kMoveWalking;
    }
}

bool allowed(std::string_view path, std::initializer_list<std::string_view> paths) {
    return std::any_of(paths.begin(), paths.end(), [path](std::string_view p) {
        return path == p || (p.ends_with('.') && path.starts_with(p));
    });
}

repl::ObjectState project(const repl::ObjectState& source, std::initializer_list<std::string_view> paths,
                          std::initializer_list<std::string_view> arrays = {}) {
    repl::ObjectState out;
    out.schema = source.schema;
    out.opts = source.opts;
    if (!source.schema) return out;
    for (const auto& [handle, value] : source.state.values()) {
        if (allowed(source.schema->commands[handle - 1].path, paths)) out.state.set(*out.schema, handle, value);
    }
    for (const auto& [field, array] : source.arrays.all()) {
        const auto* definition = source.schema->field(field);
        if (definition && allowed(definition->name, arrays)) *out.arrays.get(*out.schema, field) = array;
    }
    return out;
}

bool arrays_equal(const repl::FastArrays& a, const repl::FastArrays& b) {
    if (a.all().size() != b.all().size()) return false;
    for (const auto& [field, aa] : a.all()) {
        const auto* bb = b.find(field);
        if (!bb || aa.array_key() != bb->array_key() || aa.items().size() != bb->items().size()) return false;
        for (size_t i = 0; i < aa.items().size(); ++i) {
            const auto& x = aa.items()[i]; const auto& y = bb->items()[i];
            if (x.id != y.id || x.key != y.key || x.values.values != y.values.values) return false;
        }
    }
    return true;
}

bool equal_public_state(const repl::Actor& a, const repl::Actor& b) {
    if (!(a.obj.state == b.obj.state) || !arrays_equal(a.obj.arrays, b.obj.arrays) || a.subobjects.size() != b.subobjects.size()) return false;
    for (size_t i = 0; i < a.subobjects.size(); ++i) {
        const auto& x = a.subobjects[i]; const auto& y = b.subobjects[i];
        if (x.guid != y.guid || !(x.obj.state == y.obj.state) || !arrays_equal(x.obj.arrays, y.obj.arrays)) return false;
    }
    return true;
}

uint16_t angle(double degrees, double scale) {
    return static_cast<uint16_t>(static_cast<int64_t>(std::floor(degrees * scale / 360.0 + 0.5)) & (scale == 256 ? 255 : 65535));
}

repl::Actor public_pawn(const bootstrap::PlayerBootstrap& player, const stats::CharacterStats* stats, const repl::Actor* previous) {
    const auto& source = player.pawn();
    const auto& guids = player.setup().guids;
    repl::Actor out;
    out.guid = source.guid;
    out.open = source.open;
    out.obj = project(source.obj, {"AuthServerIDReplicated", "bHidden", "bTearOff", "bCanBeDamaged", "bReplicates",
        "ReplicatedGravityDirection", "bIsCrouched", "bProxyIsJumpForceApplied", "JumpMaxHoldTime", "JumpMaxCount",
        "AnimRootMotionTranslationScale", "AttachmentReplication.", "ReplicatedBasedMovement.", "RepRootMotion.",
        "StealthInfo.", "bIsSwimmingAtTop", "bWantsToWalk", "bRootMotionObstructed"});
    out.obj.state.set(*out.obj.schema, "RemoteRole", Value::u64(1));
    out.obj.state.set(*out.obj.schema, "Role", Value::u64(4));
    out.obj.state.set(*out.obj.schema, "bReplicateMovement", Value::boolean(true));
    out.obj.state.set(*out.obj.schema, "PlayerState", Value(guids.player_state));
    for (const auto& component : source.subobjects) {
        repl::Subobject sub = component;
        if (component.guid == guids.base_character_info) {
            sub.obj = project(component.obj, {"bReplicates", "bIsActive", "CharacterGuid.", "CharacterType", "CharacterName",
                "CreatureOwnerName", "Race", "Gender", "ServerId", "Level", "DynamicLevelAmount", "bHideNameplate",
                "bHideNameplateWhenTargeted", "bIsPhasedOut", "bHasBeenPossessed", "bShowGMTag", "CharacterTitle"}, {"HeldItems"});
        } else if (component.guid == guids.appearance) {
            sub.obj = project(component.obj, {"bReplicates", "bIsActive", "AppearanceIDs", "SharedAppearanceInfoId.",
                "CharacterCustomization", "bForceHideHeldItems"});
        } else if (component.guid == guids.combat_info) {
            sub.obj = project(component.obj, {"bReplicates", "bIsActive", "bIsDead", "CurrentStatusEffects", "bPvPForceFlag",
                "InPvpSuppressionArea", "bInCombat", "bInPVPCombat", "bNotTargetable", "AnimBPValues", "PrimaryClassId.",
                "SecondaryClassId.", "bIsUsingRangedWeapon", "SiegeTeamType", "SiegeNodeId"});
        } else if (component.guid == guids.ability) {
            sub.obj = project(component.obj, {"bReplicates", "bIsActive"}, {"ReplicatedActiveAbilities"});
            // the records of the emote abilities the owner's component loads: another player's client needs them to play the owner's emotes (the owner's class abilities, its
            // KnownAbilities and its Cooldowns stay the owner's)
            if (const repl::Value* records = component.obj.state.get(*component.obj.schema, "ReplicatedRecordsToLoad")) {
                if (const auto* list = records->get_if<repl::ArrayValue>()) {
                    repl::ArrayValue shown;
                    for (const repl::ElementState& e : list->elements) {
                        const auto guid = e.values.find(1);
                        if (guid != e.values.end() && guid->second.get_if<int64_t>() && content::is_emote_ability(*guid->second.get_if<int64_t>())) shown.elements.push_back(e);
                    }
                    if (!shown.elements.empty()) sub.obj.state.set(*sub.obj.schema, "ReplicatedRecordsToLoad", Value(std::move(shown)));
                }
            }
        } else if (component.guid == guids.stats) {
            sub.obj = project(component.obj, {"bReplicates", "bIsActive", "CrowdControlState."}, {"StatRepInt32OwnerProxy", "StatRepInt32EveryoneProxy",
                "StatRepFloatArrayOwnerProxy", "StatRepFloatArrayEveryoneProxy", "CurrentEffects"});
            // Proxy-only stats are absent from the owner's bootstrap. Give their public
            // array its own stable IDs and keys so later changes are genuine deltas.
            auto* array = sub.obj.arrays.get(*sub.obj.schema, "StatRepInt32ProxyOnly");
            if (array && stats) {
                if (const auto* old = previous ? previous->find(sub.guid) : nullptr) {
                    const auto* field = sub.obj.schema->field("StatRepInt32ProxyOnly");
                    if (field) if (const auto* before = old->obj.arrays.find(field->index)) *array = *before;
                }
                std::vector<int32_t> keep;
                for (const auto& value : stats->values()) {
                    if (value.replication != 6 || value.float_array()) continue;
                    repl::ElementState element;
                    element.values[1] = Value(repl::StatInt32Inst{value.guid, std::bit_cast<uint32_t>(value.value), value.buckets(),
                        std::bit_cast<uint32_t>(value.base), std::bit_cast<uint32_t>(value.equip)});
                    const auto found = std::find_if(array->items().begin(), array->items().end(), [&](const auto& item) {
                        const auto it = item.values.values.find(1);
                        const auto* stat = it == item.values.values.end() ? nullptr : it->second.template get_if<repl::StatInt32Inst>();
                        return stat && stat->record == value.guid;
                    });
                    if (found == array->items().end()) { if (const auto id = array->add(element)) keep.push_back(*id); }
                    else { const int32_t id = found->id; if (found->values.values != element.values) array->change(id, element); keep.push_back(id); }
                }
                std::vector<int32_t> removed;
                for (const auto& item : array->items()) if (std::find(keep.begin(), keep.end(), item.id) == keep.end()) removed.push_back(item.id);
                for (const auto id : removed) array->remove(id);
            }
        } else if (component.guid == guids.alignment) {
            sub.obj = project(component.obj, {"bReplicates", "bIsActive", "Alignments"});
        } else if (player.guild_component_guid().ObjectId != 0 && component.guid == player.guild_component_guid()) {
            // What the client's GetGuildName and IsGuildmate read of another character (0x146ac0740, 0x146b4b7e0): never the members, the ranks or the cooldown.
            // A player in no guild has nothing to show: the client reads a component it was never sent as empty.
            const auto* guild_name = component.obj.state.get(*component.obj.schema, "GuildName");
            const auto* named = guild_name ? guild_name->get_if<std::string>() : nullptr;
            if (!named || named->empty()) continue;
            sub.obj = project(component.obj, {"bReplicates", "bIsActive", "GuildName"});
            if (const auto* whole = component.obj.state.get(*component.obj.schema, "GuildData")) {
                if (const auto* wire = whole->get_if<repl::GuildDataWire>()) {
                    repl::GuildDataWire shown;   // the name and the guid: GuildData is one command, so what stays private has to be left out of its value
                    shown.name = wire->name;
                    shown.guid = wire->guid;
                    sub.obj.state.set(*sub.obj.schema, "GuildData", Value(std::move(shown)));
                }
            }
        } else continue; // Deny inventories, wallet, XP, account data and future components by default.
        out.subobjects.push_back(std::move(sub));
    }
    return out;
}
}

PlayerPresence::~PlayerPresence() { clear(); }

void PlayerPresence::clear() {
    if (pawn_) registry_.remove(pawn_);
    if (state_) registry_.remove(state_);
    pawn_ = state_ = 0;
    next_update_ = 0;
}

bool PlayerPresence::update(const bootstrap::PlayerBootstrap& player, const GoodPosition& at,
                            const stats::CharacterStats* stats, double now) {
    if (!std::isfinite(now) || !std::isfinite(at.x) || !std::isfinite(at.y) || !std::isfinite(at.z) || !std::isfinite(at.yaw_degrees)) return false;
    const auto& g = player.setup().guids;
    if (const auto* base = player.pawn().find(g.base_character_info)) {
        const auto* v = base->obj.state.get(*base->obj.schema, "bGmHidden");
        if (v && v->get_if<bool>() && *v->get_if<bool>()) { clear(); return true; }
    }
    if (pawn_ && now < next_update_) return true;
    const auto* old = pawn_ ? registry_.find(pawn_) : nullptr;
    repl::Actor pawn = public_pawn(player, stats, old ? &old->actor : nullptr);
    repl::RepMovement movement;
    movement.location = repl::quantize_location(pawn.obj.opts, at.x, at.y, at.z);
    movement.rotation = repl::Rot{0, angle(at.yaw_degrees, pawn.obj.opts.rep_movement_short_rotation ? 65536 : 256), 0};
    const double dt = now - last_update_;
    double vx = 0, vy = 0, vz = 0;
    if (old && dt > 0 && dt < 1.0) { vx = (at.x - previous_.x) / dt; vy = (at.y - previous_.y) / dt; vz = (at.z - previous_.z) / dt; }
    movement.linear_velocity = repl::quantize_velocity(pawn.obj.opts, vx, vy, vz);
    auto& object = pawn.obj;
    object.state.set(*object.schema, "ReplicatedMovement", Value(movement));
    object.state.set(*object.schema, "ReplicatedMovementMode", Value::u64(shown_movement_mode(at.movement_mode)));
    object.state.set(*object.schema, "RemoteViewPitch16", Value::u64(at.pitch16));
    const double radians = at.yaw_degrees * 3.141592653589793 / 180.0;
    const double forward = vx * std::cos(radians) + vy * std::sin(radians);
    const double right = -vx * std::sin(radians) + vy * std::cos(radians);
    const uint8_t input = static_cast<uint8_t>((forward > 1 ? 1 : forward < -1 ? 2 : 0) | (right > 1 ? 4 : right < -1 ? 8 : 0));
    object.state.set(*object.schema, "CurrentMovementInput", Value(repl::PlayerMovementInput{input}));
    const auto* previous_movement = old ? old->actor.obj.state.get(*object.schema, "ReplicatedMovement") : nullptr;
    const bool moved = !previous_movement || !(*previous_movement == Value(movement));
    const auto* stamp = old ? old->actor.obj.state.get(*object.schema, "ReplicatedServerLastTransformUpdateTimeStamp") : nullptr;
    object.state.set(*object.schema, "ReplicatedServerLastTransformUpdateTimeStamp", moved || !stamp ? Value::f32(static_cast<float>(now)) : *stamp);
    pawn.open.spawn.location = repl::quantize_vector(10, at.x, at.y, at.z);
    pawn.open.spawn.rotation = repl::Rot{0, angle(at.yaw_degrees, 65536), 0};
    pawn.open.spawn.velocity = repl::quantize_vector(10, vx, vy, vz);

    repl::Actor state;
    state.guid = g.player_state;
    state.open = player.player_state().open;
    state.open.spawn = {};
    state.obj = project(player.player_state().obj, {"RemoteRole", "Role", "PlayerId", "StartTime"});
    state.obj.state.set(*state.obj.schema, "PlayerNamePrivate", Value::str(player.setup().name));
    const uint64_t own = static_cast<uint64_t>(player.setup().player_id);
    if (!pawn_) {
        world::WorldActor ps;
        ps.id = state.guid.ObjectId; ps.kind = world::ActorKind::PlayerState; ps.actor = std::move(state);
        ps.x = at.x; ps.y = at.y; ps.z = at.z; ps.never_dormant = true; ps.hidden_from_player = own; ps.label = player.setup().name;
        if (!registry_.add(std::move(ps))) return false;
        state_ = g.player_state.ObjectId;
        world::WorldActor actor;
        actor.id = pawn.guid.ObjectId; actor.kind = world::ActorKind::PlayerPawn; actor.actor = std::move(pawn);
        actor.x = at.x; actor.y = at.y; actor.z = at.z; actor.yaw_degrees = at.yaw_degrees;
        actor.never_dormant = true; actor.hidden_from_player = own; actor.label = player.setup().name;
        actor.npc.collision_cylinder = g.collision_cylinder.ObjectId;
        actor.ability_exports = player.public_ability_exports();
        if (!registry_.add(std::move(actor))) { clear(); return false; }
        pawn_ = g.pawn.ObjectId;
    } else {
        // Movement is unreliable; appearance, health, equipment and effects retain the
        // registry's reliable change counter and the view's retransmission behavior.
        repl::Actor comparable = pawn;
        for (const char* path : {"ReplicatedMovement", "ReplicatedMovementMode", "RemoteViewPitch16", "CurrentMovementInput", "ReplicatedServerLastTransformUpdateTimeStamp"}) {
            if (const auto* before = old->actor.obj.state.get(*object.schema, path)) comparable.obj.state.set(*object.schema, path, *before);
        }
        const bool state_changed = !equal_public_state(comparable, old->actor);
        if (!equal_public_state(pawn, old->actor)) {
            registry_.move(pawn_, at.x, at.y, at.z, at.yaw_degrees);
            registry_.modify(pawn_, state_changed ? world::Change::State : world::Change::Movement, [&](auto& actor) {
                actor.actor = std::move(pawn); actor.ability_exports = player.public_ability_exports();
            });
        }
        if (const auto* ps = registry_.find(state_); ps && (ps->x != at.x || ps->y != at.y || ps->z != at.z))
            registry_.move(state_, at.x, at.y, at.z, 0);
        if (const auto* before = registry_.find(state_); before && !equal_public_state(state, before->actor))
            registry_.modify(state_, world::Change::State, [&](auto& actor) { actor.actor = std::move(state); });
    }
    previous_ = at; last_update_ = now; next_update_ = now + 0.067;
    return true;
}
}
