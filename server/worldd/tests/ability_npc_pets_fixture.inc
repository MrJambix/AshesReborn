// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// Summoner pets: contributed by MrJambix in #13.
// ============================================================================
//  tests/ability_npc_pets_fixture.inc: what core's pet check files (ability_npc_pets_*_tests.inc) share.  Core keeps it at the head of ability_npc_pets_tests.inc, whose
//  checks this tree has inline in test_worldd_ability_npc.cpp; included first from ability_npc_registered.inc.
// ============================================================================

/// What every world of these tests is made from: the Blueprints' table and the Wolf's path.
struct PetTestData {
    npc::MotionTable motion;
    std::string      wolf_path;
    bool             ok = false;
    PetTestData() {
        std::string error;
        ok = motion.load(NPC_MOTION_PATH, error);
        CHECKF(ok, "npc_motion.tsv: %s", error.c_str());
        for (const auto& [p, row] : motion.rows()) {
            if (p.find("/Beast/Canine/Wolf.Default__Wolf_C") != std::string::npos) wolf_path = p;
        }
    }
};

/// The effect the first of a hit's effects puts on its target; 0 when the hit or its effects are not there (a check on that effect's stacks then fails, and the run goes on).
static int64_t first_effect_of(const abilities::Catalog& catalog, int64_t hit) {
    const abilities::HitDef* found = catalog.hit(hit);
    return found && !found->effects.empty() ? found->effects.front().effect_guid : 0;
}
