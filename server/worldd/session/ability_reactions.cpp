// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/ability_reactions.cpp  (see ability_pipeline.h)
//
//  The game's reactions (contentc version 35): a small rule an effect carries while it is on its holder. When an event of a kind it is bound to happens to
//  the holder (a hit the holder made lands, an ability's cost is paid) and its trigger expression is true, the reaction applies its hits: to the holder
//  (Owner) or to the targets of the hit that raised the event (Targets), under the reaction's own source so that the client takes them as the server's.
//
//  How the game does it [the client's code, which shares the logic with the server]:
//    - the holder's effects that are registered for the kind of the event are walked in the order they were added, and each one's reactions in the order they
//      are listed: the binding is tested first (one byte of bits per event kind), then the trigger;
//    - a reaction's hits are applied in order through the ordinary hit path; a hit that a reaction made raises no reaction (the data repeats it in its triggers);
//    - a hit raises its event only when its record has event tags or stat mods;
//    - a limit on how often a reaction may fire (GlobalProcLimit, InstigatorProcLimit) is counted per reaction here (one holder), and a carrier that is a
//      pipeline effect is taken off when its reaction reached it.
//
//  What this file serves: the events Hit (OwnerHits) and Ability (Consume), the instigators Owner and Instigator (who put the effect on: the holder itself, unless a
//  creature's hit left it), and the targets Owner and Instigator (the holder), HitSource (the source of the hit that hit the holder: the creature that made it), Targets
//  (the hit's) and HitInstigator (who hit the holder).
//  The hit taken (Hit, OwnerIsHit) is raised for the Cleric's reactions (Defiant Light), the Tank's Vengeance (its cooldown drops 0.5 s for each hit that hurts, 1 s for a
//  block: instigator Instigator, a self hit and a ModifyCooldownsIds row, all served), the Tank's Reflect (Tank_Reflect_Trigger and its Refreshing, on the hit's source) and
//  the Ranger's Thorns (on the hit's instigator).  Guard, Dodge, Camouflage, Stealth and the other classes' hit-taken reactions (the NPC shields, the Proc and Summoner
//  ones) are still left alone: Guard's and Dodge's stamina rules are A7's, and the rest need bindings, instigators or targets this file does not serve yet.
//  The rest of the bindings, instigators and targets of the 335 records are left alone and said once in the log when a reaction would need them.
// ============================================================================
#include "session/ability_pipeline.h"

#include "content/content_db.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace aoc::worldd {

using abilities::AbilityDef;
using abilities::EffectDef;
using abilities::HitDef;
using abilities::ReactionDef;
using bootstrap::AbilityChange;

namespace {

// The EReactionTarget bits of a reaction's TargetsBitmask.
constexpr int kTargetOwner = 1;
constexpr int kTargetTargets = 8;
constexpr int kTargetInstigator = 2;
constexpr int kTargetHitSource = 4;
constexpr int kTargetHitInstigator = 16;
constexpr int kTargetInstigatorsPet = 8192;

// The event kinds (the byte of the binding the reaction listens with) and the bits this file serves.
constexpr int kKindHit = 0;
constexpr int kKindAbility = 2;
constexpr int kBitOwnerIsHit = 2;
constexpr int kBitOwnerHits = 4;
constexpr int kBitConsume = 4;

int binding_of(const content::ReactionRow& r, int kind) {
    switch (kind) {
        case kKindHit: return r.bind_hit;
        case kKindAbility: return r.bind_ability;
        default: return 0;
    }
}

/// The hit-taken (OwnerIsHit) reactions whose shape this file serves: the Cleric's Defiant Light, the Tank's Vengeance and Reflect, and the Ranger's Thorns.
/// The rest (Guard, Dodge, Camouflage, Stealth, the NPC shields and the Proc and Summoner ones) wait on rules this file does not serve yet (see the head).
bool hit_taken_reaction_served(const std::string& name) {
    return name.rfind("Cleric_", 0) == 0 || name.rfind("Tank_Vengeance_", 0) == 0 || name.rfind("Tank_Reflect_", 0) == 0 || name.rfind("Ranger_Thorns", 0) == 0;
}

}  // namespace

