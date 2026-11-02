// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  combat/combat_world.cpp (see combat_world.h)
// ============================================================================
#include "combat/combat_world.h"

#include "bootstrap/ability_replication.h"
#include "bootstrap/constants.h"
#include "combat/npc_ability_replicator.h"
#include "combat/npc_kits.h"
#include "combat/npc_replicator.h"
#include "content/content_db.h"
#include "npc/area_npcs.h"
#include "session/summoner_pets.h"
#include "spawns/generic_npc.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace aoc::worldd::combat {

namespace {

nav::Vec3 to_nav(const Vec3& v) { return nav::Vec3{v.x, v.y, v.z}; }

// The game's own defaults for the intelligence settings no Blueprint on a creature's chain sets (the base pawn sets all of them but the sight cone, so these stand only for a creature
// that is not built on it, and for the cone).
constexpr double kBlueprintAggroDefaultCm = 1000.0;
constexpr double kBlueprintSightDefaultCm = 3000.0;
constexpr double kBlueprintSightConeDefaultDeg = 45.0;   // the half angle either side of its facing
constexpr double kBlueprintHearingDefaultCm = 2000.0;
constexpr double kBlueprintPlayerLookSeconds = 2.0;      // how often a creature looks for players

constexpr double kHumanoidTurnGain = 1.2;   // how much faster than their Blueprint's rotation rate the goblins and bandits turn

/// Whether a creature chases at its Blueprint's run speed times its own MoveSpeedMult stat (1.05 plus 0.1 for each rank of its elite scalar), as the game's creatures do: the humanoids, goblins
/// and bandits.  The other families keep the Blueprint's run speed as it is.
bool runs_at_stat_speed(const std::string& archetype) {
    return archetype.find("/Population/NPC/Humanoid/") != std::string::npos || archetype.find("/Population/NPC/PCRaces/") != std::string::npos;
}

/// The kit of a creature kind: by the Blueprint's leaf without "_C"; else the kit whose name is the longest prefix of the leaf (or the leaf of it).
const npc_kits::Kit* kit_of(const std::string& leaf_in) {
    std::string leaf = leaf_in;
    if (leaf.size() > 2 && leaf.compare(leaf.size() - 2, 2, "_C") == 0) leaf.resize(leaf.size() - 2);
    const npc_kits::Kit* best = nullptr;
    size_t best_len = 0;
    for (size_t i = 0; i < npc_kits::kKitCount; ++i) {
        const npc_kits::Kit& k = npc_kits::kKits[i];
        const std::string name = k.blueprint;
        if (name == leaf) return &k;
        const bool related = leaf.rfind(name + "_", 0) == 0 || name.rfind(leaf + "_", 0) == 0;
        if (related && name.size() > best_len) {
            best = &k;
            best_len = name.size();
        }
    }
    return best;
}

}  // namespace
// [CoA ~ C:Y:B]

// ---- one NPC's combat state ----------------------------------------------------------------------------------------------------------

/// A hit as what reacts to it reads it.
struct CombatWorld::HolderEvent {
    uint64_t                  attacker = 0;
    const HitOrigin*          origin = nullptr;
    uint8_t                   flags = 0;
    bool                      lethal = false;
    std::map<int64_t, double> mods, effective;
    std::vector<std::pair<const abilities::EffectDef*, uint64_t>> held;   // what the holder held when it landed, and who left it
};

class CombatWorld::Npc final : public Fighter {
public:
    Npc(CombatWorld& w, uint64_t actor, npc::MotionStats bp_in, std::string leaf_in, int level_in)
        : world(w), actor_id(actor), bp(std::move(bp_in)), leaf(std::move(leaf_in)), lvl(level_in) {}

    uint64_t id() const override { return actor_id; }
    bool     is_player() const override { return false; }
    int      level() const override { return lvl; }
    Vec3     position() const override {
        const world::WorldActor* a = world.registry_.find(actor_id);
        return a ? Vec3{a->x, a->y, a->z} : Vec3{};
    }
    double radius_cm() const override { return bp.radius; }
    bool   alive() const override { return !dead; }
    const std::vector<int64_t>& alignments() const override { return aligns; }
    stats::CharacterStats&      stats() override { return *cs; }

    double health() const {
        const stats::StatValue* v = cs ? cs->find(content::kGuidHealth) : nullptr;
        return v ? v->value : 0.0;
    }
    double max_health() const {
        const stats::StatValue* v = cs ? cs->find(content::kGuidMaxHealth) : nullptr;
        return v ? v->value : 0.0;
    }

    CombatWorld&                           world;
    uint64_t                               actor_id;
    npc::MotionStats                       bp;
    std::string                            leaf;
    int                                    lvl;
    std::vector<int64_t>                   aligns;
    std::unique_ptr<stats::CharacterStats> cs;
    npc::ai::NpcAi                         ai;
    bool                                   hostile = false;
    bool                                   dead = false;
    bool                                   flags_in_combat = false;
    uint64_t                               flags_target = 0;
    bool                                   returning = false;
    const npc_kits::Kit*                   kit = nullptr;
    CombatWorld::Attack                    next;
    double                                 next_attack_at = 0;
    std::string                            archetype;
    const std::vector<CreatureAbility>*    anim = nullptr;   // the Blueprint's use-ability tasks when the creature attacks with its own animation
    NpcCastState                           cast;             // the attack the client is shown (combat/npc_ability_replicator.h)
    std::map<std::string, double>          cooldown_until;   // the name of a task's cooldown -> when it is over
    double                                 hold_until = 0;   // the creature stands still, facing as it does, until then (the montage of an attack it stands for)
    double                                 stand_off_cm = -1;   // a creature whose attacks launch projectiles closes in on its target to this distance (its tree's arrival distance) between two casts; -1: it does not
    double                                 shot_reach_cm = -1;  // ... and starts casting once its target is within this reach (the distance its tree lets it cast from)
    double                                 fight_began = 0;  // when its fight began (the tree asks how long it has gone on)
    double                                 link_radius_cm = 0;   // its pack: friends this close join when it begins a fight (the Blueprint's AggroProximityLinkRadius)
    bool                                   joined_pack = false;  // the fight it is about to begin is a pack-mate's: it calls nobody in, so a fight does not run on through a whole camp
    std::map<std::string, double>          fight_rolls;      // the numbers its tree rolled when this fight began, by the tree's variable (a first special attack comes only after one of them)
    uint64_t                               killer = 0;
    std::set<uint64_t>                     attackers;
    struct Shield {
        double amount = 0;
        double until = 0;   // 0: until used up
    };
    std::vector<Shield>                    shields;
    struct HeldEffect {
        const abilities::EffectDef*   def = nullptr;
        int                           stacks = 1;
        double                        start = 0, until = -1;   // until < 0: no end
        double                        next_tick = 0;           // 0: it does not tick
        std::vector<stats::EffectMod> mods;
        uint64_t                      by = 0;                  // the player whose hit left it
        int64_t                       key = 0;                 // its CurrentEffects item's EffectId
        int32_t                       array_id = -1;
        double                        taken = 0;               // the damage it has seen (an effect that a share of the holder's Health breaks)
    };
    std::vector<HeldEffect>                effects;
    uint8_t                                crowd = 0;          // what its crowd controls stop
    CrowdStops                             crowd_stops = CrowdStops::None;
    double                                 pace = 1.0;         // what its walking speed is multiplied by

    int effect_stacks(int64_t effect) const override {
        int stacks = 0;
        for (const HeldEffect& h : effects) {
            if (h.def && h.def->row.guid == effect) stacks += h.stacks;
        }
        return stacks;
    }

    /// A loss goes through the shields first (the oldest first); what is left reaches Health.
    double absorb(double loss, double now) {
        shields.erase(std::remove_if(shields.begin(), shields.end(), [&](const Shield& s) { return s.amount <= 0 || (s.until > 0 && s.until <= now); }), shields.end());
        for (Shield& s : shields) {
            const double take = std::min(s.amount, loss);
            s.amount -= take;
            loss -= take;
        }
        return loss;
    }
};

CombatWorld::CombatWorld(world::ActorRegistry& registry, npc::NpcWorld* npcs, const nav::AreaNav* nav, const CombatData& data, const abilities::Catalog& catalog,
                         const NpcStatBook& book, const npc::MotionTable& motion, CombatWorldConfig config)
    : registry_(registry), npcs_world_(npcs), nav_(nav), data_(data), catalog_(catalog), book_(book), motion_(motion), cfg_(std::move(config)),
      resolver_(catalog, data, cfg_.seed ? cfg_.seed : 1) {
    rng_.state = cfg_.seed * 0x9E3779B97F4A7C15ull + 7;
}

CombatWorld::~CombatWorld() = default;

// ---- players ----------------------------------------------------------------------------------------------------------------------

void CombatWorld::add_player(PlayerPort* port) {
    if (port && std::find(players_.begin(), players_.end(), port) == players_.end()) players_.push_back(port);
}

void CombatWorld::remove_player(PlayerPort* port) {
    players_.erase(std::remove(players_.begin(), players_.end(), port), players_.end());
    if (port) {
        player_combat_until_.erase(port->actor_id());
        drop_effects_of(port->actor_id());   // what a player put on the creatures ends with him
    }
}

PlayerPort* CombatWorld::player(uint64_t actor_id) {
    for (PlayerPort* p : players_) {
        if (p->actor_id() == actor_id) return p;
    }
    return nullptr;
}

void CombatWorld::player_exchange(uint64_t player_id, double now) {
    const double until = now + data_.in_combat_timer();
    const auto it = player_combat_until_.find(player_id);
    const bool was = it != player_combat_until_.end() && it->second > now;
    player_combat_until_[player_id] = until;
    if (!was) {
        if (PlayerPort* p = player(player_id)) p->set_in_combat(true, now);
    }
}

// ---- combatants -----------------------------------------------------------------------------------------------------------------------

CombatWorld::Npc* CombatWorld::make_combatant(const world::WorldActor& a) {
    if (a.kind != world::ActorKind::Npc) return nullptr;
    const std::string leaf = spawns::blueprint_leaf(a.npc.archetype);
    const npc::MotionStats bp = motion_.get(a.npc.archetype);
    auto n = std::make_unique<Npc>(*this, a.id, bp, leaf, a.npc.level);
    std::string err;
    n->cs = book_.make(bp, a.npc.level, err);
    if (!n->cs) {
        spdlog::warn("[combat] {} ({}): its stats do not run: {}", a.id, leaf, err);
        return nullptr;
    }
    // the status stats a hit's mods move (how much burn is left and for how long): a creature keeps a current value of each
    {
        std::set<int64_t> held;
        for (const int64_t stat : {data_.ids().status_burning, data_.ids().status_burning_dur}) {
            if (stat != 0) held.insert(stat);
        }
        n->cs->set_resources(std::move(held));
    }
    n->aligns.assign(bp.alignments.begin(), bp.alignments.end());
    n->hostile = data_.relation(n->aligns, CombatData::player_alignments()) == Relation::Hostile;
    n->kit = kit_of(leaf);
    npc::ai::Params p;
    p.aggro_radius_cm = a.npc.aggro_radius_cm > 0 ? a.npc.aggro_radius_cm : cfg_.default_aggro_cm;
    // who may start a fight by noticing a player: a creature hostile to players whose data does not switch its aggro off (retest 3: a Timid Grem, whose instance overrides the radius with 0, and a
    // huntable, whose alignment is Friendly, never attack first)
    p.proximity_aggro = notices_players(n->hostile, a.npc);
    p.leash_radius_cm = a.npc.leash_radius_cm > 0 ? a.npc.leash_radius_cm : std::max(cfg_.default_leash_cm, p.aggro_radius_cm * 3.0);
    n->link_radius_cm = cfg_.default_link_cm;
    if (const npc::SensesRow* row = senses_ ? senses_->find(a.npc.archetype) : nullptr) {
        n->link_radius_cm = std::max(0.0, row->link_radius);
        // what its Blueprint says: the aggro radius (the population's own when it gives one) scaled by the creature's level, the senses, how often it looks, where its fights anchor and how far they
        // may go.  A setting no Blueprint on its chain makes stands at the game's default.
        const double radius = a.npc.aggro_radius_cm > 0 ? a.npc.aggro_radius_cm : (row->aggro_radius >= 0 ? row->aggro_radius : kBlueprintAggroDefaultCm);
        p.aggro_radius_cm = radius * senses_->curve_factor(row->aggro_curve, a.npc.level);
        p.proximity_aggro = p.proximity_aggro && p.aggro_radius_cm > 0 && row->detect_players != 0;
        npc::ai::Senses& e = p.senses;
        e.enabled = true;
        e.sight_radius_cm = row->sight == 1 ? (row->sight_radius >= 0 ? row->sight_radius : kBlueprintSightDefaultCm) : 0.0;
        e.sight_vertical_cm = std::max(0.0, row->sight_vertical);
        e.sight_cone_deg = row->sight_cone >= 0 ? row->sight_cone : kBlueprintSightConeDefaultDeg;
        e.hearing_radius_cm = row->hearing == 1 ? (row->hearing_radius >= 0 ? row->hearing_radius : kBlueprintHearingDefaultCm) : 0.0;
        e.hearing_vertical_cm = std::max(0.0, row->hearing_vertical);
        p.sense_interval = row->pc_interval >= 0 ? row->pc_interval : kBlueprintPlayerLookSeconds;
        p.anchor_at_fight_start = row->leash_at_fight_start;
        if (a.npc.leash_radius_cm <= 0 && row->leash_distance >= 0) p.leash_radius_cm = row->leash_distance;
    }
    const double run = std::max({bp.max_run_speed, bp.max_walk_speed, 300.0});
    double chase = run;
    if (runs_at_stat_speed(a.npc.archetype)) {
        if (const stats::StatValue* mult = n->cs->find(data_.ids().move_speed_mult); mult && mult->value > 1.0) chase = run * mult->value;
    }
    p.chase_speed_cm_s = chase;
    p.run_home_speed_cm_s = run;
    // the goblins and bandits turn a fifth faster than their Blueprint's rotation rate
    if (npcs_world_ && runs_at_stat_speed(a.npc.archetype)) npcs_world_->set_yaw_rate(a.id, bp.rotation_rate_yaw * kHumanoidTurnGain);
    n->archetype = a.npc.archetype;
    n->anim = anim_ ? anim_->abilities_of(a.npc.archetype) : nullptr;
    // a creature whose tree has no attack it can play (a Blueprint with no open of its own, whose generic open lists none of the tree's abilities) keeps the single swing every creature had: the tree
    // would otherwise find nothing to use and the creature would stand and stare.  A tree that merely allows none right now (range, angle, cooldown, a roll) is not this: that creature waits.
    if (n->anim && !can_animate(*n)) n->anim = nullptr;
    stand_off_of(*n, n->stand_off_cm, n->shot_reach_cm);
    if (n->anim) {   // its attacks start only where its tree's conditions hold: the reach is not stretched once a fight begins, and it walks up to just inside it (a chase ends at 96 % of the reach)
        p.attack_hysteresis = 1.0;
        p.chase_stop_fraction = 0.96;
    }
    n->ai.set_params(p);
    n->next = choose_attack(*n);
    // the client sees the creature's own numbers from now on
    write_stats(registry_, a.id, *n->cs);
    write_elite_rank(registry_, a.id, static_cast<int>(n->cs->npc().elite_rank));
    Npc* raw = n.get();
    npcs_[a.id] = std::move(n);
    return raw;
}
// (C_Y_B | C.O.A)

CombatWorld::Npc* CombatWorld::combatant(uint64_t actor_id) {
    if (const auto it = npcs_.find(actor_id); it != npcs_.end()) return it->second.get();
    const world::WorldActor* a = registry_.find(actor_id);
    if (!a || a->kind != world::ActorKind::Npc) return nullptr;
    return make_combatant(*a);
}

PlayerPort* CombatWorld::player_pawn(uint64_t actor_id) {
    const world::WorldActor* a = registry_.find(actor_id);
    return a && a->kind == world::ActorKind::PlayerPawn ? player(actor_id) : nullptr;
}

Fighter* CombatWorld::fighter_of(uint64_t actor_id) {
    if (PlayerPort* p = player_pawn(actor_id)) return &p->fighter();
    return combatant(actor_id);
}

Delivery CombatWorld::apply_hit_to_player(uint64_t target, uint64_t attacker, const Resolved& r, double now, const HitOrigin* origin) {
    Delivery d;
    d.target_object = hit_component_of(target);
    PlayerPort* p = player_pawn(target);
    const abilities::HitDef* hit = origin ? origin->hit : nullptr;
    if (!p || (!p->fighter().alive() && !(hit && hit->resurrect != 0))) return d;
    d.applied = true;
    d.flags = r.flags;
    d.effective.assign(r.mods.size(), 0.0);
    IncomingHit in;
    in.instigator = attacker;
    in.from_player = true;
    in.flags = r.flags;
    in.mods = r.mods;
    in.at = now;
    if (origin) {
        in.source_type = origin->source_type;
        in.source_guid = origin->source_guid;
    }
    if (hit) {
        in.hit_guid = hit->row.guid;
        if (hit->max_remove > 0 || !hit->remove_effects.empty()) in.removes = hit;
    }
    // what the caster's confirmation and its reactions read (Divine Power rises by a heal's effect): the receiver's session applies the hit itself at its next tick
    stats::CharacterStats& s = p->fighter().stats();
    for (size_t i = 0; i < r.mods.size(); ++i) {
        const stats::StatValue* cur = s.find(r.mods[i].stat_guid);
        const stats::StatValue* max = r.mods[i].stat_guid == content::kGuidHealth ? s.find(content::kGuidMaxHealth) : r.mods[i].stat_guid == content::kGuidMana ? s.find(content::kGuidMaxMana) : nullptr;
        d.effective[i] = cur && max && r.mods[i].value > 0 ? std::clamp(r.mods[i].value, 0.0, std::max(0.0, static_cast<double>(max->value) - cur->value)) : r.mods[i].value;
    }
    p->receive_hit(in, now);
    return d;
}

std::vector<GatheredTarget> CombatWorld::allies_around(const Fighter& from, const Vec3& centre, double radius_cm, size_t max) {
    std::vector<GatheredTarget> out;
    for (PlayerPort* p : players_) {
        if (p->actor_id() == from.id() || !player_pawn(p->actor_id()) || !p->fighter().alive()) continue;
        const Vec3 at = p->fighter().position();
        const double d = distance_xy_cm(centre, at);
        if (d - p->fighter().radius_cm() > radius_cm || std::fabs(at.z - centre.z) - p->fighter().radius_cm() > radius_cm + 200.0) continue;
        out.push_back({p->actor_id(), d});
    }
    std::sort(out.begin(), out.end(), [](const GatheredTarget& a, const GatheredTarget& b) { return a.distance_cm < b.distance_cm; });
    if (out.size() > max) out.resize(max);
    return out;
}

void CombatWorld::apply_effect_to_player(uint64_t target, uint64_t attacker, const LandedEffect& e, double now) {
    PlayerPort* p = player_pawn(target);
    if (!p || !p->fighter().alive()) return;
    IncomingHit in;
    in.instigator = attacker;
    in.from_player = true;
    in.effects.push_back(e);
    in.at = now;
    p->receive_hit(in, now);
}

void CombatWorld::resurrect_player(uint64_t target, uint64_t caster, double now) {
    PlayerPort* p = player_pawn(target);
    PlayerPort* by = player(caster);
    if (p && by && !p->fighter().alive()) p->prompt_resurrect(by->character_guid(), now);
}

uint64_t CombatWorld::actor_of_object(uint64_t object_id) const {
    if (registry_.find(object_id)) return object_id;
    // the components the registry indexes: the cylinder (what the client names once the server exported it) and the interactable
    // [E1 stage 3 fix 3: no arithmetic on the id; the draft's "actor + 24" was another actor's component]
    if (const world::WorldActor* a = registry_.find_by_component(object_id)) return a->id;
    return 0;
}

uint64_t CombatWorld::hit_component_of(uint64_t actor_id) const {
    const world::WorldActor* a = registry_.find(actor_id);
    return a ? a->npc.collision_cylinder : 0;
}

TargetInfo CombatWorld::inspect(uint64_t target_actor, const Fighter& from) {
    TargetInfo t;
    const world::WorldActor* a = registry_.find(target_actor);
    if (PlayerPort* p = player_pawn(target_actor)) {   // another player: on the caster's side, as no fight between players is served
        Fighter& f = p->fighter();
        t.exists = true;
        t.position = f.position();
        t.radius_cm = f.radius_cm();
        t.alive = f.alive();
        t.relation = Relation::Friendly;
        t.surface_distance_cm = std::max(0.0, distance_xy_cm(from.position(), t.position) - from.radius_cm() - t.radius_cm);
        t.in_line_of_sight = true;
        if (p->actor_id() != from.id()) {
            t.other_player = true;
            PlayerPort* me = player(from.id());
            if (me && group_query_) t.group = group_query_(me->character_guid(), p->character_guid());
        }
        return t;
    }
    if (!a || a->kind != world::ActorKind::Npc) return t;
    t.exists = true;
    t.position = {a->x, a->y, a->z};
    const npc::MotionStats bp = motion_.get(a->npc.archetype);
    t.radius_cm = bp.radius;
    const auto it = npcs_.find(target_actor);
    t.alive = it == npcs_.end() ? true : !it->second->dead;
    std::vector<int64_t> aligns(bp.alignments.begin(), bp.alignments.end());
    t.relation = data_.relation(from.alignments(), aligns);
    const Vec3 me = from.position();
    t.surface_distance_cm = std::max(0.0, distance_xy_cm(me, t.position) - from.radius_cm() - t.radius_cm);
    t.in_line_of_sight = true;
    if (nav_) {
        const nav::Vec3 p = to_nav(me), q = to_nav(t.position);
        if (nav_->on_navmesh(p, 150, 300) && nav_->on_navmesh(q, 150, 300)) t.in_line_of_sight = nav_->line_of_sight(p, q);
    }
    return t;
}

