// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  test_worldd_ability_npc.cpp: the ability runtime against what the retail server answered to every class ability that needs another actor, for every class
//  (plan E1, stage 3, part 2)
//
//  capture_npc_casts.h (tools/capture/gen_npc_casts.py) holds every ServerTriggerAbility of the 482 unique retail captures that stage 2's all-classes test
//  could not serve (gen_casts.py's stage NPC, the class abilities: the call names another actor, a hit request or confirmation names one, a pet or summon,
//  an enemy-only ability), with what the client sent while it ran and what the server answered, item by item with its time.  The calls of one capture are
//  replayed in order on one character of the capture's class and level of the real content.db on a simulated clock, each at its own time, in a world with
//  ground and ONE creature 3 m in front of the caster, made new for every cast (a fresh Wolf, or the Blueprint the capture's target was when the capture
//  opened it, at the caster's level, that does not come for the caster before it is hit).  For each cast:
//    - it must be ACCEPTED: an enemy ability is cast at the creature, an ability that may help allies is cast as retail's call was aimed (at the caster, an
//      ally being no actor of this world), the hit requests name the creature's collision cylinder where retail's named another actor's;
//    - what the caster's own side holds is compared as the all-classes test does it: the ability item (the ability, MontageGroup, the montage's NetGUID,
//      whether it has a target), the Cooldowns item, the Mana cost, the effects on the caster, the shields, an early end;
//    - the hit confirmations of the ability's hits retail sent must be sent, and the creature must have been hit where retail's confirmation had a target
//      (the confirmation's number of actors hit and the stat mods' record guids, which say what the hit does: damage, a heal, a debuff);
//    - the AMOUNTS are compared by their ratio to retail's: the creature's stats and the caster's gear and talents are not in the captures, so the numbers
//      are a distribution (the median and the quartiles are reported per class), not a pass or fail;
//    - the TIMES are counted apart as in the all-classes test.
//  What is not matched is a LISTED difference (the ability, the capture, what, why) and the list is exact: an unlisted difference fails the test.
//  The report counts the casts accepted and matched per class and what is left per reason.  Without a content.db the test says so and is skipped.
// ============================================================================
#include "session/summoner_pets.h"
#include "ability_rig.h"
#include "capture_npc_casts.h"
#include "combat/aftermath.h"
#include "combat/combat_data.h"
#include "combat/npc_stats.h"
#include "combat_rig.h"
#include "test_registry.h"
#include "npc/motion_table.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#ifndef NPC_MOTION_PATH
#define NPC_MOTION_PATH "dist/content/npc_motion.tsv"
#endif

using namespace ability_rig;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)
#define CHECKF(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { std::printf("  [FAIL] "); std::printf(__VA_ARGS__); std::printf("\n"); g_fail++; } \
} while (0)

namespace {

using NpcCase = capture_npc_casts::Case;

struct RItem {
    std::string              kind;
    int                      ms = 0;
    std::vector<std::string> f;
};

std::string join(const char* const* parts) {
    std::string out;
    for (; *parts; ++parts) out += *parts;
    return out;
}
// -- C:Y:B . C O A --

std::vector<RItem> parse_lines(const char* const* parts) {
    std::vector<RItem> out;
    std::istringstream in(join(parts));
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        RItem r;
        ls >> r.kind >> r.ms;
        for (std::string w; ls >> w;) r.f.push_back(w);
        out.push_back(std::move(r));
    }
    return out;
}

std::map<int64_t, double> parse_before(const char* const* parts) {
    std::map<int64_t, double> out;
    std::istringstream in(join(parts));
    long long rec;
    double v;
    while (in >> rec >> v) out[rec] = v;
    return out;
}

double   to_d(const std::string& s) { return std::strtod(s.c_str(), nullptr); }
int64_t  to_i(const std::string& s) { return std::strtoll(s.c_str(), nullptr, 10); }
uint64_t to_u(const std::string& s) { return std::strtoull(s.c_str(), nullptr, 10); }

int class_enum(const std::string& name) {
    static const char* kNames[] = {"", "Ranger", "Bard", "Cleric", "Fighter", "Rogue", "Summoner", "Tank", "Mage"};
    for (int i = 1; i <= 8; ++i) {
        if (name == kNames[i]) return i;
    }
    return 2;
}

constexpr double kItemTolerance = 0.060, kStepTolerance = 0.120, kEffectTolerance = 0.150;

// every effect the ability can put on its caster: its own, those of its hits, projectiles' and zones' hits, and what those effects fire; and every hit
void closure(const abilities::Catalog& cat, const abilities::AbilityDef& def, std::set<int64_t>& effects, std::set<int64_t>& hits) {
    std::vector<int64_t> todo;
    for (const auto& [key, e] : def.effects) effects.insert(e);
    for (const content::AbilityHitRow& h : def.hits) todo.push_back(h.hit_guid);
    for (const auto& [key, e] : def.effects) {
        if (const abilities::EffectDef* ed = cat.effect(e))
            for (const auto& [phase, hit] : ed->hits) todo.push_back(hit);
    }
    while (!todo.empty()) {
        const int64_t g = todo.back();
        todo.pop_back();
        if (!hits.insert(g).second) continue;
        const abilities::HitDef* h = cat.hit(g);
        if (!h) continue;
        for (const content::HitEffectRow& x : h->effects) {
            if (effects.insert(x.effect_guid).second) {
                if (const abilities::EffectDef* e = cat.effect(x.effect_guid)) {
                    for (const auto& [phase, hit] : e->hits) todo.push_back(hit);
                }
            }
        }
    }
}

/// The skills an expression asks GetSkillPurchased about: (class tree, skill reference) pairs as the text names them.
std::vector<stats::SkillRank> skills_in(const std::string& text) {
    static const std::regex re(R"(\$#297887639323616934:(\d+)\$\s*,\s*\$#(\d+):(\d+)\$)");
    std::vector<stats::SkillRank> out;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) {
        stats::SkillRank r;
        r.cls = {stats::kRefTypeSkillTree, std::stoll((*it)[1])};
        r.skill = {std::stoull((*it)[2]), std::stoll((*it)[3])};
        out.push_back(r);
    }
    return out;
}
// ~ CoA :: C Y B ~

/// The talents the capture's character held, as far as the cooldowns and charges its casts show (the all-classes test's inference): for each ability whose
/// cooldown or charge formula asks about skills, the smallest set of them under which the formulas give retail's InitialCooldown and at least as many charges
/// as retail's Cooldowns items show.
std::vector<stats::SkillRank> infer_talents(Rig& r, int capture) {
    std::vector<stats::SkillRank> extra;
    std::set<int64_t> done;
    for (const NpcCase& c : capture_npc_casts::kCases) {
        if (c.capture != capture || !done.insert(c.ability).second) continue;
        const abilities::AbilityDef* def = r.catalog->ability(c.ability);
        if (!def || r.rules.ability(c.ability)) continue;
        std::vector<stats::SkillRank> cand;
        for (const int64_t id : {def->row.cooldown_expr, def->row.charges_expr}) {
            if (const content::ExprRow* e = r.catalog->expr(id)) {
                for (const stats::SkillRank& s : skills_in(e->text)) {
                    bool dup = false;
                    for (const stats::SkillRank& x : cand) dup = dup || (x.skill == s.skill && x.cls == s.cls);
                    if (!dup) cand.push_back(s);
                }
            }
        }
        if (cand.empty() || cand.size() > 4) continue;
        double cooldown = -1;
        int most_left = -1;
        for (const NpcCase& d : capture_npc_casts::kCases) {
            if (d.capture != capture || d.ability != c.ability) continue;
            for (const RItem& it : parse_lines(d.reply)) {
                if (it.kind == "CD" && to_i(it.f[0]) == c.ability) {
                    cooldown = to_d(it.f[1]);
                    most_left = std::max(most_left, static_cast<int>(to_i(it.f[2])));
                }
            }
        }
        if (cooldown < 0) continue;
        std::vector<stats::SkillRank> best;
        bool found = false;
        for (unsigned size = 0; size <= cand.size() && !found; ++size) {
            for (unsigned mask = 0; mask < (1u << cand.size()) && !found; ++mask) {
                if (static_cast<unsigned>(std::popcount(mask)) != size) continue;
                std::vector<stats::SkillRank> trial = extra;
                for (size_t i = 0; i < cand.size(); ++i)
                    if (mask & (1u << i)) trial.push_back(cand[i]);
                std::vector<stats::SkillRank> all = r.base_ranks;
                all.insert(all.end(), trial.begin(), trial.end());
                r.cs->set_ranks(all);
                std::string err;
                auto ev = r.cs->evaluation({}, err);
                if (!ev) continue;
                stats::ExprHost host;
                double cd = 0, ch = 1;
                if (def->row.cooldown_expr != 0 && !ev->eval(r.catalog->program(def->row.cooldown_expr), host, cd, err)) continue;
                if (def->row.charges_expr != 0 && !ev->eval(r.catalog->program(def->row.charges_expr), host, ch, err)) continue;
                if (std::fabs(cd - cooldown) < 1e-3 && static_cast<int>(std::lround(ch)) >= most_left + 1) {
                    best = trial;
                    found = true;
                }
            }
        }
        if (found) extra = best;
    }
    return extra;
}

/// The state a character of a capture had from before the capture, as far as an ability's requirement names it: the effects it asks the caster (or its
/// owner) to have (a stealth, a stack, an enchant) and a summoned pet (the effect that carries the tag Pet.Summon: the pet itself, an NPC, is not made).
/// Granted to the caster before the cast whose requirement was false.
size_t grant_states(Rig& r, const abilities::AbilityDef& def, double t) {
    const content::ExprRow* e = r.catalog->expr(def.row.requirement_expr);
    if (!e) return 0;
    size_t granted = 0;
    static const std::regex re(R"(GetEffectCount\(Get(?:Instigator|Owner)\(\),\s*\$#\d+:(\d+)\$\))");
    for (auto it = std::sregex_iterator(e->text.begin(), e->text.end(), re); it != std::sregex_iterator(); ++it) {
        const int64_t guid = std::stoll((*it)[1]);
        if (r.catalog->effect(guid) && !r.pawn.effect(guid) && r.pipe->grant_effect(guid, 1, t)) ++granted;
    }
    // a class resource the requirement asks for (a Tank's Courage at 20): the capture's character had built it up in the fights before the capture
    static const std::regex stat_re(R"(GetStat\(Get(?:Instigator|Owner)\(\),\s*\$#\d+:(\d+)\$\)\)?\s*>=\s*([0-9.]+))");
    for (auto it = std::sregex_iterator(e->text.begin(), e->text.end(), stat_re); it != std::sregex_iterator(); ++it) {
        const int64_t stat = std::stoll((*it)[1]);
        const double need = std::stod((*it)[2]);
        const stats::StatValue* v = r.cs->find(stat);
        if (v && v->value < need) {
            r.set_current(stat, need);
            ++granted;
        }
    }
    std::string lower;
    for (const char ch : e->text) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (lower.find("pet.summon") != std::string::npos) {
        const char* pet = def.row.name.find("Mystic") != std::string::npos ? "Summoner_Pet_Mystic" : (def.row.name.find("Guardian") != std::string::npos ? "Summoner_Pet_Guardian" : "Summoner_Pet_Hunter");
        const abilities::EffectDef* ed = r.catalog->effect_named(pet);
        if (ed && !r.pawn.effect(ed->row.guid) && r.pipe->grant_effect(ed->row.guid, 1, t)) ++granted;
    }
    return granted;
}

// ---- the listed differences -------------------------------------------------------------------------------------------------------------------------