void AbilityPipeline::set_reaction_carriers(std::vector<int64_t> effect_guids) {
    reaction_carriers_ = std::move(effect_guids);
}

void AbilityPipeline::fire_carried_reactions(AbilityChange& c, const HitEvent& ev, double t) {
    if (!reactions_enabled_ || carried_depth_ >= 2 || !catalog_ || !ev.hit || ev.hit->reactions.empty() || (ev.targets.empty() && !ev.on_self)) return;
    ReactionContext rc;
    rc.hit = &ev;
    for (const int64_t guid : ev.hit->reactions) {
        const ReactionDef* r = catalog_->reaction(guid);
        if (!r) continue;
        const content::ReactionRow& row = r->row;
        if (row.trigger_expr != 0) {
            double v = 0;
            std::string why;
            if (!eval_in_event(row.trigger_expr, rc, v, why)) {
                if (reaction_noted_.insert(guid).second) spdlog::info("[abilities] the reaction {} stays inert: {}", row.name, why);
                continue;
            }
            if (v == 0) continue;
        }
        HitSource src;
        src.type = content::kSourceTypeReaction;
        src.guid = guid;
        src.actor = false;
        src.confirm = true;
        ++carried_depth_;
        const ReactionContext* const outer = active_reaction_;
        active_reaction_ = &rc;
        for (const int64_t hit_guid : row.hits) {
            const HitDef* hit = catalog_->hit(hit_guid);
            if (!hit) continue;
            if ((row.targets_mask & (kTargetOwner | kTargetInstigator)) != 0) apply_hit_here(c, *hit, t, true, src);   // (the caster's: its own mask speaks of whoever casts it)
            if ((row.targets_mask & kTargetTargets) != 0 && !ev.targets.empty()) apply_hit_on_targets(c, *hit, ev.targets, t, src, nullptr);
            if ((row.targets_mask & kTargetInstigatorsPet) != 0 && combat_) {
                if (const uint64_t pet = combat_->own_pet()) apply_hit_on_targets(c, *hit, {pet}, t, src, nullptr);
            }
        }
        active_reaction_ = outer;
        drop_evaluation();
        --carried_depth_;
        ++reactions_fired_;
        spdlog::log(reactions_fired_ <= 40 ? spdlog::level::info : spdlog::level::debug, "[abilities] the reaction {} of the hit {} fired", row.name, ev.hit->row.name);
    }
}

void AbilityPipeline::raise_hit_event(AbilityChange& c, const HitEvent& ev, double t) {
    fire_carried_reactions(c, ev, t);
    if (!reactions_enabled_ || reaction_depth_ > 0 || !catalog_ || catalog_->reactions() == 0 || !ev.hit) return;
    // a hit with no event tags and no stat mods raises no event [the game's code: the hit's record is tested for tags, mods and the force flag]
    if (ev.hit->tags.empty() && ev.hit->mods.empty()) return;
    // a hit a reaction made raises no reaction
    if (ev.src.type == content::kSourceTypeReaction) return;
    ReactionContext rc;
    rc.hit = &ev;
    run_reactions(c, kKindHit, kBitOwnerHits, rc, t);
}

void AbilityPipeline::raise_hit_taken(AbilityChange& c, const HitEvent& ev, double t) {
    if (!reactions_enabled_ || reaction_depth_ > 0 || !catalog_ || catalog_->reactions() == 0) return;
    ReactionContext rc;
    rc.hit = &ev;
    run_reactions(c, kKindHit, kBitOwnerIsHit, rc, t);
}

void AbilityPipeline::raise_ability_event(AbilityChange& c, const AbilityDef& ability, double t) {
    if (!reactions_enabled_ || reaction_depth_ > 0 || !catalog_ || catalog_->reactions() == 0) return;
    ReactionContext rc;
    rc.consumed = &ability;
    run_reactions(c, kKindAbility, kBitConsume, rc, t);
}