std::vector<GatheredTarget> CombatWorld::enemies_in_cone(const Fighter& from, double angle_degrees, double length_cm, double behind_cm, size_t max) {
    std::vector<GatheredTarget> out;
    if (max == 0 || length_cm <= 0) return out;
    const Vec3 me = from.position();
    // the cone of a creature's attack record (bite_area.h): it starts `distance_center` ahead of the fighter (behind it: negative), reaches `distance` from there
    BiteArea cone;
    cone.distance = length_cm;
    cone.angle = angle_degrees;
    cone.distance_center = -behind_cm;
    HitSubject subject;
    subject.cx = me.x;
    subject.cy = me.y;
    subject.yaw = from.facing_degrees();
    std::vector<uint64_t> ids;
    registry_.query(me.x, me.y, length_cm + std::max(0.0, behind_cm), ids);
    for (const uint64_t id : ids) {
        const TargetInfo ti = inspect(id, from);
        if (!ti.exists || !ti.alive || ti.relation != Relation::Hostile) continue;   // (no sight check: the navmesh's 2D raycast stops at a bank or a ledge, and retail refused none of 361 recorded casts for sight)
        subject.tx = ti.position.x;
        subject.ty = ti.position.y;
        subject.tradius = ti.radius_cm;
        if (!validity_holds(cone, subject)) continue;
        out.push_back({id, distance_xy_cm(me, ti.position)});
    }
    std::sort(out.begin(), out.end(), [](const GatheredTarget& a, const GatheredTarget& b) { return a.distance_cm != b.distance_cm ? a.distance_cm < b.distance_cm : a.actor < b.actor; });
    if (out.size() > max) out.resize(max);
    return out;
}

std::vector<GatheredTarget> CombatWorld::enemies_in_line(const Fighter& from, double length_cm, double radius_cm, size_t max) {
    std::vector<GatheredTarget> out;
    if (max == 0 || length_cm <= 0) return out;
    const Vec3 me = from.position();
    const double yaw = from.facing_degrees() * bite_area_detail::kPi / 180.0;
    const double fx = std::cos(yaw), fy = std::sin(yaw);
    std::vector<uint64_t> ids;
    registry_.query(me.x, me.y, length_cm + radius_cm, ids);
    for (const uint64_t id : ids) {
        const TargetInfo ti = inspect(id, from);
        if (!ti.exists || !ti.alive || ti.relation != Relation::Hostile) continue;   // (no sight check: the navmesh's 2D raycast stops at a bank or a ledge, and retail refused none of 361 recorded casts for sight)
        const double dx = ti.position.x - me.x, dy = ti.position.y - me.y;
        const double ahead = dx * fx + dy * fy, aside = std::fabs(dy * fx - dx * fy);
        if (ahead + ti.radius_cm < 0 || ahead - ti.radius_cm > length_cm || aside > radius_cm + ti.radius_cm) continue;
        out.push_back({id, std::max(0.0, ahead)});
    }
    std::sort(out.begin(), out.end(), [](const GatheredTarget& a, const GatheredTarget& b) { return a.distance_cm != b.distance_cm ? a.distance_cm < b.distance_cm : a.actor < b.actor; });
    if (out.size() > max) out.resize(max);
    return out;
}

std::vector<GatheredTarget> CombatWorld::enemies_around(const Fighter& from, const Vec3& centre, double radius_cm, size_t max) {
    std::vector<GatheredTarget> out;
    if (max == 0 || radius_cm <= 0) return out;
    constexpr double kBodyMargin = 600.0;   // (the registry is asked a little wider than the sphere: a body is measured from its centre)
    std::vector<uint64_t> ids;
    registry_.query(centre.x, centre.y, radius_cm + kBodyMargin, ids);
    for (const uint64_t id : ids) {
        const TargetInfo ti = inspect(id, from);
        if (!ti.exists || !ti.alive || ti.relation != Relation::Hostile) continue;
        const double d = distance_xy_cm(centre, ti.position);
        if (d - ti.radius_cm > radius_cm) continue;
        // a sphere reaches up and down by its radius as well (the centre is on the ground: a body on a ledge far above it is out)
        if (std::fabs(ti.position.z - centre.z) - ti.radius_cm > radius_cm + 200.0) continue;
        if (!line_clear(centre, ti.position)) continue;
        out.push_back({id, d});
    }
    std::sort(out.begin(), out.end(), [](const GatheredTarget& a, const GatheredTarget& b) { return a.distance_cm != b.distance_cm ? a.distance_cm < b.distance_cm : a.actor < b.actor; });
    if (out.size() > max) out.resize(max);
    return out;
}

bool CombatWorld::ground_height(double x, double y, double z_hint, double* z) const {
    return nav_ != nullptr && nav_->ground_height(x, y, z_hint, z, 1500.0, 1500.0);
}

std::optional<npc::ai::State> CombatWorld::ai_state(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end()) return std::nullopt;
    return it->second->ai.state();
}

const npc::ai::NpcAi* CombatWorld::ai_of(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it == npcs_.end() ? nullptr : &it->second->ai;
}

double CombatWorld::health_of(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it == npcs_.end() ? -1.0 : it->second->health();
}

double CombatWorld::max_health_of(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it == npcs_.end() ? -1.0 : it->second->max_health();
}

bool CombatWorld::dead(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    return it != npcs_.end() && it->second->dead;
}

void CombatWorld::summaries(std::vector<Summary>& out) const {
    out.reserve(out.size() + npcs_.size());
    for (const auto& entry : npcs_) {
        const Npc& n = *entry.second;
        Summary s;
        s.id = entry.first;
        const double max = n.max_health(), now = n.health();
        const double pct = max > 0 ? std::clamp(now / max * 100.0, 0.0, 100.0) : 100.0;
        s.health_pct = static_cast<uint8_t>(std::lround(pct));
        if (!n.dead && now > 0 && s.health_pct == 0) s.health_pct = 1;   // alive is never shown as empty
        s.in_combat = n.flags_in_combat;
        s.dead = n.dead;
        out.push_back(s);
    }
}

std::string CombatWorld::next_attack_name(uint64_t actor_id) const {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end() || !it->second->next.ability) return {};
    return it->second->next.ability->row.name;
}

void CombatWorld::stop_cast(Npc& n) {
    if (npcs_world_) {
        npcs_world_->end_root_motion(n.actor_id);
        npcs_world_->stop_tracking(n.actor_id);   // (the turning that ran with the ability ends with it)
    }
    end_npc_cast(registry_, n.actor_id, n.cast);
}

void CombatWorld::forget(uint64_t actor_id) {
    if (const auto it = npcs_.find(actor_id); it != npcs_.end()) stop_cast(*it->second);
    flights_.erase(std::remove_if(flights_.begin(), flights_.end(), [&](const Flight& f) { return f.npc == actor_id; }), flights_.end());
    npcs_.erase(actor_id);
    stats_filled_.erase(actor_id);
}

bool CombatWorld::fill_stats(const world::WorldActor& a) {
    if (!stats_filled_.insert(a.id).second) return false;
    if (!has_stats(a)) return false;   // (a banner, a tent: nothing to fill)
    const npc::MotionStats bp = motion_.get(a.npc.archetype);
    std::string err;
    const auto cs = book_.make(bp, a.npc.level, err);
    if (!cs) {
        spdlog::warn("[combat] {} ({}): an NPC whose stats do not run keeps its template's: {}", a.id, spawns::blueprint_leaf(a.npc.archetype), err);
        return true;
    }
    write_stats(registry_, a.id, *cs);
    return true;
}

void CombatWorld::add_shield(uint64_t npc_actor, double amount, double seconds, double now) {
    Npc* n = combatant(npc_actor);
    if (!n || n->dead || !std::isfinite(amount) || amount <= 0) return;
    if (n->shields.size() >= 16) n->shields.erase(n->shields.begin());
    n->shields.push_back({amount, seconds > 0 ? now + seconds : 0.0});
}

double CombatWorld::shield_of(uint64_t npc_actor, double now) const {
    const auto it = npcs_.find(npc_actor);
    if (it == npcs_.end()) return 0;
    double total = 0;
    for (const Npc::Shield& s : it->second->shields) {
        if (s.amount > 0 && (s.until <= 0 || s.until > now)) total += s.amount;
    }
    return total;
}

void CombatWorld::add_threat(uint64_t npc_actor, uint64_t attacker, double amount, double now) {
    Npc* n = combatant(npc_actor);
    if (!n || n->dead) return;
    n->ai.notice(attacker, amount, now);
    n->attackers.insert(attacker);
}

// ---- attacks ----------------------------------------------------------------------------------------------------------------------

namespace {
// C_Y_B.C.O.A

/// Whether an ability does harm to enemies: some hit of it is aimed at enemies and moves Health.
/// An ability a creature's tree casts on its own side (a rally, a morale boost, a fire shield, a healer's touch): a hit with no Enemy in its mask that leaves an effect or moves Health.
bool helps_side(const abilities::AbilityDef& d, const abilities::Catalog& catalog, int64_t health_stat) {
    for (const content::AbilityHitRow& h : d.hits) {
        const abilities::HitDef* hit = catalog.hit(h.hit_guid);
        if (!hit || (hit->row.applicable_targets & 2) != 0 || (hit->row.applicable_targets & (1 | 4)) == 0) continue;
        if (!hit->effects.empty()) return true;
        for (const content::ModRow& m : hit->mods) {
            if (m.stat_guid == health_stat && m.value_expr != 0 && catalog.program(m.value_expr) >= 0) return true;
        }
    }
    return false;
}

bool is_attack(const abilities::AbilityDef& d, const abilities::Catalog& catalog, int64_t health_stat) {
    for (const content::AbilityHitRow& h : d.hits) {
        const abilities::HitDef* hit = catalog.hit(h.hit_guid);
        if (!hit || (hit->row.applicable_targets & 2) == 0) continue;
        for (const content::ModRow& m : hit->mods) {
            if (m.stat_guid == health_stat && m.value_expr != 0 && catalog.program(m.value_expr) >= 0) return true;
        }
    }
    return false;
}

}  // namespace

namespace {

/// The host of a montage's selection expression ("RandIntRange(0,2) == 0"): a random number from the world's source and nothing else.
class SelectHost final : public stats::ExprHost {
public:
    explicit SelectHost(stats::SplitMix& rng) : rng_(rng) {}
    double rand_double() override { return rng_.rand_double(); }
    double host_fn(int fn, const std::string&, const double* args, int) override {
        if (fn != stats::kHostRandIntRange) return 0;
        const double lo = std::floor(args[0]), hi = std::floor(args[1]);
        return hi < lo ? lo : lo + std::floor(rng_.rand_double() * (hi - lo + 1.0));
    }

private:
    stats::SplitMix& rng_;
};

/// No hit lands sooner than this after its attack begins: the client has its animation by then, and the confirmation never overtakes it.
constexpr double kMinHitDelay = 0.1;

/// The angle between two directions in degrees, 0 to 180.
double angle_between_degrees(double a, double b) {
    double d = std::fmod(a - b, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d < -180.0) d += 360.0;
    return std::fabs(d);
}

}  // namespace

CombatWorld::Attack CombatWorld::choose_attack(Npc& n) {
    Attack a;
    a.windup_ms = static_cast<int>(cfg_.default_windup * 1000);
    a.range_cm = static_cast<int>(cfg_.default_attack_range_cm);
    if (n.anim) {
        // a creature with its own animation uses what its Blueprint's tree says (the tasks with a condition the tables cannot say are left alone)
        const Attack b = choose_animated(n, nullptr);
        if (b.ability) return b;
    } else if (n.kit) {
        struct Cand {
            const abilities::AbilityDef* def;
            const npc_kits::KitAbility*  k;
        };
        std::vector<Cand> cands;
        double total = 0;
        for (size_t i = 0; i < n.kit->count; ++i) {
            const npc_kits::KitAbility& k = n.kit->abilities[i];
            const abilities::AbilityDef* d = catalog_.ability_named(k.name);
            if (!d || !is_attack(*d, catalog_, data_.ids().health)) continue;
            cands.push_back({d, &k});
            total += std::max(1, k.uses);
        }
        if (!cands.empty()) {
            double r = rng_.rand_double() * total;
            for (const Cand& c : cands) {
                r -= std::max(1, c.k->uses);
                if (r <= 0 || &c == &cands.back()) {
                    a.ability = c.def;
                    a.windup_ms = c.k->windup_ms;
                    a.range_cm = c.k->range_cm;
                    return a;
                }
            }
        }
    }
    if (const abilities::AbilityDef* d = catalog_.ability_named(cfg_.fallback_ability)) {
        if (is_attack(*d, catalog_, data_.ids().health)) a.ability = d;
    }
    return a;
}

CombatWorld::Attack CombatWorld::choose_animated(Npc& n, const Fighter* target, TreeWhy* why) {
    Attack a;
    a.windup_ms = static_cast<int>(cfg_.default_windup * 1000);
    a.range_cm = static_cast<int>(cfg_.default_attack_range_cm);
    if (!n.anim) return a;
    // how far the target is from the creature's surface (the AI measures its attack range so) and how far off the way the creature faces it stands: the tree's two conditions, asked every time
    double distance = -1, angle = 0;
    CondContext ctx;
    ctx.preview = target == nullptr;
    ctx.has_target = target != nullptr;
    if (target) {
        const world::WorldActor* w = registry_.find(n.actor_id);
        if (!w) return a;
        const Vec3 at = target->position();
        distance = distance_xy_cm(Vec3{w->x, w->y, w->z}, at) - n.radius_cm() - target->radius_cm();
        angle = angle_between_degrees(std::atan2(at.y - w->y, at.x - w->x) * 180.0 / 3.14159265358979323846, w->yaw_degrees);
        ctx.surface_distance = distance;
        ctx.angle = angle;
        ctx.centre_distance = std::sqrt((at.x - w->x) * (at.x - w->x) + (at.y - w->y) * (at.y - w->y) + (at.z - w->z) * (at.z - w->z));
        const Vec3 from{w->x, w->y, w->z};
        ctx.sees = [this, from, at]() { return line_clear(from, at); };
    }
    ctx.health_fraction = n.max_health() > 0 ? std::clamp(n.health() / n.max_health(), 0.0, 1.0) : 1.0;
    ctx.fight_seconds = std::max(0.0, now_ - n.fight_began);
    ctx.cooldown_over = [&n, this](const std::string& g) {
        const auto until = n.cooldown_until.find(g);
        return until == n.cooldown_until.end() || now_ >= until->second;
    };
    ctx.roll = [this]() { return rng_.rand_double(); };
    // STC_HasEffect_C: an effect the creature itself holds (its rally, its fire shield), or its target (a player's poison, a mark)
    ctx.self_effect = [this, id = n.actor_id](int64_t effect) { return effect_stacks(id, effect); };
    if (target) ctx.target_effect = [target](int64_t effect) { return target->effect_stacks(effect); };
    // GetNpcEliteRank(GetSource()), and "someone on the hate list nearer than D" (GetHatedCharacters filtered by GetDistanceBetween: centre to centre)
    ctx.elite = n.cs ? static_cast<int>(n.cs->npc().elite_rank) : 0;
    if (const world::WorldActor* self = registry_.find(n.actor_id)) {
        const Vec3 from{self->x, self->y, self->z};
        ctx.hated_between = [this, &n, from, target](double lo, double hi) {
            const auto near = [&](const Fighter* f) {
                if (!f || !f->alive()) return false;
                const Vec3 p = f->position();
                const double d2 = (p.x - from.x) * (p.x - from.x) + (p.y - from.y) * (p.y - from.y) + (p.z - from.z) * (p.z - from.z);
                return d2 < hi * hi && (lo < 0 || d2 > lo * lo);
            };
            if (near(target)) return true;
            for (const npc::ai::ThreatTable::Entry& e : n.ai.threat().entries()) {
                if (near(fighter_of(e.id))) return true;
            }
            return false;
        };
    }
    // a number the tree rolls once when a fight begins (its first special attack waits for it): rolled the first time it is asked, the same until the fight is over
    ctx.fight_roll = [&n, this](const CondAtom& a) {
        const auto known = n.fight_rolls.find(a.name);
        if (known != n.fight_rolls.end()) return known->second;
        double v = a.value;
        if (rng_.rand_double() < a.chance) v = a.low + (a.high - a.low) * rng_.rand_double();
        n.fight_rolls.emplace(a.name, v);
        return v;
    };
    // the tree is tried in order and the first task whose conditions hold is used
    for (const CreatureAbility& t : *n.anim) {
        if (t.unknown || t.off || !t.fight) continue;
        const abilities::AbilityDef* d = catalog_.ability(t.ability);
        if (!d || (!is_attack(*d, catalog_, data_.ids().health) && !helps_side(*d, catalog_, data_.ids().health)) || !animates(n, *d)) continue;
        bool ready = true;
        for (const std::string& g : t.gates) {
            const auto until = n.cooldown_until.find(g);
            if (until != n.cooldown_until.end() && now_ < until->second) ready = false;
        }
        if (!ready) continue;
        if (target) {
            if (t.max_distance >= 0 && distance > t.max_distance) {
                if (why) {
                    why->too_far = true;
                    if (t.min_distance <= 0 && (why->reach_cm < 0 || t.max_distance < why->reach_cm)) why->reach_cm = t.max_distance;
                }
                continue;
            }
            if (t.min_distance >= 0 && distance < t.min_distance) continue;
            if (t.max_angle >= 0 && angle > t.max_angle) {
                if (why) why->off_angle = true;
                continue;
            }
        } else if (t.min_distance > 0) {
            continue;   // a task for a target far away is not the attack the creature stands ready with
        }
        // the tree's other conditions (a roll, its health, how long the fight has gone on, whether it sees its target ...), asked afresh at every try
        bool holds = true;
        for (const CondGroup& g : t.groups) {
            if (!eval_group(g, ctx)) {
                holds = false;
                break;
            }
        }
        if (!holds) continue;
        a.ability = d;
        a.task = &t;
        if (t.max_distance >= 0) a.range_cm = static_cast<int>(t.max_distance);
        return a;
    }
    return a;
}

bool CombatWorld::line_clear(const Vec3& from, const Vec3& to) const {
    if (!nav_) return true;
    const nav::Vec3 p = to_nav(from), q = to_nav(to);
    if (!nav_->on_navmesh(p, 150, 300) || !nav_->on_navmesh(q, 150, 300)) return true;   // off the navmesh the ground says nothing
    return nav_->line_of_sight(p, q);
}

bool CombatWorld::hit_reaches(const Npc& n, const Fighter& body, const BiteArea& area, bool is_target) const {
    const world::WorldActor* w = registry_.find(n.actor_id);
    if (!w) return false;
    const Vec3 at = body.position();
    HitSubject s;
    s.cx = w->x;
    s.cy = w->y;
    s.yaw = w->yaw_degrees;
    s.tx = at.x;
    s.ty = at.y;
    s.tradius = body.radius_cm();
    const Vec3 from{w->x, w->y, w->z};
    return hit_lands(area, s, is_target, [this, from, at]() { return line_clear(from, at); });
}

uint64_t CombatWorld::aimed_at(const Npc& n, const BiteArea& area, uint64_t cast_target) {
    // the record finds its target the way it says: what the creature fights now, the one the cast was aimed at, or both in an order
    using D = BiteArea::Discovery;
    PlayerPort* locked = player(cast_target);
    const bool locked_ok = locked && locked->fighter().alive();
    const uint64_t fighting = n.ai.target();
    PlayerPort* active = fighting ? player(fighting) : nullptr;
    const bool active_ok = active && active->fighter().alive();
    switch (area.discovery) {
        case D::PrefersActive: return active_ok ? fighting : (locked_ok ? cast_target : 0);
        case D::OnlyActive: return active_ok ? fighting : 0;
        case D::OnlyLocked: return locked_ok ? cast_target : 0;
        case D::PrefersLocked: return locked_ok ? cast_target : (active_ok ? fighting : 0);
        case D::None: break;
    }
    return 0;
}

bool CombatWorld::can_animate(const Npc& n) const {
    if (!anim_ || !n.anim) return false;
    for (const CreatureAbility& t : *n.anim) {
        if (t.unknown || t.off || !t.fight) continue;
        const abilities::AbilityDef* d = catalog_.ability(t.ability);
        if (d && is_attack(*d, catalog_, data_.ids().health) && animates(n, *d)) return true;
    }
    return false;
}