const char* const R_STATE = "the ability's activation requirement reads a state the capture's character had from before the capture or that a summoned pet, a stealth, a stack or an enchant gives (GetEffectCount, GetEffectCountByTag on the caster or the target): the replay starts each capture's character fresh and stage 3 has no pets";
const char* const R_CHARGES = "the capture's character had talents or gear that give the ability more charges or a shorter cooldown than its formula's base (the test infers only the talents the formulas name): the replay's charges run out where retail's did not";
const char* const R_MOD = "retail's Mana cost carries a talent or gear modifier the capture does not name (the level comes from the first cost, the talents from the cooldowns)";
const char* const R_SHIELD = "retail's reply lists a TempHealth shield the replay's pawn does not have: a shield whose amount follows the damage the hit did or the caster's gear and talents; not resolved";
const char* const R_REACT = "retail applies the effect on the caster through a reaction (an effect's or a hit's ReactionsIds: stored, not followed) or through a state the replay's character lacks (Vengeance, a Ranger's dash, Advantage, a combo's step); not run";
const char* const R_NPCFX = "an effect the hit puts on the creature (a damage over time, a snare, a debuff: Burning, Bleeding, Hemorrhage, Conflagration ...) is not run: stage 3 puts only shields on an NPC, so that effect's periodic hits and what depends on its stacks are not made";
const char* const R_AREA = "an area hit at a ground point or around a target that the replay does not reach (the client's aim point is not in the call, retail had more creatures around): the hit is not made or finds nobody here";
const char* const R_ALLY = "the hit needs an ally, a party member or a second creature that retail's cast reached and the replay (one creature, no ally) does not have";
const char* const R_CONFX = "our confirmation rule confirms a hit that retail did not confirm in this cast (the caster counts as an ally of its own heal, a second step of a chain, another number of creatures hit)";
const char* const R_CONE = "the cast finds its own targets in a cone ahead of the caster (the server gathers them: the trigger names none) and the replay's creature stands in it, where retail's cast found nobody in front (it confirmed the ground arrows only) or was held until the charge was full (the talent Concentrated, which the replay's character lacks: the charged shot hit)";
const char* const R_GROUND = "the arrow's own hit names the piece of ground it struck as its target in retail (a world component, not an actor); ours confirms it on the caster, so the replay sees no actor hit";
const char* const R_ARROWS = "the cast's other projectiles (arcing arrows that only show where they land, their hit has no stat mod) are not made";
const char* const R_CHAN = "a channelled, toggled or chained ability that the client re-triggers or the server ends early in retail; the early end is not matched";
const char* const R_UNRES = "not resolved in stage 3";
const char* const R_KILL = "the replay's creature (a level-1 Wolf, 34.9 Health, where the capture's target is another actor of unknown level) dies of the ability's first hit since the E1 review's stat fix gave a level-1 creature retail's MaxHealth (a fifth less): the hits after the kill are not confirmed; retail's target survived";

const char* const R_JITTER = "the cost step comes the median of the ability's recorded offsets after the montage's jump (Mortal Blast: 199 ms); in this capture it came sooner (165 ms), 5 ms before the next call ended the replay's window for the cast, so the step lands 34 ms after that window and in the next cast's window (its Mana, its Cooldowns item)";

struct Known {
    const char* ability;
    int         capture;
    const char* tag;
    const char* why;
};
#include "ability_npc_known.inc"

#include "ability_npc_registered.inc"

}  // namespace

int main(int argc, char** argv) {
    if (!real_db_present()) {
        std::printf("no content.db at %s (cd services && go run ./cmd/contentc): the NPC casts' acceptance test is skipped\n", real_db_path().c_str());
        return 0;
    }
    bool verbose = false, use_known = true;   // (--no-known: the listed differences are not applied: the run that writes ability_npc_known.inc)
    std::string only_class, only_ability;
    int only_capture = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-v")) verbose = true;
        else if (!std::strcmp(argv[i], "--no-known")) use_known = false;
        else if (!std::strcmp(argv[i], "--class") && i + 1 < argc) only_class = argv[++i];
        else if (!std::strcmp(argv[i], "--ability") && i + 1 < argc) only_ability = argv[++i];
        else if (!std::strcmp(argv[i], "--capture") && i + 1 < argc) only_capture = std::atoi(argv[++i]);
    }
    spdlog::set_level(spdlog::level::warn);
    const size_t n_cases = sizeof(capture_npc_casts::kCases) / sizeof(capture_npc_casts::kCases[0]);
    std::printf("the ability runtime against %zu retail casts of every class that need another actor\n", n_cases);
    std::set<int> captures;
    for (const auto& c : capture_npc_casts::kCases) captures.insert(c.capture);

    // the data every capture's world shares
    std::string error;
    npc::MotionTable motion;
    if (!motion.load(NPC_MOTION_PATH, error)) {
        std::printf("  [FAIL] npc_motion.tsv: %s\n", error.c_str());
        return 2;
    }
    std::string wolf_path;
    for (const auto& [p, row] : motion.rows()) {
        if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) wolf_path = p;
    }

    struct Tally {
        size_t total = 0, accepted = 0, refused_listed = 0, matched = 0, exact = 0, timed = 0, with_ts = 0, hit_cases = 0, hit_landed = 0, mods_same = 0, mods_diff = 0;
        std::vector<double> ratios;   // our damage over retail's, a hit that landed with the same stat mods
    };
    std::map<std::string, Tally> tally;
    std::map<std::string, size_t> reasons;
    std::map<std::string, std::vector<std::string>> reason_cases;
    std::map<std::string, size_t> kinds;   // target_kind of the cases that were run
    size_t at_npc = 0, at_air = 0, at_self = 0, states_granted = 0;
    size_t vengeance_shields = 0, vengeance_wrong = 0, vengeance_relentless = 0;