void AbilityPipeline::run_reactions(AbilityChange& c, int kind, int bit, const ReactionContext& rc, double t) {
    // the carriers in the order the character got them: the ranks' passives first, then what abilities and hits applied (a snapshot: a reaction's hits change
    // the effects)
    std::vector<int64_t> carriers = reaction_carriers_;
    for (const EffectInst& e : effects_) carriers.push_back(e.def->row.guid);
    std::vector<int64_t> seen;
    for (const int64_t effect_guid : carriers) {
        if (std::find(seen.begin(), seen.end(), effect_guid) != seen.end()) continue;
        seen.push_back(effect_guid);
        const EffectDef* carrier = catalog_->effect(effect_guid);
        if (!carrier || carrier->reactions.empty()) continue;
        for (const int64_t reaction_guid : carrier->reactions) {
            const ReactionDef* r = catalog_->reaction(reaction_guid);
            if (!r || (binding_of(r->row, kind) & bit) == 0) continue;
            if (kind == kKindHit && bit == kBitOwnerIsHit && !hit_taken_reaction_served(r->row.name)) continue;   // (see the file's head)
            const int limit = r->row.global_limit > 0 ? r->row.global_limit : r->row.instigator_limit;
            if (limit > 0 && reaction_procs_[reaction_guid] >= limit) continue;
            if (r->row.trigger_expr != 0) {
                double v = 0;
                std::string why;
                if (!eval_in_event(r->row.trigger_expr, rc, v, why)) {
                    if (reaction_noted_.insert(reaction_guid).second) spdlog::info("[abilities] the reaction {} stays inert: {}", r->row.name, why);
                    continue;
                }
                if (v == 0) continue;
            }
            fire_reaction(c, *r, effect_guid, rc, t);
            if (limit > 0 && reaction_procs_[reaction_guid] >= limit) {
                // a reaction that reached its limit takes its carrier off [the game's code: the effect goes with all its stacks]
                for (const EffectInst& e : effects_) {
                    if (e.def->row.guid == effect_guid) {
                        remove_stacks(c, effect_guid, 1 << 20);
                        break;
                    }
                }
            }
        }
    }
}

namespace {
// The melodies that help at a finisher: the reaction's hit fires, through a targeting record of 30 m, an AoE hit that fires a projectile whose hit does the work. The records rank the ones
// short of the stat (a heal: Health, Pensive: Mana) first and then the nearest; the Epic melody ranks those under crowd control, and takes up to the Resonance held [modelled: all of its
// candidates by distance, as the server does not read another player's effects here]. The projectile's flight is not waited for [modelled].
struct AllyFinisher {
    const char* hit;
    int64_t     stat, max;   // 0: no stat to rank by
};
constexpr AllyFinisher kAllyFinishers[] = {
    {"Bard_ResonantWeapon_End_Cheerful_RecurseAoE", content::kGuidHealth, content::kGuidMaxHealth},
    {"Bard_ResonantWeapon_End_Pensive_RecurseAoE", 109183576135288ll, 109183578756729ll},
    {"Bard_ResonantWeapon_End_Epic_RecurseAoE", 0, 0},
};
constexpr double kAllyFinisherRadius = 3000.0;
}  // namespace