bool CombatWorld::launches_projectiles(const abilities::AbilityDef& def) const {
    if (!anim_) return false;
    // a notify that fires counts where plan_cast flies something from it: the ability carries a projectile row of the notify's key, and the projectile is in the data (an ability whose notify
    // names a key that no projectile row has would stand at its shot reach and repeat a cast that shows nothing)
    for (const content::MontageRow& m : def.montages) {
        const MontageData* md = m.soft_path.empty() ? nullptr : anim_->montage(m.soft_path);
        if (!md) continue;
        for (const MontageData::Fire& fire : md->fires) {
            for (const content::AbilityHitRow& r : def.hits) {
                if (r.source == "projectile" && r.key == fire.key && catalog_.projectile(r.owner_guid)) return true;
            }
        }
    }
    return false;
}

void CombatWorld::stand_off_of(const Npc& n, double& arrival, double& reach) const {
    arrival = reach = -1;
    if (!n.anim) return;
    for (const CreatureAbility& t : *n.anim) {
        if (t.unknown || t.off || !t.fight || !(t.arrival_cm > 0)) continue;
        const abilities::AbilityDef* d = catalog_.ability(t.ability);
        if (!d || !is_attack(*d, catalog_, data_.ids().health) || !animates(n, *d) || !launches_projectiles(*d)) continue;
        arrival = t.arrival_cm;
        reach = std::max(reach, t.max_distance > 0 ? t.max_distance : t.arrival_cm);   // (the farthest any of its shots may be cast from)
    }
}

bool CombatWorld::montages_readable(const abilities::AbilityDef& def) const {
    if (!anim_) return false;
    bool data = false;
    for (const content::MontageRow& m : def.montages) {
        if (m.soft_path.empty()) continue;
        const MontageData* md = anim_->montage(m.soft_path);
        if (!md) return false;
        if (md->root_unreadable) continue;   // its animation moves the creature in a way the tables could not read: it is not played (plan_cast leaves it out); an ability with no other keeps the single swing
        data = true;
    }
    return data;
}

bool CombatWorld::areas_usable(const abilities::AbilityDef& def) const {
    if (!anim_) return false;
    for (const content::AbilityHitRow& r : def.hits) {
        if (r.source != "hit") continue;
        const abilities::HitDef* hit = catalog_.hit(r.hit_guid);
        if (!hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) continue;   // (only a hit that hurts an enemy lands by an area)
        const BiteArea* area = anim_->area_of(def.row.guid, static_cast<int>(r.key));
        if (!area || !area->usable()) return false;
    }
    return true;
}

bool CombatWorld::grabs_target(const abilities::AbilityDef& def) const {
    for (const content::AbilityHitRow& r : def.hits) {
        if (r.source != "hit") continue;
        const abilities::HitDef* hit = catalog_.hit(r.hit_guid);
        if (!hit) continue;
        for (const content::HitEffectRow& applied : hit->effects) {
            if (applied.kind != "apply") continue;
            const abilities::EffectDef* effect = catalog_.effect(applied.effect_guid);
            if (effect && std::find(effect->row.tags.begin(), effect->row.tags.end(), "Debuff.Tripped") != effect->row.tags.end()) return true;
        }
    }
    return false;
}

bool CombatWorld::animates(const Npc& n, const abilities::AbilityDef& def) const {
    if (!anim_ || !n.anim) return false;
    bool listed = false;
    for (const CreatureAbility& t : *n.anim) listed = listed || t.ability == def.row.guid;
    if (!listed) return false;
    const world::WorldActor* a = registry_.find(n.actor_id);
    if (!a || !npc_knows_ability(*a, def.row.guid)) return false;
    return montages_readable(def) && areas_usable(def) && !grabs_target(def);   // (an ability whose hit lands by a volume the world cannot place, or that grabs its target, keeps the creature's single swing)
}

std::string CombatWorld::GateCensus::text() const {
    return "creature attacks by where their hits land: " + std::to_string(any) + " on the target wherever it stands, " + std::to_string(validity) + " inside a cone, " + std::to_string(volumes) +
           " by volumes; " + std::to_string(kept_swing) + " keep the single swing (a volume that cannot be placed); " + std::to_string(unplayed) + " have no hit of their montage to decide by (a thrown rock, a bolt)";
}

CombatWorld::GateCensus CombatWorld::gate_census() const {
    GateCensus out;
    if (!anim_) return out;
    using G = BiteArea::Gate;
    std::set<int64_t> seen;
    for (const auto& [archetype, tasks] : anim_->by_kind()) {
        for (const CreatureAbility& t : tasks) {
            if (t.unknown || t.off || !t.fight || !seen.insert(t.ability).second) continue;
            const abilities::AbilityDef* d = catalog_.ability(t.ability);
            if (!d || !is_attack(*d, catalog_, data_.ids().health) || !montages_readable(*d)) continue;
            if (!areas_usable(*d)) {
                ++out.kept_swing;
                out.kept_names.push_back(d->row.name);
                continue;
            }
            G gate = G::None;   // the ability's first hit that hurts an enemy decides which kind it counts as
            bool found = false;
            for (const content::AbilityHitRow& r : d->hits) {
                const abilities::HitDef* hit = catalog_.hit(r.hit_guid);
                if (r.source != "hit" || !hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) continue;
                const BiteArea* area = anim_->area_of(d->row.guid, static_cast<int>(r.key));
                if (area) gate = area->gate();
                found = true;
                break;
            }
            if (!found) {
                ++out.unplayed;
                out.unplayed_names.push_back(d->row.name);
                continue;
            }
            ++(gate == G::Validity ? out.validity : gate == G::Volumes ? out.volumes : out.any);
            (gate == G::Validity ? out.validity_names : gate == G::Volumes ? out.volume_names : out.any_names).push_back(d->row.name);
        }
    }
    return out;
}

bool CombatWorld::plan_cast(Npc& n, const abilities::AbilityDef& def, CastPlan& out) {
    out = CastPlan{};
    if (!animates(n, def)) return false;
    // the montage: the first of the ability's whose selection expression holds (a row with none always does; the last one stays when none holds)
    const content::MontageRow* chosen = nullptr;
    std::string err;
    std::unique_ptr<stats::CharacterStats::Evaluation> evaluation;
    SelectHost host(rng_);
    for (const content::MontageRow& m : def.montages) {
        if (m.soft_path.empty()) continue;
        const MontageData* playable = anim_->montage(m.soft_path);
        if (!playable || playable->root_unreadable) continue;   // (a variation whose travel the tables could not read is not played: the client would carry its copy of the creature along it and the server would not)
        chosen = &m;
        if (m.select_expr == 0) break;
        const content::ExprRow* row = catalog_.expr(m.select_expr);
        if (!row || row->program < 0) continue;
        if (!evaluation) evaluation = n.stats().evaluation(n.worn(), err, true);
        double v = 0;
        std::string e;
        if (evaluation && evaluation->eval(row->program, host, v, e) && v != 0.0) break;
    }
    if (!chosen) return false;
    const MontageData* montage = anim_->montage(chosen->soft_path);
    if (!montage || montage->length <= 0 || montage->rate_scale <= 0) return false;
    // every hit notify fires the hit record of the ability that carries the notify's key
    for (const MontageData::Hit& h : montage->hits) {
        for (const content::AbilityHitRow& r : def.hits) {
            if (r.source != "hit" || r.key != h.key) continue;
            out.hits.push_back({h.time / montage->rate_scale, r.hit_guid, static_cast<int>(r.key)});
            break;
        }
    }
    // every notify that launches a projectile flies the projectile of the ability that carries the notify's key; what it lands on arrival is the hits its record lists
    for (const MontageData::Fire& f : montage->fires) {
        PlannedFire fire;
        fire.delay = f.time / montage->rate_scale;
        for (const content::AbilityHitRow& r : def.hits) {
            if (r.source != "projectile" || r.key != f.key) continue;
            fire.projectile = r.owner_guid;
            if (std::find(fire.hits.begin(), fire.hits.end(), r.hit_guid) == fire.hits.end()) fire.hits.push_back(r.hit_guid);
        }
        if (fire.projectile != 0 && catalog_.projectile(fire.projectile)) out.fires.push_back(std::move(fire));
    }
    if (out.hits.empty() && out.fires.empty()) return false;
    out.montage = montage;
    out.seconds = montage->seconds(1.0);
    return true;
}

void CombatWorld::start_attack(Npc& n, uint64_t target, double now, const Attack* picked) {
    PlayerPort* port = player(target);
    // An attack with its own animation.  The tree picks it with the target where it stands; its montage starts on the client with the element the replicator writes; the creature turns
    // to the target, and when the tree says so stands still for the whole montage and keeps that facing; every hit lands at its notify, by the hit's targeting record
    // (on the target wherever it stands unless the record measures, and on everyone inside its volumes); the next attack comes when the montage is over.
    Attack a = n.next;
    if (n.anim && port) {
        a = picked ? *picked : choose_animated(n, &port->fighter());
        if (!a.ability) return;   // nothing the tree allows with the target where it stands: the creature tries again at its next look
    }
    // silenced, it casts no magic; disarmed, it swings nothing [modelled: the records name ability groups that are not read; the attack's element stands for them]
    if (a.ability && (n.crowd_stops == CrowdStops::Magic || n.crowd_stops == CrowdStops::Physical)) {
        const std::string_view stopped = n.crowd_stops == CrowdStops::Magic ? "Element.Magical" : "Element.Physical";
        for (const std::string& tag : a.ability->row.tags) {
            if (tag.rfind(stopped, 0) == 0) return;
        }
    }
    CastPlan plan;
    bool animated = false;
    Vec3 at;
    if (a.ability && n.anim && port && plan_cast(n, *a.ability, plan)) {
        const auto montage = bootstrap::montage_for(plan.montage->path, plan.montage->guid, plan.montage->package_guid);
        if (montage) {
            NpcCast cast;
            cast.ability = a.ability->row.guid;
            cast.target = target;
            cast.target_guid = port->cylinder_guid();   // (the client's targeting finds the player by his collision cylinder's NetGUID, in full)
            at = port->fighter().position();
            cast.tx = at.x;
            cast.ty = at.y;
            cast.tz = at.z;
            cast.montage = plan.montage->guid;
            // The creature stops where it is for the attack (the chase it was in ends with the state of the attack) and keeps the facing it has: a task that stands for its montage and does not turn
            // the creature during it began facing its target within a few degrees (think waits for that), and a task that does turn it does so from here, at the creature's rate, below.
            if (a.task && npcs_world_) npcs_world_->halt(n.actor_id);
            animated = start_npc_cast(registry_, n.actor_id, n.cast, cast, montage->chain);
            if (animated && !plan.montage->root_motion.empty()) {
                // The montage carries the creature (a lunge) and the client moves its copy along it by itself, so the world moves the creature the same way at its own rate and says where it
                // is.  A body in the way (a player's capsule) stops it; the AI brings it back into range afterwards when the lunge left it out of range.
                bool carried = false;
                if (npcs_world_) {
                    npc::NpcWorld::RootMotionPlay play;
                    play.montage = plan.montage->guid;
                    play.length = plan.montage->length;
                    play.rate = plan.montage->rate_scale;
                    // The client turns the animation's root translation into the world's by the mesh component's world transform, scale included (USkeletalMeshComponent::ConvertLocalRootMotionToWorld
                    // multiplies the translation by the component's Scale3D), and puts its copy of the creature a capsule as big as that scale.  A goblin of scale 1.5 lunges 1.5 times what its montage
                    // says, and our copy of it has to cover the same ground, or the client's copy runs ahead of ours between two updates and is put back at every one of them.
                    const world::WorldActor* me = registry_.find(n.actor_id);
                    const double scale = me ? npc::actor_scale_z(*me) : 1.0;
                    for (const MontageData::Step& s : plan.montage->root_motion) play.steps.push_back({s.time, s.x * scale, s.y * scale});
                    const double own_radius = n.radius_cm() * scale;
                    play.own_radius = own_radius;
                    if (const RootMotionFreezeRule* rule = anim_ ? anim_->freeze_of(a.ability->row.guid) : nullptr) {
                        play.freeze_stop = rule->kind == RootMotionFreezeRule::Kind::Stop;
                        play.freeze_pause = rule->kind == RootMotionFreezeRule::Kind::Pause;
                        play.freeze_distance = rule->distance;
                        play.freeze_scale = rule->radius_scale;
                        play.ignore_non_target = rule->ignore_non_target;
                    }
                    play.blockers = [this, own_radius, target](std::vector<npc::NpcWorld::RootMotionBlock>& out) {
                        for (PlayerPort* p : players_) {
                            if (!p->fighter().alive()) continue;
                            const Vec3 at = p->fighter().position();
                            out.push_back({at.x, at.y, own_radius + p->fighter().radius_cm(), p->fighter().radius_cm(), p->actor_id() == target});
                        }
                    };
                    carried = npcs_world_->begin_root_motion(n.actor_id, std::move(play));
                }
                if (!carried) {   // nothing can move it along the montage, so the client's copy would slide away from it: it plays no montage
                    end_npc_cast(registry_, n.actor_id, n.cast);
                    animated = false;
                }
            }
        }
    }
    if (animated) {
        // where the tables have the montage's animation-end notify (a bandit's), the creature's task is done there and the cooldowns it starts run from there; else they run from the cast's start
        const double done = plan.montage ? plan.montage->task_done_after(1.0) : 0.0;
        if (a.task && !a.task->task.empty() && a.task->cooldown > 0) n.cooldown_until[a.task->task] = now + done + a.task->cooldown;
        if (a.task) {   // (a timer the tree rolls anew each time takes a fresh number of seconds between its low and its high)
            for (const CooldownStart& c : a.task->starts) n.cooldown_until[c.name] = now + done + (c.high > c.low ? c.low + (c.high - c.low) * rng_.rand_double() : c.low);
        }
        if (a.task) n.hold_until = now + plan.seconds;   // (no chase runs in the state of an attack: the creature stands, and turns, until the montage is over)
        n.next_attack_at = now + plan.seconds;
        if (a.task && a.task->rotates && npcs_world_) {   // the tree turns it towards its target while the ability plays (the Rotate To task that runs with it)
            const uint64_t who = target;
            const double until = now + (a.task->rotate_seconds > 0 ? std::min(a.task->rotate_seconds, plan.seconds) : plan.seconds);
            npcs_world_->track_target(
                n.actor_id,
                [this, who](nav::Vec3& out) {
                    PlayerPort* p = player(who);
                    if (!p || !p->fighter().alive()) return false;
                    out = to_nav(p->fighter().position());
                    return true;
                },
                a.task->rotate_speed, until);
        }
    } else {
        const double interval = (n.kit && n.kit->interval_ms > 0 ? n.kit->interval_ms / 1000.0 : cfg_.default_attack_interval);
        n.next_attack_at = now + interval * (1.0 + cfg_.attack_variance * (2.0 * rng_.rand_double() - 1.0));
    }
    n.next = choose_attack(n);
    if (!a.ability) {
        ++stats_.refused_no_attack;
        return;
    }
    Pending p;
    p.npc = n.actor_id;
    p.target = target;
    p.ability = a.ability;
    if (animated) {
        if (npc_casts_logged_ < kLoggedNpcCasts) {
            ++npc_casts_logged_;
            p.logged = true;
            spdlog::info("[combat] {} ({}) plays {}: {} hit(s) and {} projectile(s) over {:.2f} s{}", n.actor_id, n.leaf, a.ability->row.name, plan.hits.size(), plan.fires.size(), plan.seconds,
                         a.task && a.task->stop_movement ? ", standing still" : "");
        }
        p.animated = true;
        for (const PlannedHit& h : plan.hits) {
            p.timed_hits.emplace_back(now + std::max(h.delay, kMinHitDelay), h.hit);
            const BiteArea* area = anim_ ? anim_->area_of(a.ability->row.guid, h.key) : nullptr;
            p.timed_areas.push_back(area ? *area : BiteArea{});   // (each hit lands by its own record, when it falls due)
        }
        for (size_t i = 1; i < p.timed_hits.size(); ++i) {   // the notifies are in time order; the lower bound keeps them so
            p.timed_hits[i].first = std::max(p.timed_hits[i].first, p.timed_hits[i - 1].first);
        }
        for (const PlannedFire& fire : plan.fires) p.timed_fires.emplace_back(now + std::max(fire.delay, kMinHitDelay), fire);
        for (size_t i = 1; i < p.timed_fires.size(); ++i) p.timed_fires[i].first = std::max(p.timed_fires[i].first, p.timed_fires[i - 1].first);
        p.end_at = now + plan.seconds;
        p.due = p.end_at;
        if (!p.timed_hits.empty()) p.due = std::min(p.due, p.timed_hits.front().first);
        if (!p.timed_fires.empty()) p.due = std::min(p.due, p.timed_fires.front().first);
    } else {
        p.due = now + a.windup_ms / 1000.0;
        std::set<int64_t> seen;
        for (const content::AbilityHitRow& h : a.ability->hits) {
            if ((h.source == "hit" || h.source == "projectile") && seen.insert(h.hit_guid).second) p.hits.push_back(h.hit_guid);
        }
    }
    pending_.push_back(std::move(p));
    ++stats_.npc_attacks;
}

void CombatWorld::land_on_friends(uint64_t npc, const abilities::HitDef& hit, double now) {
    constexpr double kFriendRadius = 1500.0;   // [modelled: the records' own volumes are not placed for these]
    constexpr size_t kMostFriends = 8;
    const auto self = npcs_.find(npc);
    bool heals = false;
    for (const content::ModRow& m : hit.mods) heals = heals || m.stat_guid == data_.ids().health;
    if (self == npcs_.end() || self->second->dead || (hit.effects.empty() && !heals) || (hit.row.applicable_targets & (1 | 4)) == 0) return;
    if (hit.row.name.find("Knockback") != std::string::npos) return;   // (one that moves what it reaches is not for its own side here)
    Npc& n = *self->second;
    std::vector<uint64_t> friends{npc};
    if ((hit.row.applicable_targets & 4) != 0) {
        for (const auto& [id, other] : npcs_) {
            if (friends.size() >= kMostFriends) break;
            if (id == npc || other->dead || other->alignments() != n.alignments()) continue;
            if (distance_xy_cm(other->position(), n.position()) <= kFriendRadius) friends.push_back(id);
        }
    }
    // a heal goes to the one most hurt, and to nobody when nobody is
    if (heals) {
        uint64_t worst = 0;
        double lowest = 1.0;
        for (const uint64_t id : friends) {
            const double max = max_health_of(id), share = max > 0 ? health_of(id) / max : 1.0;
            if (share < lowest) {
                lowest = share;
                worst = id;
            }
        }
        friends.clear();
        if (worst != 0) friends.push_back(worst);
    }
    for (const uint64_t id : friends) {
        Npc* other = combatant(id);
        if (!other) continue;
        if (heals) {
            Resolved healed;
            HitMeta meta;
            std::string why;
            if (resolver_.resolve(n, *other, hit, meta, healed, why)) {
                healed.mods.erase(std::remove_if(healed.mods.begin(), healed.mods.end(), [&](const ResolvedMod& m) { return !(m.value > 0); }), healed.mods.end());
                if (!healed.mods.empty()) apply_hit_to_npc(id, npc, healed, now);
            }
        }
        std::vector<LandedEffect> landed;
        resolver_.effects_of(n, *other, hit, landed);
        for (const LandedEffect& e : landed) apply_effect_to_npc(id, e, npc, now);
    }
}

void CombatWorld::spawn_hit_zones(uint64_t npc, const abilities::HitDef& hit, const Vec3& at, double now) {
    constexpr size_t kMaxGroundZones = 256;   // (a bound: a zone lasts seconds and creatures cast them seconds apart)
    const auto it = npcs_.find(npc);
    if (it == npcs_.end()) return;
    for (const int64_t guid : hit.zones) {
        const content::ZoneRow* row = catalog_.zone(guid);
        if (!row || !(row->life_time > 0) || !(row->radius > 0) || ground_zones_.size() >= kMaxGroundZones) continue;
        GroundZone z;
        z.npc = npc;
        z.row = row;
        z.centre = at;
        z.radius = row->radius;
        z.period = row->tick_rate > 0 ? row->tick_rate : 1.0;
        z.tick_due = now + z.period;   // (bNoHitOnFirstTick)
        z.end_at = now + row->life_time;
        land_zone_hits(*it->second, z, "InitialHitsIds", now);
        ground_zones_.push_back(std::move(z));
    }
}

void CombatWorld::land_zone_hits(Npc& n, GroundZone& z, const char* role, double now) {
    const auto hits = z.row->hits.find(role);
    if (hits == z.row->hits.end()) return;
    const bool overlap = std::string_view(role) == "OnBeginOverlapHitsIds";
    HitMeta meta;
    meta.source_type = content::kSourceTypeZone;
    meta.source_guid = z.row->guid;
    const std::vector<PlayerPort*> present = players_;   // (a hit may take a player out of the list)
    for (PlayerPort* q : present) {
        if (!q->fighter().alive() || distance_xy_cm(q->fighter().position(), z.centre) > z.radius + q->fighter().radius_cm()) continue;
        if (overlap && !z.overlapped.insert(q->actor_id()).second) continue;
        for (const int64_t guid : hits->second) {
            const abilities::HitDef* hit = catalog_.hit(guid);
            if (hit && (hit->row.applicable_targets & 2) != 0) deliver_hit_with(n, *q, *hit, meta, now);
        }
    }
}

