// ============================================================================
// test_worldd_npc_placement: where every NPC of the region stands once the NPC world has grounded it, against the height the data gave it (heavy: run alone)
//
//  The Riverlands are loaded as worldd loads them (load_area, load_ground), then the ground under every NPC is loaded one chunk at a time and the NPCs waiting for it are grounded, as a player
//  walking up to them would.  Each NPC's height then is compared with its data's.  The NPCs that moved more than 3 m, and the ones that stand where no surface is within the open world's reach
//  (15 m) of their height, are counted by why and listed (the list goes to AOC_PLACEMENT_REPORT, a TSV, when that is set): the investigation of 4 November 2026, Maryl Amberstone of A Heavy
//  Load stood 165 m up on the mountain over the Anvils' halls.
//  Needs the chunked area_nav.bin (it skips, exit 0, without it).  Loads 1 GB of ground and takes minutes.
// ============================================================================
#include "content/content_db.h"
#include "nav/area_nav.h"
#include "npc/npc_world.h"
#include "session/player_world.h"
#include "spawns/region_data.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

using namespace aoc::worldd;
namespace bs = aoc::worldd::bootstrap;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { std::printf("  [ok ] %s\n", msg); g_pass++; } \
    else { std::printf("  [FAIL] %s\n", msg); g_fail++; } \
} while (0)

static constexpr uint32_t kServerId = 60;
static constexpr uint32_t kRandomizer = 0x6ee87ee4u;
static constexpr double kMovedCm = 300.0;     // a move smaller than this is the feet finding the floor they were drawn on
static constexpr double kReachCm = 1500.0;    // the open world's reach (NpcWorld::ground_npc)
static constexpr double kFeetCm = 88.0;       // a person's half height: the data's height is the origin, the reach is asked from the feet

namespace {

struct Placed {
    uint64_t    id = 0;
    std::string name;
    world::ActorKind kind = world::ActorKind::Npc;
    bool        captured = false; // retail's own place for it (a capture), not a spawn point of the data
    bool        talks = false;    // a dialogue, a vendor or an interaction: someone a player comes to
    bool        by_data = false, by_terrain = false, flies = false;
    double      x = 0, y = 0, data_z = 0, z = 0;
    double      terrain = 0;
    bool        has_terrain = false, surface_near = false, chunk_ok = true;
    double      beside_cm = -1;   // with no surface over or under it: how far off (cm, within 10 m) the nearest walkable point at its height is (-1: none)
    std::string why;
};

}  // namespace

