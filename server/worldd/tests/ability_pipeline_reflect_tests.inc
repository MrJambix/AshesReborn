// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  tests/ability_pipeline_reflect_tests.inc: the Tank's Reflect braces for its recorded 2.9 s, whatever the key does, and its effects then go
//  (registered with AOC_REGISTER_TEST: test_worldd_ability_pipeline.cpp runs it through aoc_tests::run; it is included from ability_pipeline_registered.inc)
// ============================================================================

void test_tank_reflect_ends_by_itself() {
    std::printf("Reflect: its block mitigation lasts the recorded 2.9 s with no cancel, an early release does not end it sooner, and then it goes\n");
    // release 0: the client sends nothing (the retail recordings); 1: a cancel before the effects start; 2: a cancel while they are on; 3: an end request
    for (int release = 0; release < 4; ++release) {
        Rig r(7, 25);
        CHECKF(r.ok(), "the Tank rig: %s", r.problem.c_str());
        if (!r.ok()) return;
        const int64_t brace = r.effect("Tank_Reflect"), buff = r.effect("Tank_Reflect_BlockMitigation");
        CHECK(brace != 0 && buff != 0, "(the data has Reflect's two effects)");
        r.know({"Tank_Reflect"});
        CHECKF(r.trigger("Tank_Reflect", 1.0) == Refusal::None, "release %d: Reflect starts", release);
        const int32_t client_id = r.next_client_id - 1;
        if (release == 1) r.pipe->cancel(client_id, 1.01);
        r.run_to(1.5);
        if (release == 2) r.pipe->cancel(client_id, 1.5);
        if (release == 3) {
            int64_t instance = -1;
            for (const auto& [id, item] : r.pawn.abilities)
                if (item.ability_guid == r.ability("Tank_Reflect")) instance = item.server_instance_id;
            CHECK(instance >= 0 && r.pipe->end_request(static_cast<int32_t>(instance), 1.5), "the end request is taken");
        }
        r.run_to(3.8);
        // (the rig's client asks for every hit of the cast at once: Tank_Deflect_RemoveBuff, the reflection's, takes the hidden brace off at the start, as the
        // first reflected attack does [R: capture 1065 line 17257, one effect gone at +925 ms]; the block mitigation stays the whole time)
        CHECKF(r.pipe->stacks_of(buff) == 1, "release %d: the block mitigation is still on at +2.8 s", release);
        CHECKF(r.pawn.effect(buff) != nullptr, "release %d: the client still shows the buff at +2.8 s", release);
        r.run_to(4.2);
        CHECKF(r.pipe->stacks_of(brace) == 0 && r.pipe->stacks_of(buff) == 0, "release %d: both are gone by +3.2 s", release);
        CHECKF(r.pawn.effect(buff) == nullptr && r.pawn.effect(brace) == nullptr, "release %d: and the client is told", release);
        CHECK(r.pawn.broken.empty(), "(the pawn took every change)");
    }
}
AOC_REGISTER_TEST(test_tank_reflect_ends_by_itself);