void CombatWorld::step_zones(double now) {
    for (size_t i = 0; i < ground_zones_.size();) {
        const auto it = npcs_.find(ground_zones_[i].npc);
        if (it == npcs_.end()) {   // (its creature was forgotten)
            ground_zones_.erase(ground_zones_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        GroundZone z = ground_zones_[i];   // (a hit may reach into the world: the list is not trusted across it)
        land_zone_hits(*it->second, z, "OnBeginOverlapHitsIds", now);
        while (z.tick_due <= now && z.tick_due <= z.end_at) {
            z.tick_due += z.period;
            land_zone_hits(*it->second, z, "LingeringHitsIds", now);
        }
        const bool over = z.end_at <= now;
        if (over) land_zone_hits(*it->second, z, "EndHitsIds", now);
        if (i >= ground_zones_.size()) break;
        if (over) {
            ground_zones_.erase(ground_zones_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ground_zones_[i] = std::move(z);
            ++i;
        }
    }
}

void CombatWorld::deliver_hit(Npc& n, PlayerPort& port, const abilities::AbilityDef& ability, int64_t guid, double now, int64_t projectile) {
    const abilities::HitDef* hit = catalog_.hit(guid);
    if (!hit) return;
    // what it leaves on the ground where it lands, itself or through the hit it fires there (the shaman's bomb: an impact hit whose only work is the ooze)
    spawn_hit_zones(n.actor_id, *hit, port.fighter().position(), now);
    for (const content::RecursiveHitRow& r : hit->recursive) {
        const abilities::HitDef* child = catalog_.hit(r.child_hit);
        if (!child || child->zones.empty()) continue;
        spawn_hit_zones(n.actor_id, *child, port.fighter().position(), now);
        // its confirmation is what makes the client draw the zone: the client spawns a hit's zones by itself (UAoCAbilityComponent::SpawnHitLingeringEffects 0x146bce6b0)
        HitMeta impact;
        impact.source_type = projectile != 0 ? content::kSourceTypeProjectile : content::kSourceTypeAbility;
        impact.source_guid = projectile != 0 ? projectile : ability.row.guid;
        impact.ability_tags = ability.row.tags;
        deliver_hit_with(n, port, *child, impact, now);
    }
    if ((hit->row.applicable_targets & 2) == 0 || (hit->mods.empty() && hit->effects.empty())) return;   // (a silence or a growl has effects and no stat mods)
    HitMeta meta;
    // a hit a projectile carries names the projectile's record as its source, as the game confirms it
    meta.source_type = projectile != 0 ? content::kSourceTypeProjectile : content::kSourceTypeAbility;
    meta.source_guid = projectile != 0 ? projectile : ability.row.guid;
    meta.ability_tags = ability.row.tags;
    deliver_hit_with(n, port, *hit, meta, now);
}

void CombatWorld::npc_effect_hit(uint64_t npc, uint64_t player_actor, int64_t guid, int64_t effect_guid, double now) {
    const auto it = npcs_.find(npc);
    PlayerPort* port = player(player_actor);
    const abilities::HitDef* hit = catalog_.hit(guid);
    if (it == npcs_.end() || it->second->dead || !port || !hit || (hit->row.applicable_targets & 2) == 0 || (hit->mods.empty() && hit->effects.empty())) return;
    HitMeta meta;
    meta.source_type = content::kSourceTypeEffect;
    meta.source_guid = effect_guid;
    deliver_hit_with(*it->second, *port, *hit, meta, now);
}

void CombatWorld::deliver_hit_with(Npc& n, PlayerPort& port, const abilities::HitDef& hit_def, const HitMeta& meta, double now) {
    const abilities::HitDef* hit = &hit_def;
    const int64_t guid = hit_def.row.guid;
    Resolved r;
    std::string why;
    if (!resolver_.resolve(n, port.fighter(), *hit, meta, r, why)) {
        spdlog::debug("[combat] {} ({}): {}", n.actor_id, n.leaf, why);
        return;
    }
    double raw_health = 0, health = 0;   // the Health this hit takes: as the data gives it, and after the damage setting
    double humbled = 1.0;   // Status_Humiliated: it hits anyone but the one who humiliated it for less
    for (const Npc::HeldEffect& h : n.effects) {
        for (const HitModRow& m : kHitMods) {
            if (m.effect == h.def->row.guid && m.kind == HitModKind::OutgoingNotAtInstigator && port.actor_id() != h.by && port.actor_id() != pet_of_owner(h.by)) humbled *= m.mult;
        }
    }
    for (ResolvedMod& m : r.mods) {
        if (m.stat_guid != data_.ids().health || !(m.value < 0) || !std::isfinite(m.value)) continue;
        raw_health += m.value;
        m.value *= cfg_.npc_damage_mult * humbled;
        health += m.value;
    }
    if (!r.miss() && npc_hits_logged_ < kLoggedNpcHits) {
        // what the hit was made of: the two levels and the target's mitigation (its gear and level), so a fight can be read from the log
        ++npc_hits_logged_;
        const stats::CharacterStats& target_stats = port.fighter().stats();
        const stats::StatValue* physical = target_stats.find(data_.profile().physical.mitigation);
        const stats::StatValue* magical = target_stats.find(data_.profile().magical.mitigation);
        spdlog::info("[combat] {} (level {}) hit the player (level {}): Health {:.2f}, {:.2f} before the damage setting; the player's PhysicalMitigation {:.4f}, MagicalMitigation {:.4f}{}", n.leaf,
                     n.lvl, port.fighter().level(), health, raw_health, physical ? physical->value : 0.0, magical ? magical->value : 0.0,
                     (r.flags & stats::kHitCrit) ? ", a critical hit" : "");
    }
    IncomingHit in;
    in.instigator = n.actor_id;
    in.hit_guid = guid;
    in.source_type = meta.source_type;
    in.source_guid = meta.source_guid;
    in.flags = r.flags;
    in.mods = r.mods;
    if (!r.miss()) resolver_.effects_of(n, port.fighter(), *hit, meta, in.effects);
    in.at = now;
    r.miss() ? ++stats_.npc_misses : ++stats_.npc_hits;
    port.receive_hit(in, now);
    player_exchange(port.actor_id(), now);
}

void CombatWorld::resolve_pending(double now) {
    std::vector<Pending> keep;   // attacks with their own animation that still have a hit or their end to come
    for (size_t i = 0; i < pending_.size();) {
        // (an attack that is not due yet is passed over without a copy: this runs every tick for every attack under way; one of a creature that died or was reset goes, due or not)
        if (pending_[i].due > now) {
            bool gone = false;
            if (pending_[i].animated) {
                const auto owner = npcs_.find(pending_[i].npc);
                gone = owner == npcs_.end() || owner->second->dead;
            }
            if (!gone) {
                ++i;
                continue;
            }
        }
        Pending p = pending_[i];
        if (p.animated) {
            const auto it = npcs_.find(p.npc);
            if (it == npcs_.end() || it->second->dead) {   // (a death or a reset ended the animation already)
                pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (p.due > now) {
                ++i;
                continue;
            }
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            Npc& n = *it->second;
            PlayerPort* port = player(p.target);
            while (p.next_hit < p.timed_hits.size() && p.timed_hits[p.next_hit].first <= now) {
                const size_t at_hit = p.next_hit++;
                const int64_t guid = p.timed_hits[at_hit].second;
                const BiteArea& area = p.timed_areas[at_hit];
                if (const abilities::HitDef* helps = catalog_.hit(guid); helps && (helps->row.applicable_targets & 2) == 0) {
                    land_on_friends(n.actor_id, *helps, now);
                    continue;
                }
                // a hit lands by its targeting record when its notify falls due (where the creature is by then, facing as it faces): on the player the record's target part finds, wherever he stands
                // unless the record measures, and on every player inside one of its volumes; a hit that reaches nobody sends nothing
                const uint64_t aimed = area.has_target_part() ? aimed_at(n, area, p.target) : 0;
                const std::vector<PlayerPort*> candidates = players_;
                for (PlayerPort* q : candidates) {
                    if (!q->fighter().alive() || !hit_reaches(n, q->fighter(), area, aimed != 0 && q->actor_id() == aimed)) continue;
                    deliver_hit(n, *q, *p.ability, guid, now);
                }
            }
            while (p.next_fire < p.timed_fires.size() && p.timed_fires[p.next_fire].first <= now) {
                const PlannedFire fire = p.timed_fires[p.next_fire++].second;
                launch(n, p.target, *p.ability, fire, now);
            }
            if (p.next_hit >= p.timed_hits.size() && p.next_fire >= p.timed_fires.size() && now >= p.end_at) {
                if (p.logged && port) {   // where the lunge left the creature, as the server has it (what the client draws is for the client's log)
                    const world::WorldActor* w = registry_.find(n.actor_id);
                    const Vec3 at = port->fighter().position();
                    if (w) {
                        spdlog::info("[combat] {} ({}) {} is over: {:.0f} cm between its centre and the player's, the player {:.0f} deg off its facing", n.actor_id, n.leaf, p.ability->row.name,
                                     std::hypot(at.x - w->x, at.y - w->y), angle_between_degrees(std::atan2(at.y - w->y, at.x - w->x) * 180.0 / 3.14159265358979323846, w->yaw_degrees));
                    }
                }
                stop_cast(n);
                continue;
            }
            p.due = p.end_at;
            if (p.next_hit < p.timed_hits.size()) p.due = std::min(p.due, p.timed_hits[p.next_hit].first);
            if (p.next_fire < p.timed_fires.size()) p.due = std::min(p.due, p.timed_fires[p.next_fire].first);
            keep.push_back(std::move(p));
            continue;
        }
        if (p.due > now) {
            ++i;
            continue;
        }
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
        const auto it = npcs_.find(p.npc);
        PlayerPort* port = player(p.target);
        if (it == npcs_.end() || it->second->dead || !port || !port->fighter().alive()) continue;
        Npc& n = *it->second;
        // the target must still be near the swing (it may have run out of range during the windup)
        const double reach = static_cast<double>(n.next.range_cm) * 1.5 + 300.0;
        const double d = distance_xy_cm(n.position(), port->fighter().position()) - n.radius_cm() - port->fighter().radius_cm();
        if (d > std::max(reach, 600.0)) continue;
        for (const int64_t guid : p.hits) {
            const abilities::HitDef* helps = catalog_.hit(guid);
            if (helps && (helps->row.applicable_targets & 2) == 0) land_on_friends(n.actor_id, *helps, now);
            else deliver_hit(n, *port, *p.ability, guid, now);
        }
    }
    for (Pending& p : keep) pending_.push_back(std::move(p));
}

// ---- projectiles ----------------------------------------------------------------------------------------------------------------------

void CombatWorld::launch(Npc& n, uint64_t target, const abilities::AbilityDef& ability, const PlannedFire& fire, double now) {
    const content::ProjectileRow* record = catalog_.projectile(fire.projectile);
    const world::WorldActor* w = registry_.find(n.actor_id);
    PlayerPort* port = player(target);
    if (!record || !w || !port || !port->fighter().alive() || !(record->speed > 0)) return;
    Flight f;
    f.npc = n.actor_id;
    f.target = target;
    f.ability = &ability;
    f.projectile = fire.projectile;
    f.hits = fire.hits;
    // A projectile whose own hit hurts nobody only starts a ground zone (the oozing bomb of the goblin shamans).  The world has no ground zones of a creature yet, so the damage the target
    // takes where the bomb lands is the zone's first hit, which the ability lists among its own hits.
    const auto hurts = [&](int64_t guid) {
        const abilities::HitDef* h = catalog_.hit(guid);
        return h && (h->row.applicable_targets & 2) != 0 && !h->mods.empty();
    };
    if (std::none_of(f.hits.begin(), f.hits.end(), hurts)) {
        for (const content::AbilityHitRow& r : ability.hits) {
            if (r.source == "hit" && hurts(r.hit_guid) && std::find(f.hits.begin(), f.hits.end(), r.hit_guid) == f.hits.end()) f.hits.push_back(r.hit_guid);
        }
    }
    f.speed = record->speed;
    f.radius = std::max(0.0, record->sphere_radius);
    f.range = record->range > 0 ? record->range : 1e9;
    f.life = std::max(0.0, record->life_time);
    f.homing = record->projectile_type.find("Homing") != std::string::npos;
    f.stops_at_aim = record->projectile_type.find("ToTarget") != std::string::npos;
    f.hits_left = std::max(1, record->max_hits);
    f.at = {w->x, w->y, w->z};
    const Vec3 aim = port->fighter().position();
    f.aim_distance = distance_cm(f.at, aim);
    if (f.aim_distance > 1e-6) {
        f.dir = {(aim.x - f.at.x) / f.aim_distance, (aim.y - f.at.y) / f.aim_distance, (aim.z - f.at.z) / f.aim_distance};
    } else {
        const double yaw = w->yaw_degrees * 3.14159265358979323846 / 180.0;
        f.dir = {std::cos(yaw), std::sin(yaw), 0.0};
    }
    f.launched = f.last = now;
    flights_.push_back(std::move(f));
}

void CombatWorld::advance_flights(double now) {
    struct Landing {
        Flight   flight;
        uint64_t player = 0;
    };
    std::vector<Landing> landings;
    for (size_t i = 0; i < flights_.size();) {
        Flight& f = flights_[i];
        const double dt = now - f.last;
        if (dt <= 0) {
            ++i;
            continue;
        }
        f.last = now;
        bool gone = npcs_.find(f.npc) == npcs_.end();   // (its creature was forgotten)
        const double step = f.speed * dt;
        if (!gone && f.homing) {
            PlayerPort* port = player(f.target);
            if (!port || !port->fighter().alive()) {
                gone = true;   // nobody is left to reach
            } else {
                const Vec3 at = port->fighter().position();
                const double dx = at.x - f.at.x, dy = at.y - f.at.y, dz = at.z - f.at.z;
                const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                const double reach = f.radius + port->fighter().radius_cm();
                if (dist - reach <= step) {
                    landings.push_back({f, f.target});
                    gone = true;
                } else {
                    f.at.x += dx / dist * step;
                    f.at.y += dy / dist * step;
                    f.at.z += dz / dist * step;
                    f.travelled += step;
                }
            }
        } else if (!gone) {
            // a straight one: the line it was launched on, and whoever's body the piece flown in this tick touches
            const Vec3 from = f.at;
            const double length = f.stops_at_aim ? std::min(step, std::max(0.0, f.aim_distance + 200.0 - f.travelled)) : step;
            f.at = {from.x + f.dir.x * length, from.y + f.dir.y * length, from.z + f.dir.z * length};
            f.travelled += length;
            for (PlayerPort* q : players_) {
                if (!q->fighter().alive() || f.struck.count(q->actor_id()) || f.hits_left <= 0) continue;
                const Vec3 p = q->fighter().position();
                const double along = std::clamp((p.x - from.x) * f.dir.x + (p.y - from.y) * f.dir.y + (p.z - from.z) * f.dir.z, 0.0, length);
                const Vec3 closest{from.x + f.dir.x * along, from.y + f.dir.y * along, from.z + f.dir.z * along};
                if (distance_cm(closest, p) > f.radius + q->fighter().radius_cm()) continue;
                landings.push_back({f, q->actor_id()});
                f.struck.insert(q->actor_id());
                --f.hits_left;
            }
            if (f.hits_left <= 0 || (f.stops_at_aim && f.travelled >= f.aim_distance + 200.0)) gone = true;
        }
        if (!gone && ((f.life > 0 && now - f.launched > f.life) || f.travelled > f.range)) gone = true;
        if (gone) flights_.erase(flights_.begin() + static_cast<std::ptrdiff_t>(i));
        else ++i;
    }
    // what arrived lands after the flights are sorted out, so a hit that reaches into the world cannot disturb the list
    for (const Landing& l : landings) {
        const auto it = npcs_.find(l.flight.npc);
        PlayerPort* port = player(l.player);
        if (it == npcs_.end() || !port || !port->fighter().alive()) continue;
        if (projectiles_logged_ < kLoggedProjectiles) {
            ++projectiles_logged_;
            spdlog::info("[combat] {} ({}) projectile of {} reached player {} after {:.2f} s and {:.1f} m", it->second->actor_id, it->second->leaf, l.flight.ability->row.name, l.player, now - l.flight.launched,
                         l.flight.travelled / 100.0);
        }
        for (const int64_t hit : l.flight.hits) deliver_hit(*it->second, *port, *l.flight.ability, hit, now, l.flight.projectile);
    }
}

// ---- hits from a player -------------------------------------------------------------------------------------------------------------
// -- C.Y.B & CoA --

Delivery CombatWorld::apply_hit_to_npc(uint64_t npc_actor, uint64_t attacker, const Resolved& r, double now, uint64_t threat_to, const HitOrigin* origin) {
    Delivery d;
    d.target_object = hit_component_of(npc_actor);
    Npc* n = combatant(npc_actor);
    if (!n || n->dead) return d;
    d.applied = true;
    d.flags = r.flags;
    d.effective.assign(r.mods.size(), 0.0);
    n->attackers.insert(attacker);
    ++stats_.player_hits;
    const double health_before = n->health();
    const double taken_mult = incoming_mult(*n, origin, attacker);
    double burn_before = 0, burn_dur_before = 0;
    bool burn_moved = false;
    if (!r.miss()) {
        const CombatStatIds& id = data_.ids();
        if (const stats::StatValue* v = id.status_burning ? n->cs->find(id.status_burning) : nullptr) burn_before = v->value;
        if (const stats::StatValue* v = id.status_burning_dur ? n->cs->find(id.status_burning_dur) : nullptr) burn_dur_before = v->value;
        for (size_t i = 0; i < r.mods.size(); ++i) {
            const ResolvedMod& m = r.mods[i];
            if (!std::isfinite(m.value)) continue;
            if (id.is_status_stat(m.stat_guid)) {
                // a status stat the creature holds: the mod moves it between its limits (a stat with none stays above 0); the clients are not told (they see the effects that read it)
                const stats::StatValue* cur = n->cs->find(m.stat_guid);
                if (!cur || !cur->held()) continue;
                const double before = cur->value;
                double next = before + m.value;
                if (cur->has_max) next = std::min(next, static_cast<double>(cur->max));
                next = std::max(next, cur->has_min ? static_cast<double>(cur->min) : 0.0);
                d.effective[i] = static_cast<float>(next) - before;
                n->cs->set_current(m.stat_guid, static_cast<float>(next));
                burn_moved = true;
                continue;
            }
            if (m.stat_guid != id.health && m.stat_guid != id.mana) continue;
            const stats::StatValue* cur = n->cs->find(m.stat_guid);
            const stats::StatValue* max = n->cs->find(m.stat_guid == id.health ? id.max_health : id.max_mana);
            if (!cur || !max) continue;
            const double before = cur->value;
            double value = m.value;
            if (m.stat_guid == id.health && value < 0) value = -n->absorb(-value * taken_mult, now);   // what its effects add to the damage, and the shields take it first
            const double after = std::clamp(before + value, 0.0, static_cast<double>(max->value));
            d.effective[i] = after - before;
            n->cs->set_current(m.stat_guid, static_cast<float>(after));
            write_stat(registry_, npc_actor, m.stat_guid, static_cast<float>(after));
        }
    }
    const bool lethal = n->health() <= 0.0;
    // what reacts to this hit is read now: a death takes the effects off
    HolderEvent event;
    event.attacker = attacker;
    event.origin = origin;
    event.flags = lethal ? static_cast<uint8_t>(r.flags | stats::kHitLethal) : r.flags;
    event.lethal = lethal;
    for (size_t i = 0; i < r.mods.size(); ++i) {
        event.mods[r.mods[i].stat_guid] += r.mods[i].value;
        event.effective[r.mods[i].stat_guid] += d.effective[i];
    }
    for (const Npc::HeldEffect& h : n->effects) event.held.emplace_back(h.def, h.by);
    if (!lethal && !n->effects.empty()) {
        double loss = 0;
        for (size_t i = 0; i < d.effective.size(); ++i) {
            if (r.mods[i].stat_guid == data_.ids().health && d.effective[i] < 0) loss -= d.effective[i];
        }
        if (loss > 0) break_fragile(*n, loss, origin, now);
    }
    if (!lethal && !r.miss() && origin && origin->hit) modify_effects(*n, *origin->hit, attacker, now);
    // the owner's hand on its own pet (Extract, Reconstruct): no fight comes of it, and a pet that runs out of Health is gone, for its session to see
    if (is_pet(npc_actor)) {
        n->attackers.erase(attacker);
        if (lethal) {
            n->dead = true;
            d.lethal = true;
            d.flags |= stats::kHitLethal;
            pet_stop(npc_actor);
        }
        return d;
    }
    // the threat: what the hit says, else what it took
    double threat = r.threat;
    if (threat <= 0 && !r.miss()) {
        for (size_t i = 0; i < d.effective.size(); ++i) {
            if (d.effective[i] < 0 && !data_.ids().is_status_stat(r.mods[i].stat_guid)) threat += -d.effective[i];
        }
    }
    if (burn_moved) {
        const CombatStatIds& id = data_.ids();
        const stats::StatValue* pool = id.status_burning ? n->cs->find(id.status_burning) : nullptr;
        const stats::StatValue* dur = id.status_burning_dur ? n->cs->find(id.status_burning_dur) : nullptr;
        if (pool && dur && (pool->value != burn_before || dur->value != burn_dur_before)) {
            spdlog::info("[combat] {} ({}) is burning: {:.1f} fire damage to burn over {:.1f} s (it was {:.1f} over {:.1f} s)", n->leaf, npc_actor, pool->value, dur->value, burn_before, burn_dur_before);
        }
    }
    spdlog::log(origin && origin->source_type == content::kSourceTypeEffect ? spdlog::level::debug : spdlog::level::info, "[combat] player {} hit {} ({}, level {}): {}Health {:.1f} -> {:.1f}, threat {:.1f}", attacker, n->leaf, npc_actor, n->lvl, r.miss() ? "a miss, " : "", health_before, n->health(),
                 std::max(threat, 1.0));
    if (!lethal) {
        n->ai.notice(threat_to != 0 ? threat_to : attacker, std::max(threat, 1.0), now);
        if (threat_to != 0) n->ai.notice(attacker, 0.01, now);
    }
    player_exchange(attacker, now);
    if (lethal) {
        d.lethal = true;
        d.flags |= stats::kHitLethal;
        kill(*n, attacker, now);
    } else {
        think(*n, now);   // the fight starts in this step: the flags go up with the first exchange
    }
    holder_reacts(npc_actor, event, now);
    if (threat_to != 0 && is_pet(threat_to)) {
        pet_hit_reacts(threat_to, event, now);
    } else if (origin && origin->landed_here && origin->hit && !origin->hit->reactions.empty()) {
        // what a hit that is not the session's (an effect's tick: Spirit Shred's damage) carries on its own record; the session runs the reactions of its own hits
        HolderEvent own = event;
        own.held.clear();
        own.lethal = false;
        holder_reacts(npc_actor, own, now, 2 | 4, &origin->hit->reactions);
    }
    return d;
}

/// A pet as the creatures see it: a body on their tables that takes their hits.  One port per pet, in players_ while the pet is in the world.
class CombatWorld::PetPort final : public PlayerPort {
public:
    PetPort(CombatWorld& w, Npc& pet) : world_(w), pet_(pet) {}
    uint64_t actor_id() const override { return pet_.actor_id; }
    Fighter& fighter() override { return pet_; }
    void receive_hit(const IncomingHit& hit, double now) override {
        if (pet_.dead || (hit.flags & stats::kHitMiss) != 0) return;
        const CombatStatIds& id = world_.data_.ids();
        double taken = 0;
        for (const ResolvedMod& m : hit.mods) {
            if (m.stat_guid != id.health || !std::isfinite(m.value)) continue;
            const stats::StatValue* cur = pet_.cs->find(id.health);
            const stats::StatValue* max = pet_.cs->find(id.max_health);
            if (!cur || !max) continue;
            double value = m.value;
            if (value < 0) value = -pet_.absorb(-value, now);
            const double after = std::clamp(static_cast<double>(cur->value) + value, 0.0, static_cast<double>(max->value));
            taken += std::max(0.0, static_cast<double>(cur->value) - after);
            pet_.cs->set_current(id.health, static_cast<float>(after));
            write_stat(world_.registry_, pet_.actor_id, id.health, static_cast<float>(after));
        }
        world_.pet_was_hit(pet_, hit, taken, now);
        if (pet_.health() <= 0.0) {   // (no corpse and no reward: its session sees dead() and takes it away)
            pet_.dead = true;
            world_.pet_stop(pet_.actor_id);
        }
    }
    void set_in_combat(bool, double) override {}
    protocol::emit::FIntrepidNetworkGUID pawn_guid() const override {
        const world::WorldActor* a = world_.registry_.find(pet_.actor_id);
        return a ? a->actor.guid : protocol::emit::FIntrepidNetworkGUID{};
    }
    protocol::emit::FIntrepidNetworkGUID cylinder_guid() const override {
        protocol::emit::FIntrepidNetworkGUID g = pawn_guid();
        if (const uint64_t cylinder = world_.hit_component_of(pet_.actor_id)) g.ObjectId = cylinder;
        return g;
    }

private:
    CombatWorld& world_;
    Npc&         pet_;
};

uint64_t CombatWorld::attacker_of(uint64_t player_actor, uint64_t pet, double radius_cm, bool owner_too) const {
    const auto port = std::find_if(players_.begin(), players_.end(), [&](const PlayerPort* p) { return p->actor_id() == player_actor; });
    if (port == players_.end()) return 0;
    const Vec3 at = (*port)->fighter().position();
    uint64_t best = 0;
    double nearest = radius_cm;
    for (const auto& [id, n] : npcs_) {
        if (n->dead || id == pet || !n->flags_in_combat || (!(owner_too && n->flags_target == player_actor) && n->flags_target != pet)) continue;
        const double d = distance_xy_cm(n->position(), at);
        if (d <= nearest) {
            nearest = d;
            best = id;
        }
    }
    return best;
}

uint64_t CombatWorld::attacker_of_owner(uint64_t player_actor, uint64_t pet, double radius_cm) const {
    const auto port = std::find_if(players_.begin(), players_.end(), [&](const PlayerPort* p) { return p->actor_id() == player_actor; });
    if (port == players_.end()) return 0;
    const Vec3 at = (*port)->fighter().position();
    uint64_t best = 0;
    double nearest = radius_cm;
    for (const auto& [id, n] : npcs_) {
        if (n->dead || id == pet || is_pet(id) || n->ai.target() != player_actor) continue;
        const double d = distance_xy_cm(n->position(), at);
        if (d <= nearest) {
            nearest = d;
            best = id;
        }
    }
    return best;
}

bool CombatWorld::pet_can_act(uint64_t pet) const {
    const auto it = npcs_.find(pet);
    return it != npcs_.end() && !it->second->dead && !((it->second->crowd & kCrowdStopsActing) != 0 && it->second->crowd_stops == CrowdStops::All);
}

bool CombatWorld::fought_by(uint64_t npc_actor, uint64_t player_actor) const {
    const auto it = npcs_.find(npc_actor);
    return it != npcs_.end() && !it->second->dead && it->second->attackers.count(player_actor) != 0;
}

// ---- what a player's hits leave on a creature ----------------------------------------------------------------------------------------

/// Where a hit a player's pet, effect or reaction lands comes from.
struct CombatWorld::LandSource {
    int64_t                      type = 0, guid = 0;
    const abilities::AbilityDef* ability = nullptr;
    uint64_t                     pet = 0;                // the pet that lands it: the creature's anger is its
    const HolderEvent*           event = nullptr;        // the hit a reaction answers
    bool                         children = true;        // it fires the hits it lists
    bool                         skip_requirement = false;
    bool                         predicted = false;      // the client predicts it (an effect's Tick hit)
    int                          stacks = 1;             // the stacks its numbers count for (the tick of an effect that recalculates its hits: every stack it holds)
};

namespace {

repl::Subobject* stats_of(world::WorldActor& a) {
    for (repl::Subobject& s : a.actor.subobjects) {
        if (s.obj.schema && std::string(s.obj.schema->name) == "UAoCStatsComponent") return &s;
    }
    return nullptr;
}

bootstrap::EffectItem effect_item_of(int64_t key, const abilities::EffectDef& def, double start, double until, int stacks) {
    bootstrap::EffectItem item;
    item.effect_id = key;
    item.effect_guid = def.row.guid;
    item.start_time = bootstrap::effect_clock(start);
    item.duration = until < 0 ? std::numeric_limits<float>::max() : static_cast<float>(until - start);
    item.tick_time = static_cast<float>(def.row.tick_timer);
    item.stacks = stacks;
    return item;
}

}  // namespace

void CombatWorld::apply_effect_to_npc(uint64_t npc_actor, const LandedEffect& e, uint64_t by, double now) {
    Npc* n = combatant(npc_actor);
    if (!n || n->dead || !e.def) return;
    const content::EffectRow& row = e.def->row;
    const bool in_place = row.stack_type.find("StackInPlace") != std::string::npos;
    const bool separate = row.stack_type.find("StackSeperate") != std::string::npos;
    // the instance this application is for: the first of the record, or (a record that compares sources: a bleed for each attacker) the first of the same instigator
    Npc::HeldEffect* held = nullptr;
    int same = 0;
    for (Npc::HeldEffect& h : n->effects) {
        if (h.def != e.def) continue;
        ++same;
        if (!held && (!row.compare_source || h.by == by)) held = &h;
    }
    // ... and on the holder already from other instigators: one more instance only up to the record's global limit (0: no limit)
    if (!held && row.compare_source && same > 0 && row.global_stack_limit > 0 && same >= row.global_stack_limit) return;
    // STACK_OverrideAlways: a new application replaces it, as a new instance whose Apply hits run again (a burn's second hit ticks at once)
    if (held && row.stack_type.find("OverrideAlways") != std::string::npos) {
        if (row.stack_duration_refresh.find("NeverRefresh") != std::string::npos) return;   // (a record that never refreshes keeps what it has)
        remove_effect_at(*n, static_cast<size_t>(held - n->effects.data()));
        held = nullptr;
        same = 0;
    }
    const PlayerPort* port = player(by);
    const std::array<uint32_t, 4> who = port ? port->character_guid() : std::array<uint32_t, 4>{};
    const double until = e.seconds < 0 ? -1.0 : now + e.seconds;
    if (held && !(separate && same < e.stack_limit)) {
        if (separate) return;   // it holds as many as the record allows
        held->stacks = in_place ? std::min(e.stack_limit, held->stacks + e.stacks) : 1;
        held->mods = e.mods;
        if (row.stack_duration_refresh.find("ExtendDuration") != std::string::npos && e.extension >= 0 && held->until >= 0 && until >= 0) {
            // ExtendDuration: the time left grows by the amount the application worked out, up to the longest the record allows (what is left already is never cut)
            double end = held->until + e.extension;
            if (e.extended_max > 0) end = std::min(end, now + e.extended_max);
            held->until = std::max(end, held->until);
        } else {
            held->until = until;
        }
        held->start = now;
        held->by = by;
        held->taken = 0;
        const bootstrap::EffectItem item = effect_item_of(held->key, *e.def, now, until, held->stacks);
        const int32_t array_id = held->array_id;
        registry_.modify(npc_actor, world::Change::State, [&](world::WorldActor& a) {
            repl::Subobject* s = stats_of(a);
            repl::FastArrayState* arr = s ? s->obj.arrays.get(*s->obj.schema, "CurrentEffects") : nullptr;
            if (arr) arr->change(array_id, bootstrap::effect_item_state(who, item));
        });
    } else {
        Npc::HeldEffect h;
        h.def = e.def;
        h.stacks = in_place ? std::min(e.stack_limit, e.stacks) : 1;
        h.start = now;
        h.until = until;
        // the first tick as it lands, unless the record skips it (as the player's own effects: ability_pipeline.cpp); a bleed's first tick is a second later, a hemorrhage's at once
        h.next_tick = row.tick_timer > 0 ? std::max(row.ignore_initial_tick ? now + row.tick_timer : now, 1e-9) : 0;
        h.mods = e.mods;
        h.by = by;
        h.key = next_effect_key_++;
        const bootstrap::EffectItem item = effect_item_of(h.key, *e.def, now, until, h.stacks);
        registry_.modify(npc_actor, world::Change::State, [&](world::WorldActor& a) {
            repl::Subobject* s = stats_of(a);
            repl::FastArrayState* arr = s ? s->obj.arrays.get(*s->obj.schema, "CurrentEffects") : nullptr;
            if (!arr) return;
            if (const auto id = arr->add(bootstrap::effect_item_state(who, item))) h.array_id = *id;
        });
        n->effects.push_back(std::move(h));
        effected_.insert(npc_actor);
        effects_changed(*n, now);
        // a knockback throws it away from the one who hit it
        const CrowdControlRow* crowd = crowd_control(row.crowd_control_guid);
        if (crowd && crowd->push_cm > 0 && npcs_world_) {
            const world::WorldActor* at = registry_.find(npc_actor);
            PlayerPort* from = player(by);
            if (at && from) {
                const Vec3 origin = from->fighter().position();
                const double dx = at->x - origin.x, dy = at->y - origin.y, length = std::hypot(dx, dy);
                if (length > 1.0) npcs_world_->order_move(npc_actor, nav::Vec3{at->x + dx / length * crowd->push_cm, at->y + dy / length * crowd->push_cm, at->z}, 2000.0, 0.0);
            }
        }
        run_phases(npc_actor, *e.def, by, {"Apply"}, now);
        return;
    }
    effected_.insert(npc_actor);
    effects_changed(*n, now);
}

// What a reaction's expressions and hits ask of the hit in progress.
class CombatWorld::EventHost final : public ContextHost {
public:
    EventHost(stats::SplitMix& rng, const HolderEvent& event, uint64_t instigator) : rng_(rng), event_(event), instigator_(instigator) {}
    double rand_double() override { return rng_.rand_double(); }
    double effect_count_by_instigator(const stats::RecordRef& r) override {
        double count = 0;
        for (const auto& [def, by] : event_.held) count += def->row.guid == r.guid && by == instigator_ ? 1.0 : 0.0;
        return count;
    }

protected:
    double other_fn(int fn, const std::string& name, const double* args, int) override {
        switch (fn) {
            case stats::kHostHitStatMod:
            case stats::kHostHitStatEffective: {
                const std::map<int64_t, double>& m = fn == stats::kHostHitStatMod ? event_.mods : event_.effective;
                const auto it = m.find(std::strtoll(name.c_str(), nullptr, 10));
                return it == m.end() ? 0.0 : it->second;
            }
            case stats::kHostHitRecordIs: return event_.origin && event_.origin->hit && event_.origin->hit->row.guid == std::strtoll(name.c_str(), nullptr, 10) ? 1.0 : 0.0;
            case stats::kHostHitRecordNameIs: return event_.origin && event_.origin->hit && event_.origin->hit->row.name == name ? 1.0 : 0.0;
            case stats::kHostRandIntRange: {
                const double lo = std::floor(args[0]), hi = std::floor(args[1]);
                return hi < lo ? lo : lo + std::floor(rng_.rand_double() * (hi - lo + 1.0));
            }
            default: return 0.0;
        }
    }

private:
    stats::SplitMix&   rng_;
    const HolderEvent& event_;
    uint64_t           instigator_;
};

double CombatWorld::incoming_mult(const Npc& n, const HitOrigin* origin, uint64_t attacker) const {
    double mult = 1.0;
    for (const Npc::HeldEffect& h : n.effects) {
        for (const HitModRow& m : kHitMods) {
            if (m.effect != h.def->row.guid) continue;
            if (m.kind == HitModKind::IncomingFromEffects && origin && origin->source_type == content::kSourceTypeEffect) mult *= m.mult;
            if (m.kind == HitModKind::IncomingFromInstigator && attacker == h.by) mult *= m.mult;
        }
    }
    return mult;
}

void CombatWorld::modify_effects(Npc& n, const abilities::HitDef& hit, uint64_t attacker, double now) {
    PlayerPort* port = player(attacker);
    bool changed = false;
    for (const ModifyRow& row : kModifyEffects) {
        if (row.hit != hit.row.guid) continue;
        if (row.talent != 0) {
            bool has = false;
            if (port) {
                for (const stats::SkillRank& rank : port->fighter().stats().ranks()) has = has || rank.skill.guid == row.talent;
            }
            if (!has) continue;
        }
        for (size_t i = 0; i < n.effects.size(); ++i) {
            Npc::HeldEffect& h = n.effects[i];
            if (h.def->row.guid != row.effect || h.until < 0 || h.by != attacker) continue;
            const double remaining = h.until - now;
            double delta = row.seconds;
            if (row.kind == ModifyKind::Elapsed) delta = now - h.start;
            if (row.kind == ModifyKind::AtLeastOne) delta = std::max(0.0, 1.0 - remaining);
            h.until = std::max(now, h.until + delta);
            rewrite_effect_item(n, i);
            changed = true;
        }
    }
    if (changed) effects_changed(n, now);
}

void CombatWorld::rewrite_effect_item(Npc& n, size_t index) {
    const Npc::HeldEffect& h = n.effects[index];
    const PlayerPort* port = player(h.by);
    const std::array<uint32_t, 4> who = port ? port->character_guid() : std::array<uint32_t, 4>{};
    const bootstrap::EffectItem item = effect_item_of(h.key, *h.def, h.start, h.until, h.stacks);
    const int32_t array_id = h.array_id;
    registry_.modify(n.actor_id, world::Change::State, [&](world::WorldActor& a) {
        repl::Subobject* s = stats_of(a);
        repl::FastArrayState* arr = s ? s->obj.arrays.get(*s->obj.schema, "CurrentEffects") : nullptr;
        if (arr) arr->change(array_id, bootstrap::effect_item_state(who, item));
    });
}

uint64_t CombatWorld::pet_of_owner(uint64_t owner) const {
    for (const auto& [pet, who] : pet_owners_) {
        if (who == owner) return pet;
    }
    return 0;
}

uint64_t CombatWorld::owner_of_pet(uint64_t pet) const {
    const auto it = pet_owners_.find(pet);
    return it == pet_owners_.end() ? 0 : it->second;
}

double CombatWorld::flight_seconds(const content::AbilityHitRow& row, const Vec3& from, const Vec3& to) const {
    const content::ProjectileRow* flying = catalog_.projectile(row.owner_guid);
    return flying && flying->speed > 0 ? distance_cm(from, to) / flying->speed : 0.3;
}

void CombatWorld::land_for_player(uint64_t by, uint64_t target, const abilities::HitDef& hit, const LandSource& src, double now, int depth) {
    PlayerPort* port = player(by);
    const auto found = npcs_.find(target);
    if (!port || depth > 4 || found == npcs_.end() || found->second->dead) return;
    Npc& n = *found->second;
    HitMeta meta;
    meta.source_type = src.type;
    meta.source_guid = src.guid;
    if (src.ability) meta.ability_tags = src.ability->row.tags;
    class HeldView final : public TargetEffects {
    public:
        HeldView(const CombatWorld& world, uint64_t holder, double at) : world_(world), holder_(holder), at_(at) {}
        double remaining(int64_t effect_guid) const override { return world_.effect_remaining(holder_, effect_guid, at_); }

    private:
        const CombatWorld& world_;
        uint64_t           holder_;
        double             at_;
    };
    const HeldView held_view(*this, target, now);
    meta.target_effects = &held_view;
    if (!src.skip_requirement && !resolver_.requirement_holds(port->fighter(), n, hit, meta)) return;
    Resolved r;
    std::string why;
    bool ok = false;
    if (src.event) {   // a reaction's hit reads the hit it answers (GetStatModValue(GetHitEvent(), ...))
        std::string err;
        auto a = port->fighter().stats().evaluation(port->fighter().worn(), err, true);
        auto t = n.stats().evaluation(n.worn(), err, true);
        if (a && t) {
            EventHost host(rng_, *src.event, by);
            const Relation rel = data_.relation(port->fighter().alignments(), n.alignments());
            ok = resolver_.resolve_with(*a, host, t.get(), distance_cm(port->fighter().position(), n.position()), rel == Relation::Hostile, rel == Relation::Neutral, hit, meta, r, why);
        }
    } else {
        ok = resolver_.resolve(port->fighter(), n, hit, meta, r, why);
    }
    if (!ok) {
        spdlog::debug("[combat] the hit {} does not run on {}: {}", hit.row.name, target, why);
        return;
    }
    // a pet's numbers are its owner's, times what its own effects add (Amplify)
    if (src.pet != 0) {
        double damage = 1.0, healing = 1.0;
        if (const auto pet = npcs_.find(src.pet); pet != npcs_.end()) {
            for (const Npc::HeldEffect& h : pet->second->effects) {
                for (const stats::EffectMod& m : h.mods) {
                    if (m.stat == kStatDamageMult) damage *= m.value;
                    if (m.stat == kStatHealingMult) healing *= m.value;
                }
            }
        }
        for (ResolvedMod& m : r.mods) {
            if (m.stat_guid == data_.ids().health) m.value *= m.value < 0 ? damage : healing;
        }
    }
    // the tick of an effect that recalculates its hits is worked out for one stack and counts for all the stacks it holds now: a poison of five stacks takes five times what one
    // stack takes [M: how a poison's ticks grow with its stacks is not known; each tick counts every stack]
    if (src.stacks > 1 && !r.miss()) {
        for (ResolvedMod& m : r.mods) m.value *= static_cast<double>(src.stacks);
        r.threat *= static_cast<double>(src.stacks);
    }
    HitOrigin origin;
    origin.landed_here = true;
    origin.hit = &hit;
    origin.source_type = src.type;
    origin.source_guid = src.guid;
    origin.ability_tags = meta.ability_tags;
    const Delivery d = apply_hit_to_npc(target, by, r, now, target == src.pet ? 0 : src.pet, &origin);
    if (d.applied) {
        // what the client draws is Health and Mana: a status stat's move (the burn's pool and seconds) is not shown
        DealtHit dealt{hit.row.guid, meta.source_type, meta.source_guid, src.pet, target, d.flags, {}, {}, src.predicted};
        for (size_t i = 0; i < r.mods.size(); ++i) {
            if (data_.ids().is_status_stat(r.mods[i].stat_guid)) continue;
            dealt.mods.push_back(r.mods[i]);
            dealt.effective.push_back(d.effective[i]);
        }
        if (!dealt.mods.empty() || hit.row.has_fx || r.miss()) port->hit_dealt(dealt, now);
    }
    if (d.applied && !r.miss() && !d.lethal) {
        if (hit.max_remove > 0 || !hit.remove_effects.empty()) remove_effects_from_npc(target, hit, now);
        std::vector<LandedEffect> landed;
        resolver_.effects_of(port->fighter(), n, hit, meta, landed);
        for (const LandedEffect& e : landed) {
            if (e.shield > 0) add_shield(target, e.shield, std::max(0.0, e.seconds), now);
            apply_effect_to_npc(target, e, by, now);
        }
    }
    if (src.children) {
        LandSource child = src;
        child.stacks = 1;   // (what a tick's hit fires counts for itself, not for the tick's stacks)
        land_children(by, target, hit, child, now, depth);
    }
}

void CombatWorld::land_children(uint64_t by, uint64_t target, const abilities::HitDef& hit, const LandSource& src, double now, int depth) {
    PlayerPort* port = player(by);
    if (!port) return;
    const world::WorldActor* at = registry_.find(target);
    if (!at) return;
    const Vec3 centre{at->x, at->y, at->z};
    for (const content::RecursiveHitRow& row : hit.recursive) {
        const abilities::HitDef* child = catalog_.hit(row.child_hit);
        if (!child) continue;
        const int mask = child->row.applicable_targets;
        if ((mask & 2) != 0) {
            const double radius = targeting_radius(row.targeting);
            if (radius > 0) {
                for (const GatheredTarget& g : enemies_around(port->fighter(), centre, radius, 40)) {
                    if (!is_pet(g.actor)) land_for_player(by, g.actor, *child, src, now, depth + 1);
                }
            } else if (!is_pet(target) && target != by) {
                land_for_player(by, target, *child, src, now, depth + 1);
            }
        } else if ((mask & 4) != 0) {
            if (const uint64_t pet = pet_of_owner(by)) land_for_player(by, pet, *child, src, now, depth + 1);
        } else if ((mask & 1) != 0) {
            port->hit_from_pet(child->row.guid, src.guid, now);
        }
    }
}

void CombatWorld::run_phases(uint64_t holder, const abilities::EffectDef& def, uint64_t by, std::initializer_list<const char*> phases, double now, int stacks) {
    LandSource src;
    src.type = content::kSourceTypeEffect;
    src.guid = def.row.guid;
    src.predicted = phases.size() == 1 && std::string_view(*phases.begin()) == "Tick";   // an effect's Tick hit is predicted by the client; an Apply or Expire hit is not
    if (src.predicted && def.row.recalc_on_tick) src.stacks = std::max(1, stacks);
    for (const auto& [phase, hit_guid] : def.hits) {
        bool wanted = false;
        for (const char* p : phases) wanted = wanted || phase == p;
        const abilities::HitDef* hit = wanted ? catalog_.hit(hit_guid) : nullptr;
        if (!hit) continue;
        // an effect's hit lands on its holder, or where its records send it when it names nobody it could reach there
        const auto found = npcs_.find(holder);
        if (found == npcs_.end() || found->second->dead) return;
        land_for_player(by, holder, *hit, src, now);
    }
}

void CombatWorld::pet_hit_reacts(uint64_t pet, const HolderEvent& event, double now) {
    const auto it = npcs_.find(pet);
    if (it == npcs_.end() || !event.origin || it->second->dead) return;
    HolderEvent own = event;
    own.lethal = false;
    own.held.clear();
    for (const Npc::HeldEffect& h : it->second->effects) own.held.emplace_back(h.def, h.by);
    holder_reacts(pet, own, now, 4, event.origin->hit ? &event.origin->hit->reactions : nullptr);
}

void CombatWorld::holder_reacts(uint64_t holder, const HolderEvent& event, double now, int hit_bit, const std::vector<int64_t>* carried) {
    if (event.origin && event.origin->source_type == content::kSourceTypeReaction) return;   // a hit a reaction made raises no reaction [the game's code; ability_reactions.cpp]
    if (!event.origin && !event.lethal) return;
    std::vector<std::pair<const std::vector<int64_t>*, uint64_t>> lists;
    for (const auto& [def, left_by] : event.held) lists.emplace_back(&def->reactions, left_by);
    if (carried) lists.emplace_back(carried, event.attacker);
    for (const auto& [list, by] : lists) {
        for (const int64_t guid : *list) {
            const abilities::ReactionDef* reaction = catalog_.reaction(guid);
            if (!reaction) continue;
            const content::ReactionRow& row = reaction->row;
            const bool on_hit = event.origin && (row.bind_hit & hit_bit) != 0;
            const bool on_death = hit_bit == 2 && event.lethal && (row.bind_stat & 2) != 0;
            if (!on_hit && !on_death) continue;
            // who casts it: the one who left the effect or the one whose hit this is; a creature's hit on a player's effect has no fighter to run from
            const uint64_t caster = row.instigator == "HitInstigator" ? event.attacker : by;
            PlayerPort* port = player(caster);
            if (!port || is_pet(caster)) continue;
            const auto holder_it = npcs_.find(holder);
            if (holder_it == npcs_.end()) return;
            if (row.trigger_expr != 0) {
                const content::ExprRow* e = catalog_.expr(row.trigger_expr);
                if (!e || e->program < 0) {
                    if (reaction_noted_.insert(guid).second) spdlog::info("[abilities] the reaction {} on a creature stays inert: its trigger does not compile", row.name);
                    continue;
                }
                // GetEffectInstigator() reads as the caster: the trigger's comparison of the two is made here
                if (e->text.find("GetEffectInstigator") != std::string::npos && caster != by) continue;
                std::string err;
                auto a = port->fighter().stats().evaluation(port->fighter().worn(), err, true);
                auto t = holder_it->second->stats().evaluation(holder_it->second->worn(), err, true);
                if (!a || !t || !a->set_actor(stats::kRoleTarget, *t, err)) continue;
                HitContext ctx;
                ctx.profile = &data_.profile();
                if (event.origin) {
                    if (event.origin->hit) ctx.tags = event.origin->hit->tags;
                    ctx.tags.insert(ctx.tags.end(), event.origin->ability_tags.begin(), event.origin->ability_tags.end());
                    ctx.source_type = event.origin->source_type;
                }
                ctx.hit.flags = event.flags;
                EventHost host(rng_, event, caster);
                host.set_context(&ctx);
                double v = 0;
                const bool ran = a->eval(e->program, host, v, err);
                host.set_context(nullptr);
                a->clear_actors();
                if (!ran) {
                    if (reaction_noted_.insert(guid).second) spdlog::info("[abilities] the reaction {} on a creature stays inert: {}", row.name, err);
                    continue;
                }
                if (v == 0.0) continue;
            }
            LandSource src;
            src.type = content::kSourceTypeReaction;
            src.guid = guid;
            src.event = &event;
            src.skip_requirement = true;
            for (const int64_t hit_guid : row.hits) {
                const abilities::HitDef* hit = catalog_.hit(hit_guid);
                if (!hit) continue;
                if ((row.targets_mask & (1 | 8 | 16)) != 0) land_for_player(caster, holder, *hit, src, now);   // the holder, the hit's target, what the hit was for
                if ((row.targets_mask & 2) != 0) port->hit_from_pet(hit_guid, guid, now);                       // the effect's instigator
                if ((row.targets_mask & 4) != 0 && event.attacker != caster) land_for_player(caster, event.attacker, *hit, src, now);   // who struck the holder (Brace)
                if ((row.targets_mask & 8192) != 0) {   // the hit instigator's pet
                    if (const uint64_t pet = pet_of_owner(caster)) land_for_player(caster, pet, *hit, src, now);
                }
            }
        }
    }
}

void CombatWorld::pet_was_hit(Npc& pet, const IncomingHit& hit, double taken, double now) {
    if (pet.effects.empty()) return;
    const abilities::HitDef* def = catalog_.hit(hit.hit_guid);
    HitOrigin origin;
    origin.hit = def;
    origin.source_type = hit.source_type;
    origin.source_guid = hit.source_guid;
    HolderEvent event;
    event.attacker = hit.instigator;
    event.origin = &origin;
    event.flags = hit.flags;
    for (const ResolvedMod& m : hit.mods) event.mods[m.stat_guid] += m.value;
    event.effective[data_.ids().health] = -taken;
    for (const Npc::HeldEffect& h : pet.effects) event.held.emplace_back(h.def, h.by);
    if (taken > 0) break_fragile(pet, taken, &origin, now);
    holder_reacts(pet.actor_id, event, now);
}

void CombatWorld::remove_effect_at(Npc& n, size_t index) {
    const int32_t array_id = n.effects[index].array_id;
    if (array_id >= 0) {
        registry_.modify(n.actor_id, world::Change::State, [&](world::WorldActor& a) {
            repl::Subobject* s = stats_of(a);
            repl::FastArrayState* arr = s ? s->obj.arrays.get(*s->obj.schema, "CurrentEffects") : nullptr;
            if (arr) arr->remove(array_id);
        });
    }
    n.effects.erase(n.effects.begin() + static_cast<std::ptrdiff_t>(index));
}

void CombatWorld::end_effect_at(Npc& n, size_t index, std::initializer_list<const char*> phases, double now) {
    const abilities::EffectDef* def = n.effects[index].def;
    const uint64_t by = n.effects[index].by;
    remove_effect_at(n, index);
    effects_changed(n, now);
    run_phases(n.actor_id, *def, by, phases, now);   // (a tripped creature is Tripped_Recently from here)
}

void CombatWorld::clear_effects(Npc& n, double now) {
    if (n.effects.empty()) return;
    while (!n.effects.empty()) remove_effect_at(n, n.effects.size() - 1);
    effects_changed(n, now);
}

void CombatWorld::remove_effects_from_npc(uint64_t npc_actor, const abilities::HitDef& hit, double now) {
    const auto found = npcs_.find(npc_actor);
    if (found == npcs_.end() || found->second->effects.empty()) return;
    Npc& n = *found->second;
    int left = hit.max_remove > 0 ? hit.max_remove : 1 << 20;
    for (size_t i = 0; i < n.effects.size() && left > 0;) {
        if (!hit.removes(*n.effects[i].def)) {
            ++i;
            continue;
        }
        end_effect_at(n, i, {"Remove", "End"}, now);
        --left;
    }
}

uint64_t CombatWorld::creature_target(uint64_t npc_actor) const {
    const auto found = npcs_.find(npc_actor);
    return found == npcs_.end() || found->second->dead ? 0 : found->second->ai.target();
}

bool CombatWorld::holds_dispellable(uint64_t npc_actor) const {
    const auto found = npcs_.find(npc_actor);
    if (found == npcs_.end()) return false;
    for (const Npc::HeldEffect& h : found->second->effects) {
        if (h.def->row.dispellable && h.def->row.category.find("Debuff") != std::string::npos) return true;
    }
    return false;
}

void CombatWorld::remove_npc_effect(uint64_t npc_actor, int64_t effect_guid, double now) {
    const auto found = npcs_.find(npc_actor);
    if (found == npcs_.end()) return;
    Npc& n = *found->second;
    for (size_t i = 0; i < n.effects.size();) {
        if (n.effects[i].def->row.guid == effect_guid) end_effect_at(n, i, {"Remove", "End"}, now);
        else ++i;
    }
}

int CombatWorld::npc_effect_count(uint64_t npc_actor, int64_t effect_guid) const {
    const auto found = npcs_.find(npc_actor);
    if (found == npcs_.end()) return 0;
    int count = 0;
    for (const Npc::HeldEffect& h : found->second->effects) count += h.def->row.guid == effect_guid ? 1 : 0;
    return count;
}

uint8_t CombatWorld::crowd_status(uint64_t npc_actor) const {
    const auto found = npcs_.find(npc_actor);
    return found == npcs_.end() ? 0 : found->second->crowd;
}

double CombatWorld::crowd_pace(uint64_t npc_actor) const {
    const auto found = npcs_.find(npc_actor);
    return found == npcs_.end() ? 1.0 : found->second->pace;
}

void CombatWorld::effects_changed(Npc& n, double now) {
    if (n.effects.empty()) effected_.erase(n.actor_id);
    // the stat engine: the debuffs' mods count from here on, and GetEffectCount(GetTarget(), ...) sees them
    std::vector<stats::ActiveEffect> active;
    active.reserve(n.effects.size());
    uint8_t status = 0;
    CrowdStops stops = CrowdStops::None;
    double snare = 0, speed_mods = 0;
    const CrowdControlRow* shown = nullptr;
    const Npc::HeldEffect* shown_by = nullptr;
    for (const Npc::HeldEffect& h : n.effects) {
        stats::ActiveEffect a;
        a.effect = {stats::kRefTypeEffect, h.def->row.guid};
        a.stacks = h.stacks;
        a.tags = h.def->row.tags;
        a.tags.insert(a.tags.end(), h.def->row.effect_tags.begin(), h.def->row.effect_tags.end());
        a.mods = h.mods;
        for (const stats::EffectMod& m : h.mods) {
            if (m.stat == data_.ids().move_speed_mult) speed_mods += m.value * h.stacks;
        }
        active.push_back(std::move(a));
        const CrowdControlRow* row = crowd_control(h.def->row.crowd_control_guid);
        if (!row) continue;
        status = static_cast<uint8_t>(status | row->status);
        snare = std::max(snare, row->snare);
        if (row->stops == CrowdStops::All || stops == CrowdStops::None) stops = row->stops == CrowdStops::None ? stops : row->stops;
        if (!shown || row->priority > shown->priority) {
            shown = row;
            shown_by = &h;
        }
    }
    n.cs->set_effects(std::move(active));
    std::vector<stats::StatValue> changed;
    stats::CharacterStats::Next next;
    std::string err;
    if (n.cs->prepare(n.worn(), changed, next, err)) {
        n.cs->commit(std::move(next));
        write_stats(registry_, n.actor_id, *n.cs);
    }
    // its body: a crowd control that stops it stops what it was doing, and it stays on its table
    const bool stopped_now = (status & kCrowdStopsActing) != 0 && stops == CrowdStops::All && !(n.crowd_stops == CrowdStops::All);
    const bool rooted_now = (status & kCrowdStopsMoving) != 0 && (n.crowd & kCrowdStopsMoving) == 0;
    n.crowd = status;
    n.crowd_stops = stops;
    n.pace = std::clamp((1.0 - snare) * (1.0 + speed_mods), 0.05, 4.0);
    if (npcs_world_) npcs_world_->set_pace(n.actor_id, n.pace);
    if (stopped_now) {
        stop_cast(n);
        n.hold_until = 0;
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.npc == n.actor_id; }), pending_.end());
    }
    if ((stopped_now || rooted_now) && npcs_world_) npcs_world_->halt(n.actor_id);
    // what the client plays and the bar its nameplate shows [modelled: no recording shows a creature's CrowdControlState; the fields are the schema's]
    registry_.modify(n.actor_id, world::Change::State, [&](world::WorldActor& a) {
        repl::Subobject* s = stats_of(a);
        if (!s) return;
        const auto set = [&](const char* path, repl::Value v) { s->obj.state.set(*s->obj.schema, path, std::move(v)); };
        set("CrowdControlState.StatusFlags", repl::Value::u64(status));
        set("CrowdControlState.CurrentAnimationRequest", repl::Value::u64(shown ? shown->animation : kCrowdAnimationNone));
        set("CrowdControlState.CrowdControlRecordToAnimate", repl::Value::i64(shown ? shown->guid : 0));
        set("CrowdControlState.SnareValue", repl::Value::f32(static_cast<float>(snare)));
        set("CrowdControlState.SyncStartAnimTime", repl::Value::f64(shown_by ? bootstrap::effect_clock(shown_by->start) : 0.0));
        set("CrowdControlState.Duration", repl::Value::f32(shown_by && shown_by->until >= 0 ? static_cast<float>(shown_by->until - shown_by->start) : 0.0f));
        set("CrowdControlState.EndAnimTime", repl::Value::f64(shown_by && shown_by->until >= 0 ? bootstrap::effect_clock(shown_by->until) : 0.0));
    });
    (void)now;
}

void CombatWorld::break_fragile(Npc& n, double loss, const HitOrigin* origin, double now) {
    const int64_t hit = origin && origin->hit ? origin->hit->row.guid : 0;
    const bool exempt = std::find(std::begin(kInstantBreakExempt), std::end(kInstantBreakExempt), hit) != std::end(kInstantBreakExempt);
    for (size_t i = 0; i < n.effects.size();) {
        Npc::HeldEffect& h = n.effects[i];
        const FragileRow* row = fragile_effect(h.def->row.guid);
        h.taken += loss;
        if (row && row->share >= 0 && !(row->share == 0 && exempt) && h.taken >= row->share * n.max_health()) {
            end_effect_at(n, i, {"Remove", "End"}, now);
        } else {
            ++i;
        }
    }
}

void CombatWorld::step_effects(double now) {
    const std::vector<uint64_t> ids(effected_.begin(), effected_.end());
    for (const uint64_t id : ids) {
        const auto found = npcs_.find(id);
        if (found == npcs_.end()) {
            effected_.erase(id);
            continue;
        }
        Npc& n = *found->second;
        // the player who applied it left or died: what he put on the creature ends (what is on his pet goes with the pet)
        for (size_t i = 0; !is_pet(id) && i < n.effects.size(); ++i) {
            PlayerPort* who = player(n.effects[i].by);
            if (who && who->fighter().alive()) continue;
            drop_effects_of(n.effects[i].by);
            break;
        }
        if (n.effects.empty()) continue;
        // what falls due is worked out first: a tick can kill the creature
        struct Due {
            const abilities::EffectDef* def;
            uint64_t                    by;
            double                      at;
            int                         stacks = 1;   // (a tick counts for the stacks held when it falls due)
        };
        std::vector<Due> ticks, expired, broken;
        for (size_t i = 0; i < n.effects.size();) {
            Npc::HeldEffect& h = n.effects[i];
            // an end comes before a tick of the same moment: the tick that would fall on an effect's last moment never happens (retail's Hemorrhage and Conflagration)
            while (h.next_tick > 0 && h.next_tick <= now && (h.until < 0 || h.next_tick < h.until - 1e-6)) {
                ticks.push_back({h.def, h.by, h.next_tick, h.stacks});
                h.next_tick += h.def->row.tick_timer;
            }
            const FragileRow* fragile = fragile_effect(h.def->row.guid);
            bool breaks = fragile && fragile->holds != 0 && npc_effect_count(id, fragile->holds) > 0;
            if (fragile && fragile->with_cast) {   // it lasts while the pet that gave it plays the ability
                const auto cast = pet_casts_.find(pet_of_owner(h.by));
                breaks = cast == pet_casts_.end() || cast->second.target != id;
            }
            if (h.until >= 0 && h.until <= now) {
                expired.push_back({h.def, h.by, h.until});
                remove_effect_at(n, i);
            } else if (breaks) {
                broken.push_back({h.def, h.by, now});
                remove_effect_at(n, i);
            } else {
                ++i;
            }
        }
        if (!expired.empty() || !broken.empty()) effects_changed(n, now);
        for (const Due& d : expired) run_phases(id, *d.def, d.by, {"Expire", "End"}, d.at);
        for (const Due& d : broken) run_phases(id, *d.def, d.by, {"Remove", "End"}, d.at);
        for (const Due& d : ticks) run_phases(id, *d.def, d.by, {"Tick"}, d.at, d.stacks);
    }
}

// ---- a player's pet -------------------------------------------------------------------------------------------------------------------

bool CombatWorld::pet_join(uint64_t pet, uint64_t owner) {
    PlayerPort* port = player(owner);
    Npc* n = port ? combatant(pet) : nullptr;
    if (!n) return false;
    n->aligns = port->fighter().alignments();   // (a creature's hits resolve against it as they do against its owner)
    pet_owners_[pet] = owner;
    if (pet_ports_.count(pet) == 0) {
        auto made = std::make_unique<PetPort>(*this, *n);
        add_player(made.get());
        pet_ports_[pet] = std::move(made);
    }
    return true;
}

bool CombatWorld::pet_use(uint64_t pet, uint64_t owner, int64_t ability, uint64_t target, double reach_cm) {
    const abilities::AbilityDef* def = catalog_.ability(ability);
    if (!def || !pet_join(pet, owner)) return false;
    Npc* n = combatant(pet);
    if (!n || n->dead || ((n->crowd & kCrowdStopsActing) != 0 && n->crowd_stops == CrowdStops::All)) return false;
    PetCast cast;
    cast.owner = owner;
    cast.target = target;
    cast.ability = def;
    cast.reach_cm = reach_cm;
    if (const auto fight = pet_fights_.find(pet); fight != pet_fights_.end()) {   // the swing under way gives way; the fight goes on after the ability
        fight->second.swinging = false;
        fight->second.due.clear();
        fight->second.pursuing = false;
    }
    stop_cast(*n);
    pet_casts_[pet] = std::move(cast);
    return true;
}

void CombatWorld::pet_start_cast(Npc& pet, PetCast& cast, double now) {
    const abilities::AbilityDef& def = *cast.ability;
    PlayerPort* owner = player(cast.owner);
    const MontageData* montage = nullptr;
    if (anim_ && owner) {
        // the first of its montages whose choice holds (a Hunter's melee or ranged one) and that the tables read
        for (const content::MontageRow& m : def.montages) {
            if (m.soft_path.empty() || !resolver_.holds(owner->fighter(), m.select_expr, true)) continue;
            montage = anim_->montage(m.soft_path);
            if (montage && montage->length > 0 && montage->rate_scale > 0) break;
            montage = nullptr;
        }
    }
    double seconds = 1.0;
    bool timed = false;
    bool played = false;
    if (montage) {
        seconds = std::max(seconds, montage->seconds(1.0));
        const world::WorldActor* me = registry_.find(pet.actor_id);
        const auto play = bootstrap::montage_for(montage->path, montage->guid, montage->package_guid);
        if (me && npc_knows_ability(*me, def.row.guid) && play) {
            NpcCast shown;
            shown.ability = def.row.guid;
            shown.target = cast.target;
            const world::WorldActor* aim = cast.target ? registry_.find(cast.target) : nullptr;
            const Vec3 at = aim ? Vec3{aim->x, aim->y, aim->z} : pet.position();
            shown.tx = at.x;
            shown.ty = at.y;
            shown.tz = at.z;
            shown.montage = montage->guid;
            start_npc_cast(registry_, pet.actor_id, pet.cast, shown, play->chain);
            played = true;
        }
        const auto add = [&](double delay, int key, const char* source) {
            for (const content::AbilityHitRow& r : def.hits) {
                if (r.source != source || r.key != key) continue;
                cast.due.emplace_back(now + delay, r.hit_guid);
                timed = true;
                break;
            }
        };
        for (const MontageData::Hit& h : montage->hits) add(h.time / montage->rate_scale, static_cast<int>(h.key), "hit");
        const world::WorldActor* aim_at = cast.target ? registry_.find(cast.target) : nullptr;
        for (const MontageData::Fire& f : montage->fires) {
            // a bolt lands when it has flown: the montage's notify launches it on the clients
            double flying = 0.3;
            if (aim_at) {
                for (const content::AbilityHitRow& r : def.hits) {
                    if (r.source == "projectile" && r.key == f.key) flying = flight_seconds(r, pet.position(), Vec3{aim_at->x, aim_at->y, aim_at->z});
                }
            }
            add(f.time / montage->rate_scale + flying, static_cast<int>(f.key), "projectile");
        }
    }
    if (!timed) {
        // no notify to go by: a moment into it, every hit of the pet's present stance (a Hunter's hits are named for theirs)
        const bool ranged = cast.reach_cm >= 1000.0;
        std::set<int64_t> done;
        for (const content::AbilityHitRow& r : def.hits) {
            const abilities::HitDef* hit = catalog_.hit(r.hit_guid);
            if (!hit || !done.insert(r.hit_guid).second) continue;
            const std::string& name = hit->row.name;
            if (ranged ? name.find("Melee") != std::string::npos : name.find("Ranged") != std::string::npos) continue;
            if (name.ends_with("_Redirect") || name.ends_with("_Remove")) continue;   // (these answer something later, they are not part of the swing)
            cast.due.emplace_back(now + 0.4, r.hit_guid);
        }
    }
    cast.end_at = now + seconds;
    cast.started = true;
    ++stats_.npc_attacks;
    spdlog::info("[pets] the pet (actor {}) starts {} at actor {}: {:.2f} s, {} hit(s) due, montage {}", pet.actor_id, def.row.name, cast.target, seconds, cast.due.size(),
                 played ? "shown" : montage ? "known to no list: not shown" : "none in the tables");
    // Lunge: the melee Hunter leaps at its target, the ranged one away from it [the montages are Engage and Disengage; the distance is the targeting record's 600 cm]
    if (def.row.guid == pets::kHunterLunge && cast.target != 0 && npcs_world_) {
        if (const world::WorldActor* aim = registry_.find(cast.target)) {
            const Vec3 from = pet.position();
            const double dx = aim->x - from.x, dy = aim->y - from.y, length = std::max(1.0, std::hypot(dx, dy));
            const bool ranged = cast.reach_cm >= 1000.0;
            const nav::Vec3 to = ranged ? nav::Vec3{from.x - dx / length * 600.0, from.y - dy / length * 600.0, from.z} : nav::Vec3{aim->x, aim->y, aim->z};
            npcs_world_->order_move(pet.actor_id, to, 2500.0, ranged ? 0.0 : 150.0);
        }
    }
}


void CombatWorld::pet_land_on_self(Npc& pet, uint64_t owner, const abilities::HitDef& hit, double now) {
    PlayerPort* port = player(owner);
    if (!port || pet.dead) return;
    if (hit.max_remove > 0 || !hit.remove_effects.empty()) remove_effects_from_npc(pet.actor_id, hit, now);
    Resolved r;
    std::string why;
    HitMeta meta;
    meta.source_type = content::kSourceTypeAbility;
    if (!hit.mods.empty() && resolver_.resolve(port->fighter(), pet, hit, meta, r, why)) {
        for (const ResolvedMod& m : r.mods) {
            if (m.stat_guid != data_.ids().health || !std::isfinite(m.value)) continue;
            const double after = std::clamp(pet.health() + m.value, 0.0, pet.max_health());
            pet.cs->set_current(m.stat_guid, static_cast<float>(after));
            write_stat(registry_, pet.actor_id, m.stat_guid, static_cast<float>(after));
        }
    }
    std::vector<LandedEffect> landed;
    resolver_.effects_of(port->fighter(), pet, hit, landed);
    for (const LandedEffect& e : landed) {
        if (e.shield > 0) add_shield(pet.actor_id, e.shield, std::max(0.0, e.seconds), now);
        apply_effect_to_npc(pet.actor_id, e, owner, now);
    }
}

void CombatWorld::pet_land(Npc& pet, const PetCast& cast, const abilities::HitDef& hit, double now, int depth) {
    if (depth > 3 || hit.row.name.ends_with("_OwnerCost")) return;   // (the cost was paid when the ability was given)
    const int mask = hit.row.applicable_targets;
    PlayerPort* owner = player(cast.owner);
    if (!owner) return;
    if ((mask & 2) != 0) {
        const pets::HitArea area = pets::area_of_hit(hit.row.guid);
        const world::WorldActor* aim = cast.target ? registry_.find(cast.target) : nullptr;
        LandSource src;
        src.type = content::kSourceTypeAbility;
        src.guid = cast.ability->row.guid;
        src.ability = cast.ability;
        src.pet = pet.actor_id;
        src.children = false;
        src.skip_requirement = area.others_only;   // (its requirement is "not the owner's target", which is the test here)
        if (area.radius_cm > 0) {
            const Vec3 centre = area.around_pet || !aim ? pet.position() : Vec3{aim->x, aim->y, aim->z};
            for (const GatheredTarget& g : enemies_around(owner->fighter(), centre, area.radius_cm, 40)) {
                if (area.others_only && g.actor == cast.target) continue;
                if (!is_pet(g.actor)) land_for_player(cast.owner, g.actor, hit, src, now);
            }
        } else if (aim && !is_pet(cast.target) && cast.target != cast.owner) {
            land_for_player(cast.owner, cast.target, hit, src, now);
        }
    }
    if ((mask & 5) == 5) {   // one of the two: the pet when it was aimed at, else its owner
        if (cast.target == pet.actor_id) pet_land_on_self(pet, cast.owner, hit, now);
        else owner->hit_from_pet(hit.row.guid, cast.ability->row.guid, now);
    } else if ((mask & 1) != 0) {
        pet_land_on_self(pet, cast.owner, hit, now);
    } else if ((mask & (4 | 8)) != 0) {
        owner->hit_from_pet(hit.row.guid, cast.ability->row.guid, now);
    }
    if (cast.ability->row.guid == pets::kGuardianTaunt && cast.target != 0) {
        const auto taunted = npcs_.find(cast.target);
        if (taunted != npcs_.end() && !taunted->second->dead) {
            // it turns on the Guardian: more than anyone on its table by the margin a target changes at [modelled: the hit's threat is the design's own]
            double top = 0;
            for (const npc::ai::ThreatTable::Entry& e : taunted->second->ai.threat().entries()) top = std::max(top, e.threat);
            taunted->second->ai.notice(pet.actor_id, top * npc::ai::ThreatTable::kSwitchRatio + taunted->second->max_health(), now);
        }
    }
    if (hit.row.guid == pets::kHunterMeleeIncapHit && cast.target != 0) pet_stand_downs_[pet.actor_id] = cast.target;
    for (const content::RecursiveHitRow& r : hit.recursive) {
        if (const abilities::HitDef* child = catalog_.hit(r.child_hit)) pet_land(pet, cast, *child, now, depth + 1);
    }
}

void CombatWorld::tick_pet_casts(double now) {
    for (auto it = pet_casts_.begin(); it != pet_casts_.end();) {
        const uint64_t id = it->first;
        PetCast& cast = it->second;
        Npc* pet = combatant(id);
        PlayerPort* owner = player(cast.owner);
        bool over = !pet || pet->dead || !owner || !owner->fighter().alive();
        if (!over && (pet->crowd & kCrowdStopsActing) != 0 && pet->crowd_stops == CrowdStops::All) over = true;
        const bool at_enemy = cast.target != 0 && cast.target != id && cast.target != cast.owner && (cast.ability->row.applicable_targets & 2) != 0;
        const auto aimed = at_enemy ? npcs_.find(cast.target) : npcs_.end();
        if (!over && at_enemy && (aimed == npcs_.end() || aimed->second->dead)) over = true;
        if (over) {
            spdlog::info("[pets] the pet (actor {}): {} ends {} its hits: {}", id, cast.ability->row.name, cast.started ? "before all of" : "before any of",
                         !pet || pet->dead ? "the pet is gone" : !owner || !owner->fighter().alive() ? "its owner is gone" : at_enemy && (aimed == npcs_.end() || aimed->second->dead) ? "its target is gone" : "crowd control");
            if (pet) stop_cast(*pet);
            it = pet_casts_.erase(it);
            continue;
        }
        if (!cast.started) {
            const double distance = at_enemy ? distance_xy_cm(pet->position(), aimed->second->position()) - pet->radius_cm() - aimed->second->radius_cm() : 0.0;
            if (distance > cast.reach_cm && (pet->crowd & kCrowdStopsMoving) == 0) {
                if (npcs_world_ && !cast.pursuing) {
                    const uint64_t who = cast.target;
                    cast.pursuing = npcs_world_->order_pursue(
                        id,
                        [this, who](nav::Vec3& out) {
                            const world::WorldActor* a = registry_.find(who);
                            if (!a) return false;
                            out = to_nav(Vec3{a->x, a->y, a->z});
                            return true;
                        },
                        720.0, std::max(100.0, cast.reach_cm * 0.8));
                }
                ++it;
                continue;
            }
            if (distance > cast.reach_cm) {   // rooted, out of reach: it does not happen
                spdlog::info("[pets] the pet (actor {}): {} does not happen: rooted, {:.0f} cm away with a reach of {:.0f}", id, cast.ability->row.name, distance, cast.reach_cm);
                it = pet_casts_.erase(it);
                continue;
            }
            if (npcs_world_) npcs_world_->halt(id);
            pet_start_cast(*pet, cast, now);
        }
        std::vector<int64_t> landing;
        for (auto d = cast.due.begin(); d != cast.due.end();) {
            if (d->first <= now) {
                landing.push_back(d->second);
                d = cast.due.erase(d);
            } else {
                ++d;
            }
        }
        const PetCast held = cast;   // (a kill or a death in what lands can change the world's lists)
        for (const int64_t guid : landing) {
            if (const abilities::HitDef* hit = catalog_.hit(guid)) {
                spdlog::info("[pets] the pet (actor {}): the hit {} of {} lands", id, hit->row.name, held.ability->row.name);
                pet_land(*pet, held, *hit, now);
            }
        }
        // the ranged Incapacitate is a channel: the pet holds it while its target holds the effect
        const bool channel = cast.ability->row.guid == pets::kHunterIncapacitate && cast.reach_cm >= 1000.0 && cast.target != 0 && npc_effect_count(cast.target, pets::kHunterIncapRanged) > 0;
        if (cast.due.empty() && now >= cast.end_at && !channel) {
            stop_cast(*pet);
            it = pet_casts_.erase(it);
            continue;
        }
        ++it;
    }
}

bool CombatWorld::pet_attack(uint64_t pet, uint64_t owner, int64_t ability, uint64_t target) {
    if (pet == target || !player(owner)) return false;
    const abilities::AbilityDef* def = catalog_.ability(ability);
    if (!def) return false;
    const world::WorldActor* t = registry_.find(target);
    if (!t || t->kind != world::ActorKind::Npc) return false;
    Fighter& side = player(owner)->fighter();
    const npc::MotionStats bp = motion_.get(t->npc.archetype);
    if (data_.relation(side.alignments(), std::vector<int64_t>(bp.alignments.begin(), bp.alignments.end())) != Relation::Hostile) return false;
    Npc* other = combatant(target);
    if (!other || other->dead || !pet_join(pet, owner)) return false;
    PetFight fight;
    fight.owner = owner;
    fight.target = target;
    fight.ability = def;
    fight.reach_cm = launches_projectiles(*def) ? 1500.0 : 250.0;
    pet_fights_[pet] = std::move(fight);
    return true;
}

void CombatWorld::pet_stop(uint64_t pet) {
    if (pet_fights_.erase(pet) == 0) return;
    const auto it = npcs_.find(pet);
    if (it != npcs_.end()) stop_cast(*it->second);
    if (npcs_world_) npcs_world_->halt(pet);
}

void CombatWorld::pet_gone(uint64_t pet) {
    pet_stop(pet);
    pet_stand_downs_.erase(pet);
    pet_casts_.erase(pet);
    pet_owners_.erase(pet);
    effected_.erase(pet);
    if (const auto it = pet_ports_.find(pet); it != pet_ports_.end()) {
        remove_player(it->second.get());
        pet_ports_.erase(it);
    }
    forget(pet);
}

void CombatWorld::pet_swing(Npc& pet, PetFight& fight, const Npc& target, double now) {
    const abilities::AbilityDef& def = *fight.ability;
    double seconds = 1.5;
    bool timed = false;
    // the first of its montages the tables read: its notifies say when each hit lands.  A projectile's hit lands a moment after its notify (the pet's bolts do not fly in the world yet)
    const MontageData* montage = nullptr;
    if (anim_) {
        for (const content::MontageRow& m : def.montages) {
            if (m.soft_path.empty()) continue;
            montage = anim_->montage(m.soft_path);
            if (montage && montage->length > 0 && montage->rate_scale > 0) break;
            montage = nullptr;
        }
    }
    if (montage) {
        seconds = std::max(seconds, montage->seconds(1.0));
        const world::WorldActor* me = registry_.find(pet.actor_id);
        const auto play = bootstrap::montage_for(montage->path, montage->guid, montage->package_guid);
        if (me && npc_knows_ability(*me, def.row.guid) && play) {
            NpcCast cast;
            cast.ability = def.row.guid;
            cast.target = target.actor_id;
            const Vec3 at = target.position();
            cast.tx = at.x;
            cast.ty = at.y;
            cast.tz = at.z;
            cast.montage = montage->guid;
            start_npc_cast(registry_, pet.actor_id, pet.cast, cast, play->chain);
        }
        const auto add = [&](double delay, int key, const char* source) {
            for (const content::AbilityHitRow& r : def.hits) {
                if (r.source != source || r.key != key) continue;
                const abilities::HitDef* hit = catalog_.hit(r.hit_guid);
                if (!hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) continue;
                fight.due.emplace_back(now + delay, r.hit_guid);
                timed = true;
                break;
            }
        };
        for (const MontageData::Hit& h : montage->hits) add(h.time / montage->rate_scale, static_cast<int>(h.key), "hit");
        for (const MontageData::Fire& f : montage->fires) {
            double flying = 0.3;   // the bolt lands when it has flown
            for (const content::AbilityHitRow& r : def.hits) {
                if (r.source == "projectile" && r.key == f.key) flying = flight_seconds(r, pet.position(), target.position());
            }
            add(f.time / montage->rate_scale + flying, static_cast<int>(f.key), "projectile");
        }
    }
    if (!timed) {   // no notify the tables know: its first hit that hurts lands a moment into the swing
        for (const content::AbilityHitRow& r : def.hits) {
            const abilities::HitDef* hit = catalog_.hit(r.hit_guid);
            if (!hit || (hit->row.applicable_targets & 2) == 0 || hit->mods.empty()) continue;
            fight.due.emplace_back(now + 0.4, r.hit_guid);
            break;
        }
    }
    fight.end_at = now + seconds;
    fight.next_at = fight.end_at;
    fight.swinging = true;
    ++stats_.npc_attacks;
}

void CombatWorld::tick_pets(double now) {
    tick_pet_casts(now);
    // a pet's Health comes back as a player's does, once a second by its HealthRegeneration, whose formula gives an NPC no out-of-combat bonus (a creature resets instead; a pet does not)
    if (now >= pet_regen_at_) {
        pet_regen_at_ = now + 1.0;
        for (const auto& [id, owner] : pet_owners_) {
            Npc* pet = combatant(id);
            if (!pet || pet->dead || !pet->cs) continue;
            const stats::StatValue* regen = pet->cs->find(content::kGuidHealthRegeneration);
            const double max = pet->max_health();
            if (!regen || !(regen->value > 0) || !(pet->health() < max)) continue;
            const float after = static_cast<float>(std::min(max, pet->health() + static_cast<double>(regen->value)));
            pet->cs->set_current(data_.ids().health, after);
            write_stat(registry_, id, data_.ids().health, after);
        }
    }
    for (auto it = pet_fights_.begin(); it != pet_fights_.end();) {
        const uint64_t id = it->first;
        PetFight& fight = it->second;
        Npc* pet = combatant(id);
        if (pet_casts_.count(id) != 0 || (pet && (pet->crowd & kCrowdStopsActing) != 0 && pet->crowd_stops == CrowdStops::All)) {   // the fight waits for its ability, and for what stops it
            ++it;
            continue;
        }
        PlayerPort* owner = player(fight.owner);
        const auto found = npcs_.find(fight.target);
        const bool over = !pet || pet->dead || !owner || !owner->fighter().alive() || found == npcs_.end() || found->second->dead;
        if (over) {
            if (pet) stop_cast(*pet);
            if (npcs_world_) npcs_world_->halt(id);
            it = pet_fights_.erase(it);
            continue;
        }
        Npc& target = *found->second;
        if (distance_xy_cm(owner->fighter().position(), target.position()) > 5000.0) {   // the owner has run off: the pet goes with it
            stop_cast(*pet);
            if (npcs_world_) npcs_world_->halt(id);
            it = pet_fights_.erase(it);
            continue;
        }
        // the hits that fall due; a kill can end the fight, seen at the next step
        std::vector<int64_t> landing;
        for (auto d = fight.due.begin(); d != fight.due.end();) {
            if (d->first <= now) {
                landing.push_back(d->second);
                d = fight.due.erase(d);
            } else {
                ++d;
            }
        }
        const uint64_t owner_id = fight.owner, target_id = fight.target;
        const abilities::AbilityDef* ability = fight.ability;
        for (const int64_t guid : landing) {
            const abilities::HitDef* hit = catalog_.hit(guid);
            const auto again = npcs_.find(target_id);
            if (!hit || again == npcs_.end() || again->second->dead) continue;
            HitMeta meta;
            meta.source_type = content::kSourceTypeAbility;
            meta.source_guid = ability->row.guid;
            meta.ability_tags = ability->row.tags;
            Resolved r;
            std::string why;
            // a pet's numbers are its owner's (the game scales a pet from its GetCreatureOwner stats; the generic creature stats a pet has here are many times a player's at its level)
            if (!resolver_.resolve(owner->fighter(), *again->second, *hit, meta, r, why)) {
                spdlog::debug("[pets] {}: {}", id, why);
                continue;
            }
            HitOrigin origin;
            origin.hit = hit;
            origin.source_type = meta.source_type;
            origin.source_guid = meta.source_guid;
            origin.ability_tags = meta.ability_tags;
            const Delivery d = apply_hit_to_npc(target_id, owner_id, r, now, id, &origin);
            if (d.applied) {
                DealtHit dealt{guid, meta.source_type, meta.source_guid, id, target_id, d.flags, r.mods, d.effective};
                owner->hit_dealt(dealt, now);
            }
        }
        if (fight.swinging && now >= fight.end_at) {
            fight.swinging = false;
            stop_cast(*pet);
        }
        if (!fight.swinging) {
            const double distance = distance_xy_cm(pet->position(), target.position()) - pet->radius_cm() - target.radius_cm();
            if (distance > fight.reach_cm) {
                if (npcs_world_ && !fight.pursuing && (pet->crowd & kCrowdStopsMoving) == 0) {
                    const uint64_t who = target_id;
                    fight.pursuing = npcs_world_->order_pursue(
                        id,
                        [this, who](nav::Vec3& out) {
                            const world::WorldActor* a = registry_.find(who);
                            if (!a) return false;
                            out = to_nav(Vec3{a->x, a->y, a->z});
                            return true;
                        },
                        720.0, std::max(100.0, fight.reach_cm * 0.8));
                }
            } else if (now >= fight.next_at) {
                if (npcs_world_) npcs_world_->halt(id);
                fight.pursuing = false;
                pet_swing(*pet, fight, target, now);
            }
        }
        ++it;
    }
}

void CombatWorld::kill(Npc& n, uint64_t killer, double now, bool gm) {
    if (n.dead) return;
    n.dead = true;
    n.killer = killer;
    n.ai.kill();
    n.cs->set_in_combat(false);
    n.cs->set_current(content::kGuidHealth, 0.0f);
    write_stat(registry_, n.actor_id, content::kGuidHealth, 0.0f);
    write_combat(registry_, n.actor_id, false, true, 0);
    n.flags_in_combat = false;
    n.flags_target = 0;
    clear_effects(n, now);
    if (npcs_world_) npcs_world_->halt(n.actor_id);
    stop_cast(n);   // the animation of an attack under way stops with the creature
    n.hold_until = 0;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.npc == n.actor_id; }), pending_.end());
    ++stats_.kills;
    if (death_hook_) {
        const world::WorldActor* a = registry_.find(n.actor_id);
        DeathEvent e;
        e.npc = n.actor_id;
        e.killer = killer;
        e.at = now;
        e.level = n.lvl;
        e.elite = n.cs ? static_cast<int>(n.cs->npc().elite_rank) : 0;
        e.position = n.position();
        e.blueprint = n.leaf;
        e.info = a ? &a->npc : nullptr;
        e.motion = &n.bp;
        e.attackers = n.attackers;
        e.gm = gm;
        death_hook_(e);
    }
}