int main() {
    spdlog::set_level(spdlog::level::warn);
    const char* env = std::getenv("AOC_CONTENT_DB");
    const std::string db_path = env && *env ? env : PLACEMENT_CONTENT_DB_PATH;
    const char* env_nav = std::getenv("AOC_AREA_NAV");
    const std::string nav_path = env_nav && *env_nav ? env_nav : PLACEMENT_NAV_PATH;
    const char* env_motion = std::getenv("AOC_NPC_MOTION");
    const std::string motion_path = env_motion && *env_motion ? env_motion : PLACEMENT_MOTION_PATH;
    const char* env_report = std::getenv("AOC_PLACEMENT_REPORT");
    std::string error;
    {
        std::string e;
        auto probe = nav::AreaNav::load(nav_path, &e);
        if (!probe || !probe->windowed()) {
            std::printf("  [skip] %s: %s (the chunked ground: tools/mapc build, then aoc_navbuild)\n", nav_path.c_str(), probe ? "it is not a chunked file" : e.c_str());
            return 0;
        }
    }
    auto content = content::ContentDb::open(db_path, error);
    if (!content) {
        std::printf("  [FAIL] content.db %s does not open: %s\n", db_path.c_str(), error.c_str());
        return 2;
    }
    aoc::protocol::ServerGuidAllocator guids(kServerId, kRandomizer);
    PlayerWorld world(&*content, "", bs::GameStateMode::Tier2, kServerId, guids);
    if (!world.ready()) {
        std::printf("  [FAIL] the world is not ready: %s\n", world.content_problem().c_str());
        return 2;
    }
    spawns::SpawnContext ctx;
    ctx.hour = 12.0;
    world.set_spawn_context(ctx);
    world.load_blueprints(motion_path, error);
    if (!world.load_area(spawns::AreaConfig::riverlands(), error)) {
        std::printf("  [FAIL] the Riverlands do not load: %s\n", error.c_str());
        return 2;
    }
    // the data's places, before the ground moves anyone
    std::map<uint64_t, Placed> placed;
    world.registry().for_each([&](const world::WorldActor& a) {
        if (a.kind != world::ActorKind::Npc && a.kind != world::ActorKind::Station) return;
        Placed p;
        p.id = a.id;
        p.name = a.npc.name;
        p.kind = a.kind;
        p.captured = a.npc.source == world::Source::Captured;
        p.talks = a.npc.dialogue_root != 0 || a.npc.vendor_guid != 0 || a.npc.interact_def != 0;
        p.by_data = a.npc.ground_by_data;
        p.by_terrain = a.npc.ground_by_terrain;
        p.flies = a.npc.flies;
        p.x = a.x;
        p.y = a.y;
        p.data_z = a.z;
        placed.emplace(a.id, std::move(p));
    });
    if (!world.load_ground(nav_path, motion_path, error)) {
        std::printf("  [FAIL] the ground does not load: %s\n", error.c_str());
        return 2;
    }
    nav::AreaNav* nav = const_cast<nav::AreaNav*>(world.nav());   // (the scan drives the window itself: no player walks)
    npc::NpcWorld* npcs = world.npcs();
    CHECK(nav && npcs, "the ground and the NPC world are there");
    if (!nav || !npcs) return 2;
    std::printf("    %zu NPCs and stations from the data, %zu of them waiting for their chunk\n", placed.size(), npcs->pending_ground());

    // every chunk an NPC stands in, loaded in turn: its NPCs are grounded and measured while it is resident
    std::map<std::pair<int, int>, std::vector<uint64_t>> by_chunk;
    for (const auto& [id, p] : placed) {
        const nav::ChunkKey k = nav->chunk_at(p.x, p.y);
        by_chunk[{k.x, k.y}].push_back(id);
    }
    const auto t_scan = std::chrono::steady_clock::now();
    size_t failed_chunks = 0;
    for (const auto& [key, ids] : by_chunk) {
        const Placed& first = placed.at(ids.front());
        if (!nav->load_around(first.x, first.y, 0)) ++failed_chunks;
        npcs->chunks_resident({nav::ChunkKey{key.first, key.second}});
        for (const uint64_t id : ids) {
            Placed& p = placed.at(id);
            const world::WorldActor* a = world.registry().find(id);
            if (!a) continue;
            p.z = a->z;
            p.chunk_ok = nav->ready_at(p.x, p.y);
            p.has_terrain = nav->terrain_height(p.x, p.y, &p.terrain);
            double gz = 0;
            p.surface_near = p.chunk_ok && nav->ground_height(p.x, p.y, p.data_z - kFeetCm, &gz, kReachCm, kReachCm);
            nav::Vec3 beside;
            if (p.chunk_ok && !p.surface_near && nav->nearest_point({p.x, p.y, p.data_z - kFeetCm}, 1000.0, 300.0, &beside) && beside.z >= p.data_z - kFeetCm - 300.0 && beside.z <= p.data_z - kFeetCm + 90.0)
                p.beside_cm = std::hypot(beside.x - p.x, beside.y - p.y);   // (NpcWorldConfig::data_ground_below_cm, above_cm less 60 cm: a big creature's half height is up to 150 cm, not 88)
        }
    }
    std::printf("    %zu chunks scanned in %.0f s (%zu did not load), %zu NPCs still waiting\n", by_chunk.size(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t_scan).count(), failed_chunks, npcs->pending_ground());
    CHECK(failed_chunks == 0 && npcs->pending_ground() == 0, "the ground under every NPC loaded, and every NPC was grounded");

    // why each one stands where it does
    std::map<std::string, size_t> count;
    std::map<std::string, size_t> count_talks;
    for (auto& [id, p] : placed) {
        const double dz = p.z - p.data_z;
        const double feet = p.data_z - kFeetCm;
        if (!p.chunk_ok) p.why = "chunk-not-loaded";
        else if (p.by_terrain) p.why = p.has_terrain && std::fabs(p.z - kFeetCm - p.terrain) > kReachCm ? "random-point-far-from-terrain" : "";   // (its data has no height: the terrain is its measure)
        else if (std::fabs(dz) <= kMovedCm) {
            if (p.surface_near || (p.beside_cm >= 0 && p.beside_cm <= 100.0)) p.why = "";
            else if (p.has_terrain && p.terrain > feet + kReachCm) p.why = "kept:under-terrain";        // a cave the ground has no floor of, or a height drawn under the map
            else if (p.has_terrain && p.terrain < feet - kReachCm) p.why = p.flies ? "" : "kept:over-terrain";   // a tower, a bridge, or the air
            else if (!p.has_terrain) p.why = "kept:no-terrain";
            else p.why = p.by_data ? "kept:town-floor" : "kept:no-surface";
        } else if (std::fabs(dz) <= kReachCm) p.why = dz > 0 ? "moved-up" : "moved-down";
        else p.why = dz > 0 ? "moved-up-far" : "moved-down-far";
        if (p.why.empty()) continue;
        ++count[p.why];
        if (p.talks) ++count_talks[p.why];
    }
    size_t odd = 0;
    for (const auto& [why, n] : count) {
        std::printf("    %-32s %6zu  (%zu of them talk, sell or are used)\n", why.c_str(), n, count_talks[why]);
        odd += n;
    }
    std::printf("    %zu of %zu stand on a surface within 3 m of their data's height\n", placed.size() - odd, placed.size());
    std::printf("    the people who talk, sell or are used and stand off their data's height or with no surface near:\n");
    size_t listed = 0;
    for (const auto& [id, p] : placed) {
        if (p.why.empty() || !p.talks) continue;
        if (++listed <= 80)
            std::printf("      %-22s %10llu %-34s (%9.0f, %9.0f) data z %8.0f  now %8.0f  terrain %8s\n", p.why.c_str(), static_cast<unsigned long long>(id), p.name.c_str(), p.x, p.y, p.data_z, p.z,
                        p.has_terrain ? std::to_string(static_cast<long long>(p.terrain)).c_str() : "-");
    }
    if (listed > 80) std::printf("      ... and %zu more (AOC_PLACEMENT_REPORT lists all)\n", listed - 80);
    if (env_report && *env_report) {
        if (FILE* f = std::fopen(env_report, "w")) {
            std::fprintf(f, "why\tid\tname\tkind\ttalks\tby_data\tby_terrain\tflies\tx\ty\tdata_z\tz\tterrain\tsurface_near\tcaptured\tbeside_cm\n");
            for (const auto& [id, p] : placed)
                std::fprintf(f, "%s\t%llu\t%s\t%s\t%d\t%d\t%d\t%d\t%.0f\t%.0f\t%.0f\t%.0f\t%s\t%d\t%d\t%.0f\n", p.why.c_str(), static_cast<unsigned long long>(id), p.name.c_str(),
                             p.kind == world::ActorKind::Station ? "station" : "npc", p.talks, p.by_data, p.by_terrain, p.flies, p.x, p.y, p.data_z, p.z,
                             p.has_terrain ? std::to_string(static_cast<long long>(p.terrain)).c_str() : "", p.surface_near, p.captured, p.beside_cm);
            std::fclose(f);
            std::printf("    every NPC written to %s\n", env_report);
        }
    }
    // Maryl Amberstone, the quest's receiver in the Anvils' halls: on her data's height, not the mountain's
    bool maryl_found = false, maryl_ok = false;
    for (const auto& [id, p] : placed)
        if (p.name == "Maryl Amberstone") {
            maryl_found = true;
            maryl_ok = std::fabs(p.z - p.data_z) <= kMovedCm;
        }
    CHECK(maryl_found && maryl_ok, "Maryl Amberstone (A Heavy Load) stands at her data's height in the Anvils' halls, not on the mountain over them");
    // an NPC with a walkable floor a step beside it, at its height, stands on that floor (NpcWorld::ground_npc's near search), not on the terrain 15 m and more under it
    size_t dropped_beside = 0;
    for (const auto& [id, p] : placed)
        if (p.why == "moved-down-far" && p.beside_cm >= 0 && p.beside_cm <= 100.0) ++dropped_beside;
    std::printf("    %zu NPCs went 15 m and more down with a floor within 1 m beside them at their height\n", dropped_beside);
    CHECK(dropped_beside == 0, "no NPC goes down to the terrain from a floor a step beside it (within 1 m, at its height)");
    // (not a check: an NPC drawn more than 15 m over the terrain with no floor near it goes down to the terrain, NpcWorld's rule for a placement drawn at the wrong height.  Whether the data
    // or the baked ground is wrong there is not known from the data alone: a rock or a building the ground has no floor of, or a height that is wrong; the list above names them)
    size_t far = 0;
    for (const auto& [why, n] : count)
        if (why == "moved-down-far" || why == "moved-up-far") far += n;
    std::printf("    %zu NPCs went more than 15 m off their data's height (to the terrain, no floor near their own)\n", far);

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