bool AbilityPipeline::land_ally_finisher(AbilityChange& c, const HitDef& hit, const HitSource& src, double t) {
    const AllyFinisher* rule = nullptr;
    for (const AllyFinisher& f : kAllyFinishers) {
        if (hit.row.name == f.hit) rule = &f;
    }
    if (!rule) return false;
    for (const content::RecursiveHitRow& r : hit.recursive) {
        const HitDef* child = catalog_->hit(r.child_hit);
        if (!child) continue;
        struct Candidate {
            uint64_t actor;   // 0: the caster
            double   share, distance;
        };
        std::vector<Candidate> pool;
        const auto share_of = [&](double value, double max) { return max > 0 ? std::clamp(value / max, 0.0, 1.0) : 1.0; };
        if (hits_caster(child->row)) pool.push_back({0, rule->stat != 0 ? share_of(stat_now(rule->stat), stat_now(rule->max)) : 1.0, 0.0});
        if (combat_) {
            for (const combat::GatheredTarget& e : combat_->allies_around(combat_->self().position(), kAllyFinisherRadius, kMaxGathered)) {
                const combat::TargetInfo ti = combat_->inspect(e.actor);
                if (!ti.exists || !ti.alive || !combat::mask_reaches(child->row.applicable_targets, ti)) continue;
                double share = 1.0;
                if (combat::Fighter* f = combat_->fighter(e.actor); f && rule->stat != 0) {
                    std::string err;
                    if (auto run = f->stats().evaluation(f->worn(), err, true)) share = share_of(run->stat(rule->stat), run->stat(rule->max));
                }
                pool.push_back({e.actor, share, e.distance_cm});
            }
        }
        std::stable_sort(pool.begin(), pool.end(), [](const Candidate& a, const Candidate& b) {
            const bool short_a = a.share < 1.0, short_b = b.share < 1.0;
            if (short_a != short_b) return short_a;
            if (short_a && a.share != b.share) return a.share < b.share;
            return a.distance < b.distance;
        });
        double most = 1;
        std::string why;
        if (child->row.max_targets_expr != 0 && !eval(child->row.max_targets_expr, most, why)) most = 1;
        const size_t picks = std::min(pool.size(), static_cast<size_t>(std::clamp(most, 1.0, 40.0)));
        spdlog::info("[abilities] {}: {} of {} near it", hit.row.name, picks, pool.size());
        const auto land = [&](const HitDef& h, uint64_t actor, const HitSource& from) {
            if (actor == 0) apply_hit(c, h, t, true, from);
            else apply_hit_on_targets(c, h, {actor}, t, from, nullptr);
        };
        // the hit that fires the projectile names the Bard as its Source: the projectile's record spawns it at EAbilitySystemOrigin::Source, and a confirmation without one made the client drop
        // it ("SpawnProjectile(): Failed to find spawn transform for projectile", 25 times in the owner's client log of 4 November for Bard_ResonantWeapon_End_Cheerful_Projectile)
        HitSource from_bard = src;
        from_bard.actor = true;
        for (size_t i = 0; i < picks; ++i) {
            land(*child, pool[i].actor, from_bard);   // (its confirmation is what the client spawns the projectile from)
            for (const int64_t guid : child->fires) {
                const content::ProjectileRow* p = catalog_->projectile(guid);
                if (!p) continue;
                HitSource flown = src;
                flown.type = content::kSourceTypeProjectile;
                flown.guid = guid;
                for (const int64_t landed : p->hits) {
                    if (const HitDef* h = catalog_->hit(landed)) land(*h, pool[i].actor, flown);
                }
            }
        }
    }
    return true;
}

void AbilityPipeline::fire_reaction_for_test(int64_t reaction, double t) {
    advance(t);
    const ReactionDef* r = catalog_ ? catalog_->reaction(reaction) : nullptr;
    if (!r) return;
    AbilityChange c;
    const ReactionContext rc;
    fire_reaction(c, *r, 0, rc, t);
    recompute(c);
    emit({std::move(c), std::nullopt, t, "a reaction fires (a test)"});
}