double CombatWorld::effect_remaining(uint64_t npc_actor, int64_t effect_guid, double now) const {
    const auto it = npcs_.find(npc_actor);
    if (it == npcs_.end()) return 0.0;
    double left = 0.0;
    for (const Npc::HeldEffect& h : it->second->effects) {
        if (h.def->row.guid != effect_guid) continue;
        left = h.until < 0 ? std::numeric_limits<double>::infinity() : std::max(left, h.until - now);
    }
    return left;
}

int CombatWorld::effect_stacks(uint64_t npc_actor, int64_t effect_guid) const {
    const auto it = npcs_.find(npc_actor);
    if (it == npcs_.end()) return 0;
    int stacks = 0;
    for (const Npc::HeldEffect& h : it->second->effects) {
        if (h.def->row.guid == effect_guid) stacks += h.stacks;
    }
    return stacks;
}

size_t CombatWorld::effect_count(uint64_t npc_actor) const {
    const auto it = npcs_.find(npc_actor);
    return it == npcs_.end() ? 0 : it->second->effects.size();
}

void CombatWorld::drop_effects_of(uint64_t instigator) {
    const std::vector<uint64_t> ids(effected_.begin(), effected_.end());
    for (const uint64_t id : ids) {
        const auto it = npcs_.find(id);
        if (it == npcs_.end()) continue;
        Npc& n = *it->second;
        bool removed = false;
        for (size_t i = 0; !is_pet(id) && i < n.effects.size();) {
            if (n.effects[i].by == instigator) {
                remove_effect_at(n, i);
                removed = true;
            } else {
                ++i;
            }
        }
        if (!removed) continue;
        if (n.effects.empty()) {   // nothing is left to burn what the stats hold
            for (const int64_t stat : {data_.ids().status_burning, data_.ids().status_burning_dur}) {
                if (stat != 0) n.cs->set_current(stat, 0.0f);
            }
        }
        effects_changed(n, now_);
        spdlog::info("[effects] the effects of player {} on {} ({}) end: he is gone", instigator, n.leaf, n.actor_id);
    }
}