// -- C:Y:B & CoA --

    for (const int capture : captures) {
        std::string rig_cls;
        int level = 0;
        std::vector<int64_t> known;
        for (const auto& c : capture_npc_casts::kCases) {
            if (c.capture != capture) continue;
            rig_cls = c.rig_cls;
            level = std::max(level, c.level);
            known.push_back(c.ability);
        }
        if (level == 0) level = 25;
        if (!only_class.empty() && rig_cls != only_class) continue;
        if (only_capture != 0 && only_capture != capture) continue;
        if (!only_ability.empty()) {
            bool has = false;
            for (const auto& c : capture_npc_casts::kCases) has = has || (c.capture == capture && only_ability == c.name);
            if (!has) continue;
        }
        Rig r(class_enum(rig_cls), level);
        if (!r.ok()) {
            std::printf("  [FAIL] the rig of capture %d (%s %d): %s\n", capture, rig_cls.c_str(), level, r.problem.c_str());
            ++g_fail;
            continue;
        }
        r.pipe->set_known(known);
        r.pipe->set_data_timings(true);
        r.take();
        std::vector<stats::SkillRank> extra_ranks = infer_talents(r, capture);
        // one Tank capture's caster holds Relentless Vengeance, which no recording's did: Vengeance's shield is half as large again
        const bool relentless = capture == 527;
        if (relentless) {
            stats::SkillRank rank;
            rank.cls = {297887639323616934ull, 107281874310830ll};
            rank.skill = {4146438674247114715ull, 6064630689222295554ll};
            extra_ranks.push_back(rank);
        }
        r.use_extra_ranks(extra_ranks);

        // the world: ground, the caster, one creature at a time
        const auto rows = r.db->combat_rows(error);
        std::optional<combat::CombatData> data;
        if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
        if (!data) {
            std::printf("  [FAIL] the combat data of capture %d: %s\n", capture, error.c_str());
            ++g_fail;
            continue;
        }
        combat_rig::World w;
        if (!w.problem.empty()) {
            std::printf("  [FAIL] the world of capture %d: %s\n", capture, w.problem.c_str());
            ++g_fail;
            continue;
        }
        combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
        combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, motion);
        combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
        cw.add_player(&player);
        combat::WorldHitPort port(cw, w.registry, player.fighter());
        r.pipe->set_combat(&port);

        double world_now = 0;
        const auto advance = [&](double t) {
            while (world_now < t - 1e-9) {
                world_now = std::min(t, world_now + 0.1);
                w.npcs->tick(world_now);
                cw.tick(world_now);
                r.run_to(world_now);
            }
            if (t < world_now) r.run_to(t);   // (a request's own time, inside a step: the runtime keeps its own clock)
        };

        double clock_no_ts = 10.0;
        // the capture's casts in the order they happened (the header is sorted by class)
        std::vector<const NpcCase*> in_order;
        for (const NpcCase& x : capture_npc_casts::kCases) {
            if (x.capture == capture) in_order.push_back(&x);
        }
        std::stable_sort(in_order.begin(), in_order.end(), [](const NpcCase* a, const NpcCase* b) { return a->line < b->line; });
        uint64_t creature = 0;
        for (const NpcCase* cp : in_order) {
            const NpcCase& c = *cp;
            if (!only_ability.empty() && only_ability != c.name) continue;
            Tally& ty = tally[c.cls];
            ++ty.total;
            ++kinds[c.target_kind];
            const bool has_ts = c.at_ms >= 0;
            if (has_ts) ++ty.with_ts;
            const std::vector<RItem> reply = parse_lines(c.reply);
            const std::vector<RItem> client = parse_lines(c.client);
            const std::map<int64_t, double> before = parse_before(c.before);
            std::vector<std::string> problems, late;
            auto problem = [&](const std::string& p) { problems.push_back(p); };

            const double t0 = has_ts ? 5.0 + c.at_ms / 1000.0 + 0.0123 : (clock_no_ts += 15.0);
            // a new creature for every cast, of the kind the capture's target was when the capture knew it
            if (creature) {
                w.registry.remove(creature);
                cw.forget(creature);
            }
            std::string path = wolf_path;
            if (std::strlen(c.archetype) > 0 && motion.rows().count(c.archetype)) path = c.archetype;
            const int npc_level = std::max(1, c.level);
            // 3 m in front of the caster, or as far as the ability's own requirement asks (a Rogue's Sleep Dart needs 3 m, its Death's Mark 7.5 m)
            double distance_cm = 300;
            if (const abilities::AbilityDef* d0 = r.catalog->ability(c.ability)) {
                if (const content::ExprRow* e = r.catalog->expr(d0->row.requirement_expr)) {
                    static const std::regex re(R"(GetDistanceBetween\(GetInstigator\(\), GetTarget\(\)\)\s*>=\s*([0-9.]+))");
                    for (auto it = std::sregex_iterator(e->text.begin(), e->text.end(), re); it != std::sregex_iterator(); ++it) distance_cm = std::max(distance_cm, std::stod((*it)[1]) + 150.0);
                    distance_cm = std::min(distance_cm, 1200.0);
                }
            }
            creature = w.spawn(path, distance_cm, 0, npc_level, 1.0);
            if (!creature) {
                std::printf("  [FAIL] no creature for %s (capture %d)\n", c.name, capture);
                ++g_fail;
                continue;
            }
            advance(t0 - 0.2);
            r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
            r.set_current(content::kGuidHealth, r.pawn.stat(content::kGuidMaxHealth));
            r.take();
            for (const int64_t guid : content::kResourceStats) {
                const auto b = before.find(guid);
                if (b != before.end()) r.set_current(guid, b->second);
            }
            r.take();
            advance(t0 - 0.001);
            const double mana_before = r.pawn.stat(content::kGuidMana);
            const size_t first_event = r.log.size();
            const abilities::AbilityDef* def = r.catalog->ability(c.ability);
            // who the cast is aimed at: an ability that may harm is cast at the creature when retail's call named a target (or any other actor), one that
            // only helps (no Enemy in its mask) at the caster, as retail's call at an ally is replayed
            const bool enemy_ok = def && (def->row.applicable_targets & 2) != 0;
            const bool ally_ok = def && (def->row.applicable_targets & (1 | 4 | 8 | 16)) != 0;
            const bool names_target = c.targets > 0;
            const bool aim_at_creature = names_target && enemy_ok && (std::strcmp(c.target_kind, "npc") == 0 || !ally_ok);
            TriggerInfo info;
            info.animation_index = c.animation_index;
            info.target_count = static_cast<uint32_t>(c.targets);
            info.item = false;
            if (aim_at_creature) info.target_actors = {creature};
            (aim_at_creature ? at_npc : (names_target ? at_self : at_air)) += 1;
            int32_t trigger_id = c.client_instance_id;
            Refusal refusal = r.pipe->trigger(c.ability, trigger_id, t0, info);
            r.take();
            if (refusal == Refusal::Requirement && def && grant_states(r, *def, t0) > 0) {
                // [modelled] the capture's character had the state the requirement names from before the capture: it is given and the call is made again
                r.pipe->refresh(t0, "the state before the capture", true);
                r.take();
                ++states_granted;
                trigger_id = c.client_instance_id + 500000;   // (the refused call's id is still the runtime's)
                refusal = r.pipe->trigger(c.ability, trigger_id, t0 + 0.0005, info);
                r.take();
            }
            // what the client sent while the cast ran
            double end_t = t0 + (c.window_ms > 0 ? std::max(c.window_ms / 1000.0, 0.8) : 6.0);
            std::vector<std::pair<double, const RItem*>> sends;
            for (const RItem& q : client) sends.emplace_back(has_ts ? t0 + q.ms / 1000.0 : t0 + 0.3, &q);
            std::stable_sort(sends.begin(), sends.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [t, q] : sends) {
                if (refusal != Refusal::None) break;
                if (q->kind == "REQ") {
                    advance(t);
                    r.clock = std::max(r.clock, t);
                    r.a7->tick(t);
                    const size_t components = static_cast<size_t>(to_i(q->f[3])), own = static_cast<size_t>(to_i(q->f[4]));
                    const abilities::HitDef* hd = r.catalog->hit(to_i(q->f[2]));
                    // a component that is not the caster's names another actor: the creature when the hit can harm, and the caster itself (an ally being no actor here) otherwise
                    const bool harms = hd && (hd->row.applicable_targets & 2) != 0;
                    std::vector<uint64_t> targets;
                    size_t own_used = own;
                    if (components > own) {
                        if (harms) targets = {creature};
                        else own_used = components;
                    }
                    const auto verdict = r.pipe->hit_request(to_i(q->f[0]), to_i(q->f[1]), to_i(q->f[2]), components, own_used, t, targets);
                    if (verbose) std::printf("      request at %.3f: %s\n", t - t0, AbilityPipeline::verdict_name(verdict));
                    r.take();
                } else if (q->kind == "ENDREQ") {
                    advance(t);
                    int32_t sid = -1;
                    for (const auto& [k, v] : r.pawn.abilities)
                        if (v.ability_guid == c.ability) sid = static_cast<int32_t>(k);
                    if (sid >= 0) r.pipe->end_request(sid, t);
                    r.take();
                } else if (q->kind == "HELD") {
                    advance(t);
                    r.pipe->cancel(trigger_id, t);
                    r.take();
                } else if (q->kind == "BRANCH") {   // the client's cast bar ended: it asks for its branch (a cast bar's cost and hits follow this request)
                    advance(t);
                    r.pipe->branch_request(trigger_id, to_i(q->f[0]), t);
                    r.take();
                }
            }
            advance(end_t);
            if (refusal != Refusal::None) problem(std::string("refused:") + refusal_name(refusal));
            else ++ty.accepted;
            if (!r.pawn.broken.empty()) problem("pawn:" + r.pawn.broken);
// built with Cyb and C | O | A

            if (refusal == Refusal::None) {
                std::vector<const Seen*> events;
                for (size_t i = first_event; i < r.log.size(); ++i) events.push_back(&r.log[i]);
                std::set<int64_t> reach_effects, reach_hits;
                if (def) closure(*r.catalog, *def, reach_effects, reach_hits);
                // the ability item
                const bootstrap::ActiveAbilityItem* my_item = nullptr;
                double my_item_at = 0;
                for (const Seen* e : events) {
                    if (!my_item && !e->change.abilities_added.empty()) {
                        my_item = &e->change.abilities_added[0];
                        my_item_at = e->at - t0;
                    }
                }
                for (const RItem& it : reply) {
                    if (it.kind != "AB" || to_i(it.f[0]) != c.ability || to_u(it.f[3]) == 0) continue;
                    if (!my_item) {
                        problem("AB:missing");
                        break;
                    }
                    if (my_item->ability_guid != c.ability || my_item->montage_group != it.f[2]) problem("AB:group " + it.f[2] + " (" + my_item->montage_group + ")");
                    if (my_item->montage_guid != to_u(it.f[3])) problem("AB:montage");
                    const bool mine_has_target = my_item->targets_self || !my_item->target_objects.empty();
                    if (mine_has_target != (to_i(it.f[5]) > 0)) problem("AB:targets");
                    if (has_ts && std::fabs(my_item_at - it.ms / 1000.0) > kItemTolerance) late.push_back("AB " + std::to_string(static_cast<int>(my_item_at * 1000)) + "/" + std::to_string(it.ms));
                    break;
                }
                // the Cooldowns item
                const bootstrap::CooldownItem* my_cd = nullptr;
                double my_cd_at = -1;
                for (const Seen* e : events) {
                    for (const auto& cd : e->change.cooldowns_set) {
                        if (!my_cd && cd.ability_guid == c.ability) {
                            my_cd = &cd;
                            my_cd_at = e->at - t0;
                        }
                    }
                }
                for (const RItem& it : reply) {
                    if (it.kind != "CD" || to_i(it.f[0]) != c.ability) continue;
                    if (!my_cd) {
                        problem("CD:missing");
                        break;
                    }
                    if (my_cd->initial_cooldown != static_cast<float>(to_d(it.f[1]))) problem("CD:cooldown " + it.f[1]);
                    if (my_cd->charges != to_i(it.f[2])) problem("CD:charges " + it.f[2] + " (" + std::to_string(my_cd->charges) + ")");
                    if (has_ts && std::fabs(my_cd_at - it.ms / 1000.0) > kStepTolerance) late.push_back("CD " + std::to_string(static_cast<int>(my_cd_at * 1000)) + "/" + std::to_string(it.ms));
                    break;
                }
                // the Mana cost
                {
                    int cd_ms = -1;
                    for (const RItem& it : reply)
                        if (it.kind == "CD" && to_i(it.f[0]) == c.ability) cd_ms = it.ms;
                    double retail_before = before.count(content::kGuidMana) ? before.at(content::kGuidMana) : -1;
                    const RItem* step = nullptr;
                    for (const RItem& it : reply) {
                        if (it.kind != "STAT" || to_i(it.f[1]) != content::kGuidMana) continue;
                        if (cd_ms >= 0 && it.ms == cd_ms) {
                            step = &it;
                            break;
                        }
                        if (cd_ms < 0 || it.ms < cd_ms) retail_before = to_d(it.f[2]);
                    }
                    if (step && retail_before >= 0) {
                        const double retail_delta = retail_before - to_d(step->f[2]);
                        double my_after = mana_before;
                        for (const Seen* e : events) {
                            if (e->change.cooldowns_set.empty()) continue;
                            for (const auto& s : e->change.stats)
                                if (s.record == content::kGuidMana) my_after = s.value;
                            break;
                        }
                        if (retail_delta > 1 && std::fabs((mana_before - my_after) - retail_delta) > 1e-3)
                            problem("COST:mana " + std::to_string(retail_delta) + " (" + std::to_string(mana_before - my_after) + ")");
                    }
                }
                // the effects on the caster
                std::set<int64_t> first_seen;
                for (const RItem& it : reply) {
                    if (it.kind != "EFF") continue;
                    const int64_t guid = to_i(it.f[0]);
                    if (!reach_effects.count(guid) || !first_seen.insert(guid).second) continue;
                    const abilities::EffectDef* ed = r.catalog->effect(guid);
                    const std::string name = ed ? ed->row.name : std::to_string(guid);
                    bool produced = false, same = false, timely = false;
                    for (const Seen* e : events) {
                        for (const auto* list : {&e->change.effects_added, &e->change.effects_changed}) {
                            for (const auto& x : *list) {
                                if (x.effect_guid != guid) continue;
                                produced = true;
                                const bool inf = to_d(it.f[1]) > 1e30;
                                const bool extends = ed && ed->row.stack_duration_refresh.find("ExtendDuration") != std::string::npos;
                                const bool same_here = (inf ? x.duration > 1e30f : std::fabs(x.duration - to_d(it.f[1])) < (extends ? 0.2 : 1e-3)) &&
                                                       std::fabs(x.tick_time - to_d(it.f[2])) < 1e-4 && x.stacks == to_i(it.f[3]);
                                if (same_here) {
                                    same = true;
                                    if (!has_ts || std::fabs((e->at - t0) * 1000 - it.ms) <= kEffectTolerance * 1000) timely = true;
                                }
                            }
                        }
                    }
                    if (!produced) problem("EFF:missing " + name);
                    else if (!same) problem("EFF:differs " + name + " " + it.f[1] + "/" + it.f[2] + "/" + it.f[3]);
                    else if (!timely) late.push_back("EFF " + name);
                }
                // the confirmations: the records, then the creature's side of each (was it hit, with which stat mods, how much)
                std::map<int64_t, int> retail_conf, my_conf;
                std::map<int64_t, const RItem*> retail_first;
                for (const RItem& it : reply) {
                    if (it.kind != "CONF") continue;
                    ++retail_conf[to_i(it.f[1])];
                    if (!retail_first.count(to_i(it.f[1]))) retail_first[to_i(it.f[1])] = &it;
                    else if (to_i(it.f[6]) > 0 && to_i(retail_first[to_i(it.f[1])]->f[6]) == 0) retail_first[to_i(it.f[1])] = &it;
                }
                std::map<int64_t, const bootstrap::HitConfirm*> my_first;
                for (const Seen* e : events) {
                    for (const auto& h : e->confirmations) {
                        if (!reach_hits.count(h.record_guid)) continue;
                        ++my_conf[h.record_guid];
                        if (!my_first.count(h.record_guid) || (!h.targets.empty() && my_first[h.record_guid]->targets.empty())) my_first[h.record_guid] = &h;
                    }
                }
                for (const auto& [rec, n] : retail_conf) {
                    if (!reach_hits.count(rec)) continue;
                    const int mine = my_conf.count(rec) ? my_conf[rec] : 0;
                    const abilities::HitDef* hd = r.catalog->hit(rec);
                    const std::string hn = hd ? hd->row.name : std::to_string(rec);
                    if (mine == 0) problem("CONF:missing " + hn);
                    else if (mine < n && n <= 3) problem("CONF:fewer " + hn + " " + std::to_string(mine) + "/" + std::to_string(n));
                    // the creature's side
                    const RItem* rf = retail_first.at(rec);
                    const long retail_targets = static_cast<long>(to_i(rf->f[6]));
                    if (retail_targets > 0 && mine > 0) {
                        const bootstrap::HitConfirm* mf = my_first[rec];
                        ++ty.hit_cases;
                        if (mf->targets.empty()) {
                            // our confirmation lists no actor hit: a hit that needs somebody else (an ally, a second enemy) found nobody here
                            problem("HIT:nobody " + hn);
                            continue;
                        }
                        ++ty.hit_landed;
                        std::multiset<int64_t> retail_mods, my_mods;
                        double retail_sum = 0, my_sum = 0;
                        for (size_t k = 7; k < rf->f.size(); ++k) {
                            const std::string& m = rf->f[k];
                            const size_t a = m.find(':'), b = m.find(':', a + 1);
                            retail_mods.insert(std::stoll(m.substr(0, a)));
                            retail_sum += std::bit_cast<float>(static_cast<uint32_t>(std::stoul(m.substr(a + 1, b - a - 1), nullptr, 16)));
                        }
                        for (const auto& tg : mf->targets) {
                            for (const auto& m : tg.mods) {
                                my_mods.insert(m.stat_mod_guid);
                                my_sum += m.value;
                            }
                        }
                        // the stat mods of the hit that retail's confirmation lists once per actor hit: the set of records is what matters
                        const std::set<int64_t> rs(retail_mods.begin(), retail_mods.end()), ms(my_mods.begin(), my_mods.end());
                        if (rs.empty() || ms.empty()) {
                            // a miss or an immune target on either side: no amount to compare
                        } else if (rs == ms) {
                            ++ty.mods_same;
                            if (retail_sum != 0 && my_sum != 0 && retail_mods.size() == my_mods.size() && my_sum / retail_sum > 0) ty.ratios.push_back(my_sum / retail_sum);
                        } else {
                            ++ty.mods_diff;
                            problem("HIT:mods " + hn);
                        }
                    }
                }
                for (const auto& [rec, n] : my_conf) {
                    if (!retail_conf.count(rec)) {
                        const abilities::HitDef* hd = r.catalog->hit(rec);
                        problem("CONF:extra " + (hd ? hd->row.name : std::to_string(rec)));
                    }
                }
                // the shields: the count of the last TempHealth item
                {
                    int retail_n = -1;
                    for (const RItem& it : reply)
                        if (it.kind == "SHIELD") retail_n = static_cast<int>(to_i(it.f[0]));
                    if (retail_n > 0 && r.pawn.shields.empty()) problem("SHIELD:missing");
                    // Vengeance's shield is 2% of the caster's MaxHealth, 3% with Relentless Vengeance (the data's formula and the ability's text)
                    if (std::strcmp(c.name, "Tank_Vengeance") == 0 && !r.pawn.shields.empty()) {
                        const double share = r.pawn.shields.back().base / r.pawn.stat(content::kGuidMaxHealth);
                        ++vengeance_shields;
                        if (std::fabs(share - (relentless ? 0.03 : 0.02)) > 1e-3) ++vengeance_wrong;
                        if (relentless) ++vengeance_relentless;
                    }
                }
                // a cast retail ended early
                for (const RItem& it : reply) {
                    if (it.kind == "END") {
                        bool mine = false;
                        for (const Seen* e : events) mine = mine || (e->end && *e->end == to_i(it.f[0]));
                        if (!mine) problem("END:missing");
                        break;
                    }
                }
            }
            // the listed differences are no error
            std::vector<std::string> unlisted;
            for (const std::string& p : problems) {
                bool listed = false;
                for (const Known& k : kKnown) listed = listed || (use_known && std::string(k.ability) == c.name && k.capture == c.capture && p.rfind(k.tag, 0) == 0);
                if (!listed) unlisted.push_back(p);
            }
            if (refusal != Refusal::None && unlisted.empty()) ++ty.refused_listed;
            if (unlisted.empty()) ++ty.matched;
            if (problems.empty()) ++ty.exact;
            if (unlisted.empty() && late.empty()) ++ty.timed;
            for (const std::string& p : unlisted) {
                const size_t sp = p.find(' ');
                const std::string head = sp == std::string::npos ? p : p.substr(0, sp);
                ++reasons[head + " [" + c.cls + "]"];
                reason_cases[head + " [" + c.cls + "]"].push_back(std::string(c.name) + "@" + std::to_string(c.capture) + ":" + std::to_string(c.line) + " " + p);
            }
            if (verbose || (!unlisted.empty() && !only_ability.empty())) {
                std::printf("%s %s cap %d line %d (%s): %s  [the creature: level %d, Health %.1f of %.1f after the cast]", unlisted.empty() ? "ok  " : "FAIL", c.name, c.capture, c.line,
                            c.target_kind, unlisted.empty() ? "matches" : "", npc_level, cw.health_of(creature), cw.max_health_of(creature));
                for (const std::string& p : unlisted) std::printf("\n      %s", p.c_str());
                for (const std::string& p : late) std::printf("\n      late %s", p.c_str());
                std::printf("\n");
            }
        }
    }
    std::printf("Vengeance: %zu shields, %zu of them at 3%% (the casters with Relentless Vengeance)\n", vengeance_shields, vengeance_relentless);
    if (only_class.empty() || only_class == "Tank") {
        CHECK(vengeance_shields > vengeance_relentless && vengeance_relentless > 0 && vengeance_wrong == 0, "Vengeance gives its caster a shield of 2% of MaxHealth, 3% with Relentless Vengeance");
    }
    // ---- a Summoner's pets: each kind chases a creature and its attacks take the creature's Health, to the death, as its owner's hits
    if (only_ability.empty() && only_capture == 0 && (only_class.empty() || only_class == "Summoner")) {
        std::printf("what a hit leaves on a creature\n");
        {
            Rig r(class_enum("Summoner"), 25);
            const auto rows = r.db->combat_rows(error);
            std::optional<combat::CombatData> data;
            if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
            combat_rig::World w;
            CHECK(r.ok() && data && w.problem.empty(), "(the world of the effects)");
            if (r.ok() && data && w.problem.empty()) {
                combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
                combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, motion);
                combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
                cw.add_player(&player);
                const auto effect_named = [&](const char* name) {
                    return r.catalog->effect_named(name);
                };
                const auto landed = [](const abilities::EffectDef* def, double seconds) {
                    combat::LandedEffect e;
                    e.def = def;
                    e.seconds = seconds;
                    return e;
                };
                const auto run = [&](double seconds, double& now) {
                    for (double t = 0; t < seconds; t += 0.1) {
                        now += 0.1;
                        w.npcs->note_player(77, 0, 0, now);
                        w.npcs->tick(now);
                        cw.tick(now);
                    }
                };
                const auto effects_of_actor = [&](uint64_t id) {
                    size_t count = 0;
                    if (const auto* a = w.registry.find(id)) {
                        for (const auto& s : a->actor.subobjects) {
                            if (!s.obj.schema || std::string(s.obj.schema->name) != "UAoCStatsComponent") continue;
                            const auto* field = s.obj.schema->field("CurrentEffects");
                            if (const auto* arr = field ? s.obj.arrays.find(field->index) : nullptr) count = arr->items().size();
                        }
                    }
                    return count;
                };
                const auto crowd_animation = [&](uint64_t id) -> int64_t {
                    if (const auto* a = w.registry.find(id)) {
                        for (const auto& s : a->actor.subobjects) {
                            if (!s.obj.schema || std::string(s.obj.schema->name) != "UAoCStatsComponent") continue;
                            const auto* v = s.obj.state.get(*s.obj.schema, "CrowdControlState.CurrentAnimationRequest");
                            const uint64_t* n = v ? v->get_if<uint64_t>() : nullptr;
                            return n ? static_cast<int64_t>(*n) : -1;
                        }
                    }
                    return -1;
                };
                double now = 1.0;
                const auto* stun = effect_named("Status_Stunned");
                const auto* root = effect_named("Status_Rooted");
                const auto* snare = effect_named("Status_Snared");
                const auto* incap = effect_named("Status_Incapacitated");
                const auto* volatile_ = effect_named("Status_Volatile");
                CHECK(stun && root && snare && incap && volatile_, "(the effects are in the data)");
                if (stun && root && snare && incap && volatile_) {
                    // a stun: it stops everything for its time, shows on the creature, and the fight goes on after
                    const uint64_t wolf = w.spawn(wolf_path, 900, 0, 25, 1.0);
                    combat::Resolved poke;
                    poke.threat = 10;
                    cw.apply_hit_to_npc(wolf, 77, poke, now);
                    run(1.0, now);
                    cw.apply_effect_to_npc(wolf, landed(stun, 3.0), 77, now);
                    CHECK(cw.npc_effect_count(wolf, stun->row.guid) == 1 && effects_of_actor(wolf) == 1, "a stun lands and shows in CurrentEffects");
                    CHECK((cw.crowd_status(wolf) & combat::kCrowdStopsActing) != 0 && (cw.crowd_status(wolf) & combat::kCrowdStopsMoving) != 0, "a stun stops acting and moving");
                    CHECK(crowd_animation(wolf) == 0, "a stun asks the client for ECrowdControlAnimationRequest::Stunned, 0");
                    const auto* before = w.registry.find(wolf);
                    const double x = before->x, y = before->y;
                    const size_t attacks = cw.stats().npc_attacks;
                    run(2.5, now);
                    const auto* during = w.registry.find(wolf);
                    CHECK(std::hypot(during->x - x, during->y - y) < 1.0, "a stunned creature does not move");
                    CHECK(cw.stats().npc_attacks == attacks, "a stunned creature does not attack");
                    run(1.0, now);
                    CHECK(cw.npc_effect_count(wolf, stun->row.guid) == 0 && (cw.crowd_status(wolf) & combat::kCrowdStopsActing) == 0, "the stun ends with its time");
                    CHECK(crowd_animation(wolf) == combat::kCrowdAnimationNone, "with the stun over the client is asked for ECrowdControlAnimationRequest::None, 255");
                    run(3.0, now);
                    const auto* after = w.registry.find(wolf);
                    CHECK(cw.stats().npc_attacks > attacks || std::hypot(after->x - x, after->y - y) > 10.0, "after the stun the creature fights on");
                    // a slow: the record's strength
                    cw.apply_effect_to_npc(wolf, landed(snare, 3.0), 77, now);
                    CHECK(std::abs(cw.crowd_pace(wolf) - 0.75) < 1e-6, "Status_Snared takes a quarter of its speed");
                    run(3.5, now);
                    CHECK(std::abs(cw.crowd_pace(wolf) - 1.0) < 1e-6, "its speed comes back");
                    // a root: it stands, and can still act
                    cw.apply_effect_to_npc(wolf, landed(root, 2.0), 77, now);
                    CHECK(cw.crowd_status(wolf) == combat::kCrowdStopsMoving, "a root stops moving and nothing else");
                    const auto* rooted = w.registry.find(wolf);
                    const double rx = rooted->x, ry = rooted->y;
                    run(1.5, now);
                    const auto* still = w.registry.find(wolf);
                    CHECK(std::hypot(still->x - rx, still->y - ry) < 1.0, "a rooted creature stays where it is");
                    run(1.0, now);
                    // an incapacitate: any damage breaks it, a stun does not break
                    combat::Resolved hurt;
                    hurt.mods.push_back({0, content::kGuidHealth, -5.0});
                    cw.apply_effect_to_npc(wolf, landed(incap, 10.0), 77, now);
                    cw.apply_effect_to_npc(wolf, landed(stun, 3.0), 77, now);
                    cw.apply_hit_to_npc(wolf, 77, hurt, now);
                    CHECK(cw.npc_effect_count(wolf, incap->row.guid) == 0, "damage breaks an incapacitate");
                    CHECK(cw.npc_effect_count(wolf, stun->row.guid) == 1, "... and not a stun");
                    // a debuff's stat mods count, and in-place stacks add up to the record's limit
                    combat::LandedEffect weak = landed(volatile_, 6.0);
                    for (const content::ModRow& m : volatile_->mods) weak.mods.push_back({m.stat_guid, -0.15});
                    weak.stack_limit = 3;
                    const auto stat_of = [&](int64_t guid) {
                        const auto* f = cw.fighter_of(wolf);
                        const auto* v = f ? const_cast<combat::Fighter*>(f)->stats().find(guid) : nullptr;
                        return v ? static_cast<double>(v->value) : 0.0;
                    };
                    const int64_t mitigation = volatile_->mods.empty() ? 0 : volatile_->mods.front().stat_guid;
                    const double armour = stat_of(mitigation);
                    cw.apply_effect_to_npc(wolf, weak, 77, now);
                    std::printf("  [info] MagicalMitigation %.4f -> %.4f\n", armour, stat_of(mitigation));
                    CHECK(stat_of(mitigation) > armour, "Status_Volatile makes the creature take more magic damage (the stat is the share that gets through)");
                    // death takes it all off
                    combat::Resolved kill;
                    kill.mods.push_back({0, content::kGuidHealth, -1.0e9});
                    cw.apply_hit_to_npc(wolf, 77, kill, now);
                    CHECK(cw.dead(wolf) && effects_of_actor(wolf) == 0, "a corpse holds no effects");
                    // every effect a hit of the data puts on a creature has numbers the engine can work out: Stomp's slow, with its 3 seconds
                    const auto* stomp = r.catalog->hit(6064633821443985837ll);
                    const uint64_t second = w.spawn(wolf_path, 900, 200, 25, 1.0);
                    combat::Fighter* prey = cw.fighter_of(second);
                    CHECK(stomp && prey, "(Stomp's hit and a creature for it)");
                    if (stomp && prey) {
                        std::vector<combat::LandedEffect> out;
                        cw.resolver().effects_of(player.fighter(), *prey, *stomp, out);
                        CHECK(out.empty(), "without the Quaking talent Stomp slows nothing: each effect's requirement asks for it");
                        const auto* demoralize = r.catalog->hit(6064633780113113263ll);   // Taunt's hit: no talent asked, its second landing is another effect
                        out.clear();
                        if (demoralize) cw.resolver().effects_of(player.fighter(), *prey, *demoralize, out);
                        CHECK(out.size() == 1 && out[0].def == r.catalog->effect_named("Status_Demoralized") && out[0].seconds > 0, "Taunt's first landing demoralizes, with its time from the record");
                        if (!out.empty()) cw.apply_effect_to_npc(second, out[0], 77, now);
                        out.clear();
                        if (demoralize) cw.resolver().effects_of(player.fighter(), *prey, *demoralize, out);
                        CHECK(out.size() == 1 && out[0].def == r.catalog->effect_named("Status_Humiliated"), "... its second humiliates: the requirement reads what the creature holds");
                    }
                    cw.forget(second);
                    w.registry.remove(second);
                }
            }
        }

        std::printf("the rules of a creature's effects, from the data\n");
        {
            Rig r(class_enum("Summoner"), 25);
            const auto rows = r.db->combat_rows(error);
            std::optional<combat::CombatData> data;
            if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
            combat_rig::World w;
            CHECK(r.ok() && data && w.problem.empty(), "(the world of the rules)");
            if (r.ok() && data && w.problem.empty()) {
                combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
                combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, motion);
                combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
                cw.add_player(&player);
                double now = 1.0;
                const auto run = [&](double seconds) {
                    for (double t = 0; t < seconds; t += 0.1) {
                        now += 0.1;
                        r.set_current(content::kGuidHealth, r.pawn.stat(content::kGuidMaxHealth));   // (the owner outlives the wolves around it)
                        r.take();
                        w.npcs->note_player(77, 0, 0, now);
                        w.npcs->tick(now);
                        cw.tick(now);
                    }
                };
                const auto landed = [&](const char* name, double seconds) {
                    combat::LandedEffect e;
                    e.def = r.catalog->effect_named(name);
                    e.seconds = seconds;
                    CHECK(e.def != nullptr, name);
                    return e;
                };
                const auto hit_named = [&](int64_t guid) { return r.catalog->hit(guid); };
                const auto loss = [](double amount) {
                    combat::Resolved hurt;
                    hurt.mods.push_back({0, content::kGuidHealth, -amount});
                    return hurt;
                };
                const auto count_of = [&](uint64_t id, const char* name) {
                    const auto* def = r.catalog->effect_named(name);
                    return def ? cw.npc_effect_count(id, def->row.guid) : -1;
                };
                const uint64_t a = w.spawn(wolf_path, 900, 0, 25, 1.0);
                cw.fighter_of(a);
                // the phases: a trip that ends leaves Tripped_Recently, which the game's next trip asks about
                cw.apply_effect_to_npc(a, landed("Status_Tripped", 0.5), 77, now);
                CHECK(count_of(a, "Status_Tripped") == 1, "a trip lands");
                run(1.0);
                CHECK(count_of(a, "Status_Tripped") == 0 && count_of(a, "Status_Tripped_Recently") == 1, "the trip's end leaves Status_Tripped_Recently");
                // the effect's own requirement: a Boss takes no stun
                const abilities::HitDef* stunning = nullptr;
                for (const content::AbilityHitRow& row : r.catalog->ability(6064633748157319575ll)->hits) {
                    const auto* h = r.catalog->hit(row.hit_guid);
                    if (h && h->row.name == "Summoner_SoulFlare_Damage") stunning = h;
                }
                CHECK(stunning != nullptr, "(Soul Flare's damage hit)");
                const uint64_t stun_target = w.spawn(wolf_path, 900, 300, 25, 1.0);
                combat::Fighter* stun_fighter = cw.fighter_of(stun_target);
                if (stunning && stun_fighter) {
                    std::vector<combat::LandedEffect> out;
                    cw.resolver().effects_of(player.fighter(), *stun_fighter, *stunning, out);
                    CHECK(out.size() == 1 && out[0].def == r.catalog->effect_named("Status_Stunned"), "Soul Flare stuns a creature");
                }
                // the hit mods: Exposed adds a quarter to what effects do to it, Death's Mark a tenth to what its instigator does
                const uint64_t b = w.spawn(wolf_path, 900, -300, 25, 1.0);
                cw.fighter_of(b);
                combat::HitOrigin from_effect;
                from_effect.source_type = content::kSourceTypeEffect;
                combat::HitOrigin from_ability;
                from_ability.source_type = content::kSourceTypeAbility;
                cw.apply_effect_to_npc(b, landed("Status_Exposed", 10.0), 77, now);
                double before = cw.health_of(b);
                cw.apply_hit_to_npc(b, 77, loss(100.0), now, 0, &from_effect);
                CHECK(std::abs(before - cw.health_of(b) - 125.0) < 0.5, "Status_Exposed: an effect's hit takes a quarter more");
                before = cw.health_of(b);
                cw.apply_hit_to_npc(b, 77, loss(100.0), now, 0, &from_ability);
                CHECK(std::abs(before - cw.health_of(b) - 100.0) < 0.5, "... and an ability's the same");
                // what a hit changes: a Hex's time goes down with the hits that name it, and its end detonates
                const uint64_t c = w.spawn(wolf_path, 900, 600, 25, 1.0);
                cw.fighter_of(c);
                cw.apply_effect_to_npc(c, landed("Summoner_Hex", 3.0), 77, now);
                combat::HitOrigin reduce;
                reduce.hit = hit_named(6064633499091999341ll);
                reduce.source_type = content::kSourceTypeAbility;
                CHECK(reduce.hit != nullptr, "(Summoner_Hex_DurReduc)");
                before = cw.health_of(c);
                cw.apply_hit_to_npc(c, 77, loss(1.0), now, 0, &reduce);
                player.dealt.clear();
                run(1.5);
                std::printf("  [info] Hex: %.1f -> %.1f, %zu dealt, Exposed %d\n", before, cw.health_of(c), player.dealt.size(), count_of(c, "Status_Exposed"));
                CHECK(count_of(c, "Summoner_Hex") == 0, "a Hex of 3 seconds cut by 2 is over in a second and a half");
                CHECK(cw.health_of(c) < before - 1.5, "... and its end detonates on the creature");
                // a reaction on the holder: Shocked answers the next hit with a hit of its own
                const uint64_t d = w.spawn(wolf_path, 900, -600, 25, 1.0);
                cw.fighter_of(d);
                cw.apply_effect_to_npc(d, landed("Status_Shocked", 6.0), 77, now);
                combat::HitOrigin strike;
                strike.hit = hit_named(stunning ? stunning->row.guid : 0);
                strike.source_type = content::kSourceTypeAbility;
                before = cw.health_of(d);
                player.dealt.clear();
                cw.apply_hit_to_npc(d, 77, loss(10.0), now, 0, &strike);
                std::printf("  [info] Shocked: a hit of 10 took %.1f, %zu further hits\n", before - cw.health_of(d), player.dealt.size());
                CHECK(player.dealt.size() == 1 && before - cw.health_of(d) > 10.5, "Status_Shocked answers a hit with its own");
                // a knockback throws it away from its instigator
                const uint64_t e = w.spawn(wolf_path, 500, 0, 25, 1.0);
                cw.fighter_of(e);
                const auto* at = w.registry.find(e);
                const double ex = at->x;
                cw.apply_effect_to_npc(e, landed("Prototype_KnockBack_Medium", 1.0), 77, now);
                run(0.6);
                CHECK(w.registry.find(e)->x > ex + 100.0, "a knockback moves it away");
                // the owner's hand on its pet: Reconstruct's ticks heal it, Extract's loss starts no fight and cannot make a corpse
                const pets::Definition& guardian = pets::kDefinitions[0];
                const uint64_t pet = w.spawn(guardian.archetype, 200, 0, 25, 1.0);
                CHECK(cw.pet_summoned(pet, 77), "the pet joins its owner's side");
                combat::TargetInfo seen = cw.inspect(pet, player.fighter());
                CHECK(seen.relation != combat::Relation::Hostile, "to its owner the pet is no enemy");
                const double pet_full = cw.max_health_of(pet);
                cw.apply_hit_to_npc(pet, 77, loss(pet_full * 0.5), now);
                CHECK(std::abs(cw.health_of(pet) - pet_full * 0.5) < 1.0, "the owner's Extract takes the pet's Health");
                CHECK(!cw.ai_of(pet) || cw.ai_state(pet) == npc::ai::State::Idle, "... and the pet does not turn on its owner");
                cw.apply_effect_to_npc(pet, landed("Summoner_Reconstruct", 10.0), 77, now);
                const double hurt_at = cw.health_of(pet);
                run(4.5);
                std::printf("  [info] Reconstruct: pet %.0f -> %.0f of %.0f\n", hurt_at, cw.health_of(pet), pet_full);
                CHECK(cw.health_of(pet) > hurt_at, "Reconstruct's ticks heal the pet");
                // Soul Flare: on the pet, its Apply hit fires the damage around it
                const uint64_t prey = w.spawn(wolf_path, 350, 0, 25, 1.0);
                cw.fighter_of(prey);
                const double prey_before = cw.max_health_of(prey);
                cw.apply_effect_to_npc(pet, landed("Summoner_SoulFlare", 1.0), 77, now);
                run(0.2);
                std::printf("  [info] Soul Flare: creature %.0f of %.0f, stunned %d\n", cw.health_of(prey), prey_before, count_of(prey, "Status_Stunned"));
                CHECK(cw.health_of(prey) < prey_before, "Soul Flare's Apply hit hurts what stands around the pet");
                CHECK(count_of(prey, "Status_Stunned") == 1, "... and stuns it");
                // Amplify: what the pet deals goes up by its own effect
                // Brace: the shield, and the hits it takes go back at whoever struck
                const double pet_shield = cw.shield_of(pet, now);
                CHECK(cw.pet_use(pet, 77, 6064634403600334848ll, 0, guardian.reach_cm), "the pet braces");
                run(1.5);
                CHECK(cw.npc_effect_count(pet, r.catalog->hit(6064634403610624001ll)->effects.front().effect_guid) == 1, "Brace lands on the Guardian");
                std::printf("  [info] Brace: shield %.0f -> %.0f\n", pet_shield, cw.shield_of(pet, now));
                const uint64_t striker = w.spawn(wolf_path, 300, 0, 25, 1.0);
                cw.fighter_of(striker);
                const double striker_full = cw.max_health_of(striker);
                cw.add_threat(striker, pet, 100000.0, now);
                for (int tries = 0; tries < 10 && cw.health_of(striker) >= striker_full; ++tries) run(6.0);   // (the hit it sends back rolls to hit like any other)
                std::printf("  [info] Brace: the creature that struck %.0f of %.0f\n", cw.health_of(striker), striker_full);
                CHECK(cw.health_of(striker) < striker_full, "Brace sends the hit back at the creature");
                // Taunt: it is shouted from afar, and a creature on the owner turns on the Guardian
                const uint64_t on_owner = w.spawn(wolf_path, 1600, 0, 25, 1.0);
                cw.fighter_of(on_owner);
                cw.add_threat(on_owner, 77, 5000.0, now);
                run(0.3);
                CHECK(cw.creature_target(on_owner) == 77, "(the creature is on the owner)");
                CHECK(cw.owner_attacker(77, pet, 3000.0) == on_owner, "a Defensive pet finds what is attacking its owner");
                CHECK(cw.pet_can_act(pet), "the Guardian can act");
                CHECK(cw.pet_use(pet, 77, pets::kGuardianTaunt, on_owner, pets::kGuardianTauntReach), "the Guardian takes Taunt");
                const double guardian_x = w.registry.find(pet)->x;
                run(2.5);
                CHECK(std::abs(w.registry.find(pet)->x - guardian_x) < 200.0, "it taunts from where it stands");
                CHECK(count_of(on_owner, "Status_Demoralized") == 1, "Taunt lands at range");
                CHECK(cw.creature_target(on_owner) == pet, "the creature turns on the Guardian");
                // following: one walk after a moving owner, not a walk that stops and starts
                {
                    const pets::Definition& mystic = pets::kDefinitions[2];
                    const uint64_t follower = w.spawn(mystic.archetype, 0, 2500, 25, 1.0);
                    auto owner_at = std::make_shared<nav::Vec3>(nav::Vec3{0, 2000, 1010});
                    CHECK(w.npcs->order_pursue(follower, [owner_at](nav::Vec3& out) { out = *owner_at; return true; }, 720.0, 250.0), "the pet sets off after its owner");
                    int stops = 0;
                    bool was_moving = true;
                    double last_y = w.registry.find(follower)->y;
                    for (int i = 0; i < 40; ++i) {
                        owner_at->y -= 40.0;   // (a walking owner, 400 cm a second)
                        now += 0.1;
                        w.npcs->note_player(77, 0, 0, now);
                        w.npcs->tick(now);
                        const double y = w.registry.find(follower)->y;
                        const bool moving = std::abs(y - last_y) > 0.5;
                        if (i > 8 && was_moving && !moving) ++stops;
                        was_moving = moving;
                        last_y = y;
                    }
                    std::printf("  [info] following a walking owner: %d stops in 4 s, %.0f cm behind\n", stops, last_y - owner_at->y);
                    CHECK(stops <= 2, "a pet following a walking owner keeps walking");
                    w.npcs->halt(follower);
                }
                // Lunge: the melee Hunter closes in a leap, not a run
                const pets::Definition& hunter = pets::kDefinitions[1];
                const uint64_t lunger = w.spawn(hunter.archetype, -800, 0, 25, 1.0);
                const uint64_t far_prey = w.spawn(wolf_path, -200, 0, 1, 1.0);
                cw.pet_summoned(lunger, 77);
                CHECK(cw.pet_use(lunger, 77, pets::kHunterLunge, far_prey, 999.0), "the Hunter takes Lunge");
                const double from = w.registry.find(lunger)->x;
                run(0.4);
                CHECK(w.registry.find(lunger)->x > from + 300.0, "it covers the ground faster than it could run");
                run(2.0);
                // what a creature's hit leaves on the ground: the shaman's ooze hurts a player standing in it each second, for its six seconds, and no longer
                {
                    const abilities::HitDef* impact = r.catalog->hit_named("GoblinShaman_OozingBombProjectileImpactGroundTrace_Hit");
                    CHECK(impact && !impact->zones.empty(), "(the ooze's impact hit)");
                    const uint64_t shaman = w.spawn(wolf_path, 900, -1200, 25, 1.0);
                    cw.fighter_of(shaman);
                    if (impact) {
                        const size_t before_zone = player.hits.size();
                        cw.spawn_hit_zones(shaman, *impact, player.fighter().position(), now);
                        run(3.2);
                        size_t oozed = 0;
                        for (size_t i = before_zone; i < player.hits.size(); ++i) oozed += player.hits[i].source_type == content::kSourceTypeZone ? 1 : 0;
                        std::printf("  [info] the ooze: %zu hits in 3.2 s\n", oozed);
                        CHECK(oozed >= 2 && oozed <= 4, "the ooze hits the player standing in it about once a second");
                        run(4.0);
                        const size_t after_end = player.hits.size();
                        run(2.0);
                        size_t later = 0;
                        for (size_t i = after_end; i < player.hits.size(); ++i) later += player.hits[i].source_type == content::kSourceTypeZone ? 1 : 0;
                        CHECK(later == 0, "... and it is gone after its six seconds");
                    }
                    cw.forget(shaman);   // (it has hit the owner: the pet's tests below ask who does)
                }
                // a rally: the effect goes on the creature and its friends near it, and not on one far away
                {
                    const abilities::HitDef* rally = r.catalog->hit_named("Goblin_Rally_Hit");
                    const abilities::EffectDef* rallied = r.catalog->effect_named("Goblin_Rally_Effect");
                    CHECK(rally && rallied, "(the goblins' rally)");
                    const uint64_t caller = w.spawn(wolf_path, 900, 2000, 25, 1.0), near_one = w.spawn(wolf_path, 1000, 2000, 25, 1.0), far_one = w.spawn(wolf_path, 900, 6000, 25, 1.0);
                    cw.fighter_of(caller);
                    cw.fighter_of(near_one);
                    cw.fighter_of(far_one);
                    if (rally && rallied) {
                        cw.land_on_friends(caller, *rally, now);
                        CHECK(cw.npc_effect_count(caller, rallied->row.guid) == 1 && cw.npc_effect_count(near_one, rallied->row.guid) == 1, "a rally reaches the caller and the friend beside it");
                        CHECK(cw.npc_effect_count(far_one, rallied->row.guid) == 0, "... and not one 40 m away");
                    }
                    // a healer's touch: the friend most hurt is healed, and nobody when nobody is hurt
                    const abilities::HitDef* touch = r.catalog->hit_named("Bandit_Healer_ReliefTouch_Hit");
                    CHECK(touch != nullptr, "(the bandit healer's touch)");
                    if (touch) {
                        cw.apply_hit_to_npc(near_one, 77, loss(500.0), now);
                        const double hurt = cw.health_of(near_one), whole = cw.health_of(caller);
                        cw.land_on_friends(caller, *touch, now);
                        std::printf("  [info] a healer's touch: %.1f -> %.1f\n", hurt, cw.health_of(near_one));
                        CHECK(cw.health_of(near_one) > hurt && cw.health_of(caller) == whole, "the healer's touch heals its hurt friend");
                    }
                    cw.forget(caller);
                    cw.forget(near_one);
                    cw.forget(far_one);
                }
            }
        }

        std::printf("a pet's abilities\n");
        for (const pets::PetAbility& use : pets::kPetAbilities) {
            if (use.ability == pets::kHunterStanceSwap) continue;   // (the session's)
            const pets::Definition* def = pets::for_effect(use.pet_effect);
            Rig r(class_enum("Summoner"), 25);
            const auto rows = r.db->combat_rows(error);
            std::optional<combat::CombatData> data;
            if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
            combat_rig::World w;
            CHECK(r.ok() && data && w.problem.empty() && def, "(the world of the pet's ability)");
            if (!r.ok() || !data || !w.problem.empty() || !def) break;
            combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
            combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, motion);
            combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
            cw.add_player(&player);
            const uint64_t pet = w.spawn(def->archetype, 200, 0, 25, 1.0);
            const uint64_t prey = w.spawn(wolf_path, 500, 0, 25, 1.0);
            const auto* a = r.catalog->ability(use.ability);
            CHECK(pet && prey && a, "(the pet, a creature and the ability)");
            if (!pet || !prey || !a) break;
            CHECK(cw.pet_use(pet, 77, use.ability, prey, def->reach_cm) && cw.pet_casting(pet), "the pet takes the ability");
            double now = 1.0;
            cw.fighter_of(prey);
            const double full = cw.max_health_of(prey);
            for (int i = 0; i < 80 && cw.pet_casting(pet); ++i) {
                now += 0.1;
                w.npcs->note_player(77, 0, 0, now);
                w.npcs->tick(now);
                cw.tick(now);
            }
            CHECK(!cw.pet_casting(pet), "the ability ends");
            std::printf("  [info] %s: creature %.0f of %.0f Health, %zu dealt, %zu on the owner\n", a->row.name.c_str(), cw.health_of(prey), full, player.dealt.size(), player.from_pet.size());
            const std::string& name = a->row.name;
            if (name.find("Stomp") != std::string::npos) {
                CHECK(cw.health_of(prey) < full && !player.dealt.empty(), "Stomp hurts what stands around the pet, and the owner is told");
            }
            if (name.find("Taunt") != std::string::npos) CHECK(cw.npc_effect_count(prey, 6064630758760448234ll) == 1, "Taunt demoralizes the creature");
            if (name.find("Incapacitate") != std::string::npos) CHECK(cw.npc_effect_count(prey, 6064634064199483408ll) == 1, "Incapacitate stops the melee Hunter's target");
            if (name.find("Eviscerate") != std::string::npos || name.find("Lunge") != std::string::npos) CHECK(cw.health_of(prey) < full, "its hit hurts the creature");
            if (name.find("Eruption") != std::string::npos) CHECK(cw.health_of(prey) < full, "Essence Eruption hurts around its target");
            if (name.find("Recreate") != std::string::npos || name.find("Cleanse") != std::string::npos) CHECK(!player.from_pet.empty(), "the Mystic's help lands on its owner");
            if (name.find("Brace") != std::string::npos) CHECK(cw.npc_effect_count(pet, r.catalog->hit(6064634403610624001ll)->effects.front().effect_guid) == 1, "Brace lands on the Guardian");
            CHECK(!cw.pet_use(pet, 77, 987654, prey, def->reach_cm), "a pet plays no ability that is not there");
        }

        std::printf("a Summoner's abilities on its pet, as the client asks for them\n");
        {
            Rig r(class_enum("Summoner"), 25);
            const auto rows = r.db->combat_rows(error);
            std::optional<combat::CombatData> data;
            if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
            combat_rig::World w;
            CHECK(r.ok() && data && w.problem.empty(), "(the world of the pet)");
            if (r.ok() && data && w.problem.empty()) {
                combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
                combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, motion);
                combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
                cw.add_player(&player);
                combat::WorldHitPort port(cw, w.registry, player.fighter());
                r.pipe->set_combat(&port);
                r.pipe->set_reactions_enabled(true);
                const pets::Definition& guardian = pets::kDefinitions[0];
                const uint64_t pet = w.spawn(guardian.archetype, 200, 0, 25, 1.0);
                const uint64_t prey = w.spawn(wolf_path, 900, 0, 25, 1.0);
                cw.fighter_of(prey);
                CHECK(pet && prey && cw.pet_summoned(pet, 77), "(the Guardian is on its owner's side)");
                CHECK(r.pipe->grant_effect(guardian.effect, 1.0), "(its summon's effect is on the owner)");
                r.take();
                constexpr int64_t kExtract = 6064633645783911914ll, kExtractHit = 6064633645898797957ll, kSiphon = 6064633662084304872ll, kSiphonHit = 6064633662084304873ll;
                constexpr int64_t kBond = 6064633814845161472ll, kBondHit = 6064633815783636993ll, kBondPet = 6064633815887904770ll, kBondCaster = 6064633821403750774ll;
                constexpr int64_t kShred = 6064633271239453745ll, kShredHit = 6064633271360225904ll, kFortitude = 6064634608804770959ll;
                constexpr int64_t kHex = 6064633493499835793ll, kHexKillReset = 6064634568783891085ll;
                constexpr int64_t kReconstruct = 6064633663176909411ll, kReconstructHit = 6064633663176909412ll, kAmplify = 6064633844273079586ll, kAmplifyHit = 6064633861412726145ll;
                constexpr int64_t kPhaseOut = 6064633866966605140ll, kPhaseOutPet = 6064633867155810140ll;
                r.pipe->set_known({kExtract, kSiphon, kBond, kShred, kHex, kReconstruct, kAmplify, kPhaseOut});
                double t = 2.0;
                int32_t id = 100;
                // the client's order: the trigger, then at the montage's notify the request for the hit, naming who it reached
                const auto cast = [&](int64_t ability, int64_t hit, const std::vector<uint64_t>& aim, int64_t branch = 0) {
                    const bool channelled = branch != 0;
                    TriggerInfo info;
                    info.target_count = static_cast<uint32_t>(aim.size());
                    info.target_actors = aim;
                    const Refusal why = r.pipe->trigger(ability, ++id, t, info);
                    r.take();
                    const size_t notes_from = r.log.size();
                    t += channelled ? 1.6 : 0.3;
                    if (channelled) r.pipe->branch_request(id, branch, t);
                    t += channelled ? 0.2 : 0.0;
                    r.pipe->refresh(t, "(the cost step)");
                    r.take();
                    const auto verdict = r.pipe->hit_request(ability, 0, hit, 1, aim.empty() ? 1 : 0, t, aim);
                    std::printf("  [info] %s: trigger %d, request %s, targets %zu\n", r.catalog->ability(ability)->row.name.c_str(), static_cast<int>(why), AbilityPipeline::verdict_name(verdict), aim.size());
                    r.take();
                    t += 1.5;
                    r.pipe->refresh(t, "(the hit lands)");
                    r.take();
                    for (size_t i = notes_from; i < r.log.size(); ++i) std::printf("    [note] %s\n", r.log[i].note.c_str());
                    t += 40.0;   // (the next one owes nothing to this one's cooldown)
                    return why;
                };
                const auto refill = [&] {
                    r.pipe->refresh(t, "(refill)");
                    r.take();
                    r.set_current(pets::kEssence, pets::kEssenceMax);
                    r.set_current(content::kGuidMana, r.pawn.stat(content::kGuidMaxMana));
                    r.take();
                };
                refill();
                const double full = r.pawn.stat(content::kGuidMaxHealth);
                r.set_current(content::kGuidHealth, full * 0.5);
                r.take();
                const double pet_before = cw.health_of(pet);
                CHECK(cast(kExtract, kExtractHit, {pet}, 6064633645892571971ll) == Refusal::None, "Extract is taken with a pet out");
                std::printf("  [info] Extract: Health %.0f -> %.0f of %.0f, pet %.0f -> %.0f\n", full * 0.5, r.pawn.stat(content::kGuidHealth), full, pet_before, cw.health_of(pet));
                CHECK(r.pawn.stat(content::kGuidHealth) > full * 0.5 + 1.0, "Extract heals its owner (the reaction its hit carries)");
                CHECK(cw.health_of(pet) < pet_before, "and takes it from the pet");
                refill();
                r.set_current(content::kGuidMana, 1.0);
                r.take();
                CHECK(cast(kSiphon, kSiphonHit, {pet}, 6064633662084370415ll) == Refusal::None, "Siphon is taken with a pet out");
                std::printf("  [info] Siphon: Mana 1 -> %.0f\n", r.pawn.stat(content::kGuidMana));
                CHECK(r.pawn.stat(content::kGuidMana) > 2.0, "Siphon gives its owner Mana (the reaction its hit carries)");

                refill();
                CHECK(cast(kBond, kBondHit, {}) == Refusal::None, "Bond is taken");
                r.pipe->refresh(t, "(Bond ticks)");
                r.take();
                std::printf("  [info] Bond: on the pet %d, on the owner %d\n", cw.npc_effect_count(pet, kBondPet), r.pipe->stacks_of(kBondCaster));
                CHECK(cw.npc_effect_count(pet, kBondPet) == 0, "Mutual Bond, the pet's own effect, asks for its talent");
                CHECK(r.pipe->stacks_of(kBondCaster) > 0, "Bond's tick reaches the pet, whose hit gives its owner its side (the reaction it carries)");

                refill();
                {
                    w.npcs->note_player(77, 0, 0, t);   // (the player is where it was, as a session says every step)
                    w.npcs->tick(t);
                    cw.tick(t);
                    TriggerInfo shred;
                    shred.target_count = 1;
                    shred.target_actors = {prey};
                    CHECK(r.pipe->trigger(kShred, ++id, t, shred) == Refusal::None, "Spirit Shred is taken");
                    r.take();
                    t += 0.3;
                    r.pipe->refresh(t, "(its cost)");
                    r.take();
                    const std::vector<uint64_t> on_prey = {prey};
                    r.pipe->hit_request(kShred, 0, kShredHit, 1, 0, t, on_prey);
                    for (int step = 0; step < 16; ++step) {   // (its effect ticks on the creature)
                        t += 0.5;
                        r.pipe->refresh(t, "(it ticks)");
                        r.take();
                        w.npcs->note_player(77, 0, 0, t);
                        w.npcs->tick(t);
                        cw.tick(t);
                    }
                    CHECK(cw.npc_effect_count(prey, r.catalog->hit(kShredHit)->effects.front().effect_guid) == 1 || cw.health_of(prey) < cw.max_health_of(prey), "(it landed on the creature)");
                    t += 40.0;
                }
                std::printf("  [info] Spirit Shred: Fortitude on the Guardian %d\n", cw.npc_effect_count(pet, kFortitude));
                CHECK(cw.npc_effect_count(pet, kFortitude) >= 1, "its damage stacks Fortitude on the Guardian (the reaction its hit carries)");

                refill();
                CHECK(cast(kReconstruct, kReconstructHit, {pet}) == Refusal::None, "Reconstruct is taken");
                refill();
                CHECK(cast(kAmplify, kAmplifyHit, {pet}) == Refusal::None, "Amplify is taken");
                CHECK(cw.npc_effect_count(pet, r.catalog->hit(kAmplifyHit)->effects.front().effect_guid) == 1, "Amplify lands on the pet");
                refill();
                CHECK(cast(kPhaseOut, kPhaseOutPet, {pet}) == Refusal::None, "Phase Out is taken");
                CHECK(cw.npc_effect_count(pet, r.catalog->hit(kPhaseOutPet)->effects.front().effect_guid) == 1, "Phase Out lands on the pet");

                // a pet's ability: its Essence, and its cooldown where the owner's bar reads it
                refill();
                {
                    double seconds = 0;
                    const double essence = r.pawn.stat(pets::kEssence);
                    CHECK(r.pipe->pay_pet_ability(pets::kGuardianTaunt, t, seconds), "Taunt is paid for");
                    r.take();
                    std::printf("  [info] Taunt: Essence %.1f -> %.1f, cooldown %.1f s\n", essence, r.pawn.stat(pets::kEssence), seconds);
                    CHECK(r.pawn.stat(pets::kEssence) < essence, "it costs Essence");
                    CHECK(std::abs(seconds - 10.0) < 0.01, "its cooldown is the record's 10 s");
                    CHECK(r.pipe->charges_left(pets::kGuardianTaunt) == 0, "the cooldown runs on the owner, where the bar reads it");
                    t += seconds + 1.0;
                    r.pipe->refresh(t, "(the cooldown is over)");
                    r.take();
                    CHECK(r.pipe->charges_left(pets::kGuardianTaunt) == -1, "and ends");
                    r.set_current(pets::kEssence, 0.0);
                    r.take();
                    CHECK(!r.pipe->pay_pet_ability(pets::kGuardianTaunt, t, seconds), "with no Essence it is refused");
                    r.take();
                    CHECK(r.pipe->charges_left(pets::kGuardianTaunt) == -1, "and starts no cooldown");
                }
                // a hit's ModifyCooldownsIds: a Hexed creature's death gives Hex back
                refill();
                TriggerInfo hex;
                hex.target_count = 1;
                hex.target_actors = {prey};
                CHECK(r.pipe->trigger(kHex, ++id, t, hex) == Refusal::None, "Hex is taken");
                r.take();
                t += 0.6;
                r.pipe->refresh(t, "(its cost)");
                r.take();
                CHECK(r.pipe->charges_left(kHex) != -1, "Hex's cooldown runs");
                r.pipe->land_pet_hit(kHexKillReset, kHex, t);
                r.take();
                CHECK(r.pipe->charges_left(kHex) == -1, "the kill gives Hex back (Summoner_Hex_KillReset)");
                // its Health comes back by itself, and 5 s on its hits land as before
                {
                    const double hurt = cw.health_of(pet);
                    for (int step = 0; step < 50; ++step) {
                        t += 0.1;
                        w.npcs->note_player(77, 0, 0, t);
                        w.npcs->tick(t);
                        cw.tick(t);
                    }
                    std::printf("  [info] a pet's regeneration: %.1f -> %.1f in 5 s of %.0f\n", hurt, cw.health_of(pet), cw.max_health_of(pet));
                    CHECK(cw.health_of(pet) > hurt, "a hurt pet regenerates");
                    const uint64_t beside = w.spawn(wolf_path, 350, 0, 25, 1.0);
                    cw.fighter_of(beside);
                    const double whole = cw.max_health_of(beside);
                    combat::LandedEffect flare;
                    flare.def = r.catalog->effect_named("Summoner_SoulFlare");
                    flare.seconds = 1.0;
                    cw.apply_effect_to_npc(pet, flare, 77, t);
                    for (int step = 0; step < 3; ++step) {
                        t += 0.1;
                        w.npcs->note_player(77, 0, 0, t);
                        w.npcs->tick(t);
                        cw.tick(t);
                    }
                    CHECK(cw.health_of(beside) < whole, "Soul Flare still hurts what stands around the pet");
                }

                r.pipe->set_combat(nullptr);
            }
        }

        std::printf("Dimensional Bolt: its presses, a held key's triggers and its cooldown\n");
        {
            Rig r(class_enum("Summoner"), 25);
            const auto rows = r.db->combat_rows(error);
            std::optional<combat::CombatData> data;
            if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
            combat_rig::World w;
            CHECK(r.ok() && data && w.problem.empty(), "(the world of the bolt)");
            if (r.ok() && data && w.problem.empty()) {
                combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
                combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, motion);
                combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
                cw.add_player(&player);
                combat::WorldHitPort port(cw, w.registry, player.fighter());
                r.pipe->set_combat(&port);
                const uint64_t prey = w.spawn(wolf_path, 900, 0, 25, 1.0);
                cw.fighter_of(prey);
                r.pipe->set_known({pets::kBolt});
                TriggerInfo info;
                info.target_count = 1;
                info.target_actors = {prey};
                double t = 1.0;
                const auto press = [&](int32_t id, double at) {
                    const Refusal why = r.pipe->trigger(pets::kBolt, id, at, info);
                    r.take();
                    return why;
                };
                CHECK(r.pipe->grant_effect(pets::kDefinitions[0].effect, t), "(a Guardian is out)");
                r.take();
                CHECK(press(1, t) == Refusal::None, "the first press throws the bolt");
                CHECK(press(2, t + 0.02) == Refusal::Busy, "a held key's next trigger, 20 ms on, throws no second bolt");
                CHECK(r.pipe->charges_left(pets::kBolt) == -1, "the bolt itself starts no cooldown");

                const abilities::EffectDef* mark = r.catalog->effect(pets::kBoltMark);
                CHECK(mark != nullptr, "the bolt's mark is in the data");
                if (mark) {
                    combat::LandedEffect landed;
                    landed.def = mark;
                    landed.seconds = 5.0;
                    t = 3.0;
                    cw.apply_effect_to_npc(prey, landed, 77, t);
                    r.pipe->refresh(t, "the first bolt is paid for");
                    r.take();
                    const double mana = r.pawn.stat(content::kGuidMana);
                    CHECK(press(3, t) == Refusal::None, "the next press on the marked creature, with a pet out, is taken");
                    const auto recall = r.pipe->take_bolt_recall();
                    CHECK(recall.target == prey && !recall.behind, "it sends the Guardian to the creature");
                    CHECK(cw.npc_effect_count(prey, pets::kBoltMark) == 0, "the press spends the mark");
                    CHECK(r.pipe->stacks_of(pets::kBoltLockout) == 0 && r.pipe->stacks_of(pets::kBoltLockout2) == 0, "and the lockouts");
                    CHECK(r.pawn.stat(content::kGuidMana) >= mana, "and costs no Mana");
                    CHECK(r.pipe->charges_left(pets::kBolt) == 1, "it starts the cooldown, and spends no charge (retail's CD says 1)");
                    CHECK(press(4, t + 0.02) == Refusal::Charges, "the next trigger of the held key is refused");
                    CHECK(r.pipe->take_bolt_recall().target == 0, "and sends nothing");
                    CHECK(press(5, t + 13.0) == Refusal::None, "12 s on the bolt can be thrown again");
                    t += 32.0;   // (that bolt's own lockout and cooldown are over)
                }
                // no second press: the lockout refuses the bolt, and its second effect's end starts the cooldown
                CHECK(r.pipe->grant_effect(pets::kBoltLockout2, t), "(the bolt's second lockout)");
                r.take();
                CHECK(press(6, t + 0.1) == Refusal::Busy, "under the lockout, with no mark, nothing is thrown");
                r.pipe->refresh(t + 6.0, "the lockout is over");
                r.take();
                CHECK(r.pipe->charges_left(pets::kBolt) == 1, "the lockout's end starts the cooldown");
                r.pipe->set_combat(nullptr);
            }
        }

        std::printf("a Summoner's pets fight\n");
        for (const pets::Definition& def : pets::kDefinitions) {
            Rig r(class_enum("Summoner"), 25);
            const auto rows = r.db->combat_rows(error);
            std::optional<combat::CombatData> data;
            if (rows) data = combat::CombatData::build(*rows, *r.ctx->bundle, error);
            combat_rig::World w;
            CHECK(r.ok() && data && w.problem.empty(), "(the world of the pet's fight)");
            if (!r.ok() || !data || !w.problem.empty()) break;
            combat::NpcStatBook book(*r.ctx->bundle, *r.ctx->gear);
            combat::CombatWorld cw(w.registry, w.npcs.get(), &navfix::shared_nav(), *data, *r.catalog, book, motion);
            combat_rig::FakePlayer player(r, 77, {0, 0, 1098});
            cw.add_player(&player);
            const uint64_t pet = w.spawn(def.archetype, 200, 0, 25, 1.0);
            const uint64_t prey = w.spawn(wolf_path, 900, 0, 1, 1.0);
            CHECK(pet && prey, "(the pet and a creature stand in the world)");
            if (!pet || !prey) break;
            CHECK(r.catalog->ability(def.attack) != nullptr, "the pet's attack is in the data");
            CHECK(!cw.pet_attack(pet, 77, def.attack, pet) && !cw.pet_attack(pet, 77, def.attack, 987654), "a pet does not attack itself or nothing");
            CHECK(cw.pet_attack(pet, 77, def.attack, prey) && cw.pet_fighting(pet), "the pet takes the order to attack");
            double now = 1.0, full = cw.max_health_of(prey);
            size_t steps = 0;
            for (; steps < 900 && !cw.dead(prey); ++steps) {
                now += 0.1;
                w.npcs->note_player(77, 0, 0, now);
                w.npcs->tick(now);
                cw.tick(now);
            }
            std::printf("  [info] %s killed the creature (%.0f Health) in %.1f s\n", def.name, full, 0.1 * static_cast<double>(steps));
            CHECK(cw.dead(prey), "the pet's attacks kill the creature");
            for (int i = 0; i < 5; ++i) {
                now += 0.1;
                cw.tick(now);
            }
            CHECK(!cw.pet_fighting(pet), "the fight ends with the creature's death");
            // a creature of the pet's level, far from the owner: it answers by running at the owner, who is what it hits, so the seconds until it arrives are the pet's alone
            const uint64_t second = w.spawn(wolf_path, 3600, 0, 25, 1.0);
            CHECK(second && cw.pet_attack(pet, 77, def.attack, second), "it takes another order");
            double even = 0, lowest = second ? cw.max_health_of(second) : 0.0, lowest_at = 0;
            for (; even < 15.0 && second && !cw.dead(second) && cw.pet_fighting(pet); even += 0.1) {
                now += 0.1;
                w.npcs->note_player(77, 0, 0, now);
                w.npcs->tick(now);
                cw.tick(now);
                if (cw.health_of(second) < lowest) {
                    lowest = cw.health_of(second);
                    lowest_at = even;
                }
            }
            const double taken = second ? cw.max_health_of(second) - lowest : 0.0;
            std::printf("  [info] %s against a creature of its own level: %.0f of %.0f Health by %.1f s\n", def.name, taken, second ? cw.max_health_of(second) : 0.0, lowest_at);
            CHECK(taken > 0, "its hits count as its owner's and hurt a creature of its level");
            CHECK(taken < 0.9 * cw.max_health_of(second) || lowest_at > 5.0, "a pet does not cut a creature of its level down in a few seconds");
            // the creature turns on the pet, not on the owner, and when the pet is killed it goes for the owner rather than going home healed
            if (second) {   // (the last creature leaves: it was chasing the owner)
                cw.forget(second);
                w.registry.remove(second);
            }
            r.set_current(content::kGuidHealth, r.pawn.stat(content::kGuidMaxHealth));
            r.take();
            const double pet_full = cw.health_of(pet);
            const size_t hits_before = player.hits.size();
            const uint64_t foe = w.spawn(wolf_path, 3600, 0, 25, 1.0);
            CHECK(foe && cw.pet_attack(pet, 77, def.attack, foe), "the pet goes for a third creature");
            for (double t = 0; t < 180.0 && foe && !cw.dead(pet) && !cw.dead(foe); t += 0.1) {
                now += 0.1;
                w.npcs->note_player(77, 0, 0, now);
                w.npcs->tick(now);
                cw.tick(now);
            }
            std::printf("  [info] %s after the fight: pet %.0f of %.0f Health (%s), creature %.0f of %.0f\n", def.name, cw.health_of(pet), pet_full, cw.dead(pet) ? "killed" : "alive", foe ? cw.health_of(foe) : 0.0, foe ? cw.max_health_of(foe) : 0.0);
            CHECK(cw.health_of(pet) < pet_full, "the creature hurts the pet");
            CHECK(player.hits.size() == hits_before, "while the pet stands the creature leaves the owner alone");
            if (cw.dead(pet)) {
                CHECK(!cw.pet_fighting(pet), "a killed pet's fight is over");
                CHECK(foe && !cw.dead(foe) && cw.health_of(foe) < cw.max_health_of(foe), "(the creature lived, hurt)");
                for (int i = 0; i < 5; ++i) {
                    now += 0.1;
                    w.npcs->note_player(77, 0, 0, now);
                    w.npcs->tick(now);
                    cw.tick(now);
                }
                const auto state = cw.ai_state(foe);
                CHECK(state && *state != npc::ai::State::Reset && *state != npc::ai::State::Idle, "the creature does not go home healed: it goes for the owner");
            }
            cw.pet_stop(pet);
            CHECK(!cw.pet_fighting(pet), "a stop ends the fight");
            cw.pet_gone(pet);
            CHECK(!cw.is_combatant(pet), "a pet that leaves is forgotten");
        }
    }
    std::printf("\n%-10s %7s %9s %9s %9s %9s %9s %9s %9s\n", "class", "casts", "accepted", "listed ok", "exact", "on time", "with ts", "hits", "landed");
    size_t total = 0, accepted = 0, matched = 0, exact = 0, refused_listed = 0;
    std::vector<double> all_ratios;
    for (const auto& [cls, t] : tally) {
        std::printf("%-10s %7zu %9zu %9zu %9zu %9zu %9zu %9zu %9zu\n", cls.c_str(), t.total, t.accepted, t.matched, t.exact, t.timed, t.with_ts, t.hit_cases, t.hit_landed);
        total += t.total;
        exact += t.exact;
        accepted += t.accepted;
        matched += t.matched;
        refused_listed += t.refused_listed;
        all_ratios.insert(all_ratios.end(), t.ratios.begin(), t.ratios.end());
    }
    std::printf("%-10s %7zu %9zu %9zu %9zu\n", "all", total, accepted, matched, exact);
    std::printf("\n%zu casts were made after the state their requirement names was given to the caster (a stealth, a stack, an enchant, a summoned pet's effect) [modelled]", states_granted);
    std::printf("\ncast at: %zu at the creature, %zu at the caster (an ally's place), %zu with no target; retail's targets: ", at_npc, at_self, at_air);
    for (const auto& [k, n] : kinds) std::printf("%s %zu  ", k.c_str(), n);
    std::printf("\n");
    // the amounts: our hit's total stat mod over retail's, where the hit landed and both name the same stat mods (the creature's and the caster's stats differ
    // from retail's, so this is a distribution)
    std::printf("\namounts (ours / retail's, hits that landed on both sides with the same stat mods):\n");
    for (const auto& [cls, t] : tally) {
        if (t.ratios.empty()) continue;
        std::vector<double> v = t.ratios;
        std::sort(v.begin(), v.end());
        const auto q = [&](double p) { return v[std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size())))]; };
        std::printf("  %-9s %5zu hits: quartiles %.2f / %.2f / %.2f   (mods the same set: %zu, a different set: %zu)\n", cls.c_str(), v.size(), q(0.25), q(0.5), q(0.75), t.mods_same, t.mods_diff);
    }
    if (!all_ratios.empty()) {
        std::sort(all_ratios.begin(), all_ratios.end());
        std::printf("  all       %5zu hits: quartiles %.2f / %.2f / %.2f\n", all_ratios.size(), all_ratios[all_ratios.size() / 4], all_ratios[all_ratios.size() / 2], all_ratios[all_ratios.size() * 3 / 4]);
    }
    std::printf("\nwhat is left:\n");
    for (const auto& [reason, n] : reasons) {
        std::printf("  %5zu  %s\n", n, reason.c_str());
        if (verbose) {
            size_t shown = 0;
            for (const std::string& s : reason_cases[reason]) {
                if (++shown > 6) break;
                std::printf("           %s\n", s.c_str());
            }
        }
    }
    std::printf("%zu casts are refused and listed in kKnown with the reason\n", refused_listed);
    if (only_class.empty() && only_ability.empty() && only_capture == 0) {
        CHECKF(total == n_cases, "every cast of the header was run (%zu of %zu)", total, n_cases);
        CHECKF(accepted + refused_listed == total, "every cast is accepted, or its refusal is listed (%zu + %zu of %zu)", accepted, refused_listed, total);
        CHECKF(matched == total, "every accepted cast matches retail's reply (%zu of %zu)", matched, total);
    }
    g_fail += aoc_tests::run();   // every check registered in ability_npc_registered.inc
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