void AbilityPipeline::fire_reaction(AbilityChange& c, const ReactionDef& r, int64_t carrier, const ReactionContext& rc, double t) {
    const content::ReactionRow& row = r.row;
    // who casts the hits: the holder, or whoever put the carrier on, which is the holder for a rank's passive and for what its own hits applied [R: Divine Power rises with the heals
    // and hits of the character that holds Cleric_DivinePower_Gen, by the hit's formula]; a creature that left it, the hit's instigator or its source need actors the world does not hold yet
    bool left_by_creature = false;
    for (const EffectInst& e : effects_) left_by_creature = left_by_creature || (e.def->row.guid == carrier && e.from_npc != 0);
    if (row.instigator != "Owner" && (row.instigator != "Instigator" || left_by_creature)) {
        if (reaction_noted_.insert(row.guid).second) spdlog::info("[abilities] the reaction {} is cast by {}, which is not served yet: it does not fire", row.name, row.instigator);
        return;
    }
    int served = kTargetOwner | kTargetInstigator | kTargetTargets | kTargetHitInstigator;
    // a hit-taken event also serves the hit's source (the creature that hit the holder); an owner's own hit has no such actor, so its HitSource stays unserved
    if (rc.hit && rc.hit->attacker != 0) served |= kTargetHitSource;
    if ((row.targets_mask & ~served) != 0 && reaction_noted_.insert(-row.guid).second) {
        spdlog::info("[abilities] the reaction {} lands on targets (mask {}) that are not all served yet: the holder and the hit's targets get its hits", row.name, row.targets_mask);
    }
    if ((row.targets_mask & served) == 0) return;
    ++reaction_procs_[row.guid];
    ++reactions_fired_;
    // the reaction's hits are the game's own records, applied under the reaction as their source: the client takes them as the server's (predicted on neither
    // side) and draws their numbers from the confirmations
    HitSource src;
    src.type = content::kSourceTypeReaction;
    src.guid = row.guid;
    src.actor = false;
    src.confirm = true;
    src.autonomous = false;
    src.simulated = false;
    ++reaction_depth_;
    const ReactionContext* const outer = active_reaction_;
    active_reaction_ = &rc;
    size_t applied = 0;
    for (const int64_t hit_guid : row.hits) {
        const HitDef* hit = catalog_->hit(hit_guid);
        if (!hit) continue;
        if (land_ally_finisher(c, *hit, src, t)) {
            ++applied;
            continue;
        }
        // what the reaction's hit fires on enemies: around each enemy it reached by the record's radius (a shuriken's explosion), else at that enemy again (a bounce's chance to bleed)
        if (combat_ && (row.targets_mask & kTargetTargets) != 0 && rc.hit && !rc.hit->targets.empty()) {
            for (const content::RecursiveHitRow& fired : hit->recursive) {
                const HitDef* child = catalog_->hit(fired.child_hit);
                if (!child || (child->row.applicable_targets & 2) == 0) continue;
                for (const uint64_t target : rc.hit->targets) {
                    std::vector<uint64_t> around;
                    if (fired.radius > 0) {
                        const combat::TargetInfo at = combat_->inspect(target);
                        if (!at.exists) continue;
                        for (const combat::GatheredTarget& e : combat_->enemies_around(at.position, fired.radius, kMaxGathered)) around.push_back(e.actor);
                    } else {
                        around.push_back(target);
                    }
                    if (!around.empty()) apply_hit_on_targets(c, *child, around, t, src, nullptr);
                }
            }
        }
        // the other hits it fires through a targeting record are not followed yet
        if (!hit->recursive.empty() && reaction_recursive_noted_.insert(row.guid).second) {
            spdlog::info("[abilities] the reaction {}: its hit {} fires {} further hit(s) through a targeting record, which are not followed yet: they do not land", row.name,
                         hit->row.name, hit->recursive.size());
        }
        if ((row.targets_mask & kTargetTargets) != 0 && rc.hit && !rc.hit->targets.empty()) {
            apply_hit_on_targets(c, *hit, rc.hit->targets, t, src, nullptr);
            ++applied;
        }
        if ((row.targets_mask & kTargetHitInstigator) != 0 && rc.hit && rc.hit->attacker != 0) {
            apply_hit_on_targets(c, *hit, {rc.hit->attacker}, t, src, nullptr);
            ++applied;
        }
        if ((row.targets_mask & kTargetHitSource) != 0 && rc.hit && rc.hit->attacker != 0) {
            apply_hit_on_targets(c, *hit, {rc.hit->attacker}, t, src, nullptr);
            ++applied;
        }
        if ((row.targets_mask & (kTargetOwner | kTargetInstigator)) != 0) {
            apply_hit(c, *hit, t, true, src);
            ++applied;
        }
    }
    active_reaction_ = outer;
    drop_evaluation();
    --reaction_depth_;
    spdlog::log(reactions_fired_ <= 40 ? spdlog::level::info : spdlog::level::debug, "[abilities] the reaction {} fired ({} of its {} hit(s) applied; event: {})", row.name, applied, row.hits.size(),
                rc.hit && rc.hit->hit ? "a hit, " + rc.hit->hit->row.name : rc.consumed ? "the cost of " + rc.consumed->row.name + " paid" : std::string("?"));
}

}  // namespace aoc::worldd