// ---- a game master's hands on an NPC ------------------------------------------------------------------------------------------------

bool CombatWorld::gm_kill(uint64_t actor_id, double now, std::string& why) {
    Npc* n = combatant(actor_id);
    if (!n) {
        why = "worldd has no combat stats for this creature";
        return false;
    }
    if (n->dead) {
        why = "it is dead already";
        return false;
    }
    kill(*n, 0, now, true);
    return true;
}

bool CombatWorld::gm_heal(uint64_t actor_id) {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end()) return true;   // it has never been in a fight: its health is full
    Npc& n = *it->second;
    if (n.dead) return false;
    if (const stats::StatValue* max = n.cs->find(data_.ids().max_health)) {
        n.cs->set_current(content::kGuidHealth, max->value);
        write_stat(registry_, actor_id, content::kGuidHealth, max->value);
    }
    return true;
}

CombatWorld::GmReset CombatWorld::gm_reset(uint64_t actor_id, double now) {
    const auto it = npcs_.find(actor_id);
    if (it == npcs_.end()) return GmReset::Standing;   // it has never been in a fight: no threat to clear, health full
    Npc& n = *it->second;
    if (n.dead) return GmReset::Dead;
    n.ai.reset_now();
    begin_reset(n, now);   // healed, its flags down, an attack under way stopped, and sent home
    const npc::Mover* m = npcs_world_ ? npcs_world_->mover(actor_id) : nullptr;
    if (m && m->moving()) return GmReset::Walking;
    // no way home on foot (or none to walk): it is home as far as the fight goes; the caller stands it at its spawn point
    n.ai.revive();
    n.returning = false;
    if (npcs_world_) npcs_world_->release(actor_id);
    return GmReset::Standing;
}

// ---- thinking -------------------------------------------------------------------------------------------------------------------

void CombatWorld::set_flags(Npc& n, bool in_combat, uint64_t target_actor) {
    const uint64_t object = target_actor;
    if (n.flags_in_combat == in_combat && n.flags_target == object) return;
    n.flags_in_combat = in_combat;
    n.flags_target = object;
    n.cs->set_in_combat(in_combat);
    // the one it fights is named as the client knows his pawn: the NetGUID in full, with the session's ServerId and Randomizer
    const PlayerPort* who = object != 0 ? player(object) : nullptr;
    write_combat(registry_, n.actor_id, in_combat, false, who ? who->pawn_guid() : ::aoc::protocol::emit::FIntrepidNetworkGUID::from_u64(object));
}

void CombatWorld::begin_reset(Npc& n, double now) {
    // a reset leaves a damage over time or a debuff on it for the walk home; only what stops or slows its body ends, or it could not walk
    bool freed = false;
    for (size_t i = 0; i < n.effects.size();) {
        if (crowd_control(n.effects[i].def->row.crowd_control_guid)) {
            remove_effect_at(n, i);
            freed = true;
        } else {
            ++i;
        }
    }
    if (freed) effects_changed(n, now);
    ++stats_.resets;
    n.returning = true;
    n.attackers.clear();
    // it heals to full as it walks home
    if (const stats::StatValue* max = n.cs->find(data_.ids().max_health)) {
        n.cs->set_current(content::kGuidHealth, max->value);
        write_stat(registry_, n.actor_id, content::kGuidHealth, max->value);
    }
    stop_cast(n);   // ... and with a fight that is reset
    n.hold_until = 0;
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& p) { return p.npc == n.actor_id; }), pending_.end());
    set_flags(n, false, 0);
    if (npcs_world_) npcs_world_->order_return_home(n.actor_id);
}

void CombatWorld::pursue(Npc& n, uint64_t target, double speed_cm_s, double stop_distance_cm) {
    if (!npcs_world_ || (n.crowd & kCrowdStopsMoving) != 0) return;   // (a root: it stands, and fights what it can reach)
    npcs_world_->order_pursue(
        n.actor_id,
        [this, target](nav::Vec3& out) {
            PlayerPort* p = player(target);
            if (!p || !p->fighter().alive()) return false;
            out = to_nav(p->fighter().position());
            return true;
        },
        speed_cm_s, stop_distance_cm);
}

bool CombatWorld::faces(const Npc& n, const Vec3& at, double tolerance_degrees) const {
    const world::WorldActor* w = registry_.find(n.actor_id);
    if (!w) return true;
    return angle_between_degrees(std::atan2(at.y - w->y, at.x - w->x) * 180.0 / 3.14159265358979323846, w->yaw_degrees) <= tolerance_degrees;
}

void CombatWorld::think(Npc& n, double now) {
    if (n.dead) return;
    if ((n.crowd & kCrowdStopsActing) != 0 && n.crowd_stops == CrowdStops::All) return;   // stunned: it does nothing until that ends, and it stays on its table
    const world::WorldActor* a = registry_.find(n.actor_id);
    if (!a) return;
    npc::ai::Sense s;
    s.now = now;
    s.self = {a->x, a->y, a->z};
    s.self_radius_cm = n.bp.radius;
    s.home = {a->npc.home_x, a->npc.home_y, a->npc.home_z};
    s.self_yaw_degrees = a->yaw_degrees;
    s.attack_range_cm = n.next.range_cm;
    if (n.shot_reach_cm > 0) s.attack_range_cm = n.shot_reach_cm;   // a creature that shoots starts casting once its target is within the reach of its shots, whichever attack the preview pick is (right after a cast it is on its timer)
    const double far = n.ai.params().leash_radius_cm * 2.0 + n.ai.params().aggro_radius_cm;
    for (PlayerPort* p : players_) {
        const Fighter& f = p->fighter();
        if (distance_xy_cm(f.position(), s.self) > far) continue;
        npc::ai::Candidate c;
        c.id = p->actor_id();
        c.pos = f.position();
        c.radius_cm = f.radius_cm();
        c.alive = f.alive();
        c.hostile = !is_pet(c.id);   // (a creature does not notice a pet by itself: the pet gets on its table by hurting it)
        s.known.push_back(c);
    }
    if (nav_) {
        s.line_of_sight = [this](const Vec3& from, const Vec3& to) {
            const nav::Vec3 p = to_nav(from), q = to_nav(to);
            if (!nav_->on_navmesh(p, 150, 300) || !nav_->on_navmesh(q, 150, 300)) return true;   // off the navmesh the ground says nothing
            return nav_->line_of_sight(p, q);
        };
    }
    const npc::ai::State before = n.ai.state();
    const npc::ai::Orders o = n.ai.think(s);
    if (o.began_fight) {
        ++stats_.aggros;
        n.fight_began = now;
        n.fight_rolls.clear();
        // its pack joins in: creatures of its side within its link radius that would start a fight by themselves and can reach it (modelled: the data gives the radius, not who answers).  They
        // get threat through the AI only: joining is no attack on them, and `attackers` is who may loot.
        if (!n.joined_pack && n.link_radius_cm > 0 && o.target != 0) {
            std::vector<uint64_t> near;   // (from the registry: a neighbour may not be a combatant yet, the player having just come into the scan's reach)
            registry_.query(s.self.x, s.self.y, n.link_radius_cm, near);
            for (const uint64_t id : near) {
                const world::WorldActor* mate = registry_.find(id);
                if (id == n.actor_id || !mate || mate->kind != world::ActorKind::Npc || (huntable_ && huntable_(mate->npc))) continue;
                const npc::MotionStats bp = motion_.get(mate->npc.archetype);
                const std::vector<int64_t> side(bp.alignments.begin(), bp.alignments.end());
                if (side != n.aligns && data_.relation(n.aligns, side) != Relation::Friendly) continue;
                const Vec3 at{mate->x, mate->y, mate->z};
                if (distance_xy_cm(at, s.self) > n.link_radius_cm || std::fabs(at.z - s.self.z) > n.link_radius_cm) continue;
                if (s.line_of_sight && !s.line_of_sight(s.self, at)) continue;
                Npc* made = combatant(id);
                if (!made) continue;
                Npc& m = *made;
                if (m.dead || m.returning || m.ai.state() != npc::ai::State::Idle || !m.ai.params().proximity_aggro) continue;
                m.joined_pack = true;
                m.ai.notice(o.target, m.ai.params().proximity_threat, now);
            }
        }
        n.joined_pack = false;
        if (n.ai.params().senses.enabled) {
            const npc::ai::Params& pr = n.ai.params();
            PlayerPort* who = player(o.target);
            spdlog::info("[combat] {} ({}, level {}) began a fight with player {}{}: it notices within {:.1f} m (hears {:.1f} m, sees {:.1f} m in front), leashes {:.0f} m from {}", n.leaf, n.actor_id, a->npc.level, o.target,
                         who ? fmt::format(", {:.1f} m away", distance_xy_cm(who->fighter().position(), s.self) / 100.0) : std::string(), pr.aggro_radius_cm / 100.0, pr.senses.hearing_radius_cm / 100.0,
                         pr.senses.sight_radius_cm / 100.0, pr.leash_radius_cm / 100.0, pr.anchor_at_fight_start ? "where the fight began" : "home");
        }
    }
    if (o.began_reset) {
        if (n.ai.params().senses.enabled) {
            spdlog::info("[combat] {} ({}, level {}) leashed: {}; {:.1f} m from {}, {:.1f} m from home", n.leaf, n.actor_id, a->npc.level, o.reset_reason,
                         distance_xy_cm(s.self, n.ai.leash_anchor(s.home)) / 100.0, n.ai.params().anchor_at_fight_start ? "where the fight began" : "home", distance_xy_cm(s.self, s.home) / 100.0);
        }
        begin_reset(n, now);
    }
    if (o.reset_done) {
        n.returning = false;
        if (npcs_world_) npcs_world_->release(n.actor_id);
    }
    if (o.in_combat) set_flags(n, true, o.target);
    else if (n.ai.state() == npc::ai::State::Idle || n.ai.state() == npc::ai::State::Reset) set_flags(n, false, 0);
    // A montage that carries the creature (a lunge) is played to the end of its carry: the client moves its copy along the whole montage, so a walk order that took the creature's feet back
    // half way would leave the server's creature behind it.  The AI's walking and turning wait until the carry is over; a reset (the way home) and a death end it.
    const bool carried = npcs_world_ && npcs_world_->root_motion_active(n.actor_id);
    if (npcs_world_ && !o.began_reset && now >= n.hold_until && !carried) {   // (a creature that stands for a montage is not sent anywhere until it is over)
        const nav::Vec3 here{a->x, a->y, a->z};
        switch (o.move) {
            case npc::ai::Orders::Move::ChaseTo: pursue(n, o.target, o.speed_cm_s, o.stop_distance_cm); break;
            case npc::ai::Orders::Move::Stand:
                if (before != npc::ai::State::Attack) npcs_world_->order_move(n.actor_id, here, 0.0, 0.0);
                break;
            default: break;
        }
    }
    if (o.attack && now >= n.next_attack_at) {
        // A creature with its own animation starts an attack only where its tree allows one, every time: from where it stands now, facing the way it faces now.  Where the tree allows none it
        // does not cast: it turns towards the target when only its facing is wrong (it turns at its rate while it stands, and is never stopped to turn), and walks up to it when it is too far, and
        // looks again at its next look.
        bool allowed = true;
        Attack picked;
        bool have_pick = false;
        if (n.anim && npcs_world_ && now >= n.hold_until) {
            PlayerPort* port = player(o.target);
            TreeWhy why;
            if (port) {
                picked = choose_animated(n, &port->fighter(), &why);
                have_pick = picked.ability != nullptr;
            }
            if (port && !have_pick) {
                allowed = false;
                const Vec3 at = port->fighter().position();
                if (why.off_angle) {
                    npcs_world_->turn_toward(n.actor_id, at.x, at.y);
                } else if (why.too_far) {
                    // it walks up to the reach of the nearest-reaching attack that is ready, not of the attack it stands ready with: a long-reaching special that waits for a timer (a captain's Bash, a
                    // raider's heavy slash) would otherwise park the creature at its reach until the timer ran out, looking at its target
                    const double reach = why.reach_cm >= 0 ? why.reach_cm : static_cast<double>(n.next.range_cm);
                    const double stop = std::max(0.0, reach * n.ai.params().chase_stop_fraction) + n.radius_cm() + port->fighter().radius_cm();
                    pursue(n, o.target, n.ai.params().chase_speed_cm_s, stop);
                } else if (n.stand_off_cm > 0 && distance_xy_cm(Vec3{a->x, a->y, a->z}, at) - n.radius_cm() - port->fighter().radius_cm() > n.stand_off_cm + 5.0) {
                    // a creature that shoots closes in on its target between two casts, up to the arrival distance of its tree's approach (it does not back away when it is nearer)
                    pursue(n, o.target, n.ai.params().chase_speed_cm_s, n.stand_off_cm + n.radius_cm() + port->fighter().radius_cm());
                }
            } else if (port && have_pick && picked.task && picked.task->stop_movement && !picked.task->rotates && !faces(n, port->fighter().position(), 2.0)) {
                // a task that stands for its montage and does not turn the creature during it begins with the creature facing its target: it turns first, at its rate, and begins at a later look
                allowed = false;
                const Vec3 at = port->fighter().position();
                npcs_world_->turn_toward(n.actor_id, at.x, at.y);
            }
        }
        if (allowed) start_attack(n, o.target, now, have_pick ? &picked : nullptr);
    }
}
// Cyb + C-O-A

void CombatWorld::tick(double now) {
    now_ = now;
    if (!cfg_.enabled) return;
    resolve_pending(now);
    advance_flights(now);
    step_zones(now);
    step_effects(now);   // (every tick: what falls due lands when it falls due)
    if (now < next_think_) return;
    next_think_ = now + cfg_.think_seconds;

    // the players' combat flags end a while after the last exchange
    for (auto it = player_combat_until_.begin(); it != player_combat_until_.end();) {
        if (now >= it->second) {
            if (PlayerPort* p = player(it->first)) p->set_in_combat(false, now);
            it = player_combat_until_.erase(it);
        } else {
            ++it;
        }
    }

    std::set<uint64_t> todo;
    for (PlayerPort* p : players_) {
        const Vec3 pos = p->fighter().position();
        std::vector<uint64_t> ids;
        registry_.query(pos.x, pos.y, cfg_.scan_radius_cm, ids);
        for (const uint64_t id : ids) {
            const world::WorldActor* a = registry_.find(id);
            if (!a || a->kind != world::ActorKind::Npc) continue;
            const auto known = npcs_.find(id);
            if (known != npcs_.end()) {
                todo.insert(id);
                continue;
            }
            if (huntable_ && huntable_(a->npc)) {   // a huntable is no fighter: its stats, nothing more
                fill_stats(*a);
                continue;
            }
            auto h = hostile_by_archetype_.find(a->npc.archetype);
            if (h == hostile_by_archetype_.end()) {
                const npc::MotionStats bp = motion_.get(a->npc.archetype);
                const std::vector<int64_t> al(bp.alignments.begin(), bp.alignments.end());
                h = hostile_by_archetype_.emplace(a->npc.archetype, data_.relation(al, CombatData::player_alignments()) == Relation::Hostile).first;
            }
            if (h->second) todo.insert(id);
        }
    }
    for (const auto& [id, n] : npcs_) {
        if (!n->dead && n->ai.state() != npc::ai::State::Idle) todo.insert(id);
    }
    // Every NPC a player can see opens in the game with the stat list at its level (a guard's Health, a villager's ...), whether it ever fights or not [R: the retail captures' guards and villagers, 9
    // items each, Health 56 at level 1 and 320 at level 5 for a Guard_Wizard].  The templates the NPCs here are made from were recorded without the list (an open that repeated what the client
    // held) or with another guard's (a level-50 Soldier's 13.5 million Health), so it is written from the stat engine, a few of them each second, for the NPCs that are no combatants.
    if (now >= next_stats_) {
        next_stats_ = now + cfg_.stats_seconds;
        size_t budget = 64;
        for (PlayerPort* p : players_) {
            const Vec3 pos = p->fighter().position();
            std::vector<uint64_t> visible;
            registry_.query(pos.x, pos.y, cfg_.stats_radius_cm, visible);
            // the nearest first: the one a player stands beside is filled before a far one
            std::vector<std::pair<double, uint64_t>> todo_stats;
            for (const uint64_t id : visible) {
                if (stats_filled_.count(id) != 0 || npcs_.count(id) != 0) continue;
                const world::WorldActor* a = registry_.find(id);
                if (!a || a->kind != world::ActorKind::Npc) continue;
                todo_stats.emplace_back(std::hypot(a->x - pos.x, a->y - pos.y), id);
            }
            std::sort(todo_stats.begin(), todo_stats.end());
            for (const auto& [distance, id] : todo_stats) {
                if (budget == 0) break;
                if (const world::WorldActor* a = registry_.find(id); a && fill_stats(*a)) --budget;
            }
        }
    }
    for (const uint64_t id : todo) {
        if (is_pet(id)) continue;   // (a pet's fight is its owner's order, not its AI's)
        Npc* n = combatant(id);
        if (n) think(*n, now);
    }
    tick_pets(now);
}

}  // namespace aoc::worldd::combat
