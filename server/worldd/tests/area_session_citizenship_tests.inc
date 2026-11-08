// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  server/worldd/tests/area_session_citizenship_tests.inc  (listed in tests/area_session_registered.inc)
//
//  Node citizenship through a session (session/player_citizenship.cpp): the Node Administrator's line opens the join dialog for its node with the cost, the dues' date
//  and the shop of the tax receipts; an application for another node, or before any offer, is refused with nothing paid; the application takes the cost in the
//  dues currency, makes the account a citizen and saves it with the progress; the window's data names the player as the primary character; a second offer is
//  refused; dues paid ahead are refused, overdue dues cost the late fee too and move the date on; a renouncement in another's name is refused, one in the player's
//  own ends the citizenship.  The world's content.db here has no city nodes or currencies: the test gives the world one node over the zone of influence where the
//  player stands, and a tax receipt currency carried as the character's potions.
// ============================================================================
#include "items/currency.h"
#include "protocol/repl/citizenship_codec.h"
#include "world/citizenship.h"
#include "world/zoi_surface.h"

namespace {

constexpr int64_t kTestCitizenNode = 7770001;
constexpr int64_t kTestReceiptTier = 7770002;
constexpr int64_t kTestReceiptDef = 7770003;
constexpr int64_t kTestReceiptShop = 7770004;

struct CitizenHarness : Harness {
    uint64_t admin = 0;
    std::unique_ptr<world::CityNodes> nodes;
    bool zoned = false;

    static svc::Character citizen_record() {
        svc::Character c = cyb_record();
        c.level = 12;
        return c;
    }
    CitizenHarness() : Harness(citizen_record()) {
        // one node over the zone of influence at the player's spot, at Town (4)
        const uint64_t zoi = world::zoi_at(kCx, kCy);
        zoned = zoi != 0;
        world::CityNodeInfo n;
        n.rank = 0;
        n.guid = kTestCitizenNode;
        n.name = "Verra_Test_Node";
        n.display_name = "Testhold";
        n.x = kCx;
        n.y = kCy;
        n.zoi_guid = static_cast<int64_t>(zoi);
        world::CityZoneInfo z;
        z.guid = static_cast<int64_t>(zoi);
        z.name = "ZOI_Test";
        z.node_guid = kTestCitizenNode;
        z.node_rank = 0;
        nodes = std::make_unique<world::CityNodes>(std::vector<world::CityNodeInfo>{n}, std::vector<world::CityZoneInfo>{z});
        nodes->apply_change(world::LiveNode{kTestCitizenNode, 4, 0, 0});
        fx.world->set_city_nodes(nodes.get());
        world::CitizenshipRules rules;
        rules.dues_tier = kTestReceiptTier;
        rules.join_cost = 3;
        rules.dues_cost = 1;
        rules.late_fee = 1;
        rules.cooldown_secs = {{3, 0}, {4, 3600}};
        fx.world->set_citizenship_rules(rules);
        content::CurrencyData data;
        data.tiers.push_back({kTestReceiptTier, "TaxReceipts", 1000000});
        data.defs.push_back({kTestReceiptDef, "TaxReceiptCurrencyDef", kTestReceiptTier, 0, fi::kPotion, 20});
        std::string why;
        if (auto table = items::CurrencyTable::from(data, why)) fx.world->set_currencies_for_test(std::move(*table));
        else std::printf("  [FAIL] the test's currency does not build: %s\n", why.c_str());
        const spawns::OpenTemplate* t = fx.npc_template();
        if (!t) {
            std::printf("  [FAIL] no NPC template\n");
            std::exit(2);
        }
        admin = fx.add(*t, kCx + 300, kCy, false, 0, "Node Administrator");
        start();
    }
    const world::WorldActor& npc() const { return *fx.world->registry().find(admin); }
    template <class F>
    void pc_call(const char* rpc, F fill) {
        const schema::ClassSchema& pc = class_named("AoCPlayerControllerBP_C");
        const schema::RpcSchema* r = pc.rpc(rpc);
        if (!r) {
            std::printf("  [FAIL] no RPC %s\n", rpc);
            return;
        }
        repl::RpcParams p;
        fill(*r, p);
        client_request(actor_rpc(pc, rpc, p), pc_ch);
    }
    void apply(int64_t node, int64_t dues) {
        pc_call("ServerSendCitizenshipApplication", [&](const schema::RpcSchema& r, repl::RpcParams& p) {
            repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(node));
            repl::set_rpc_param(r, p, "NumDuesToFulfill", repl::Value::i64(dues));
        });
    }
};

int64_t call_int(const repl::RpcCall& c, const char* path) { return repl::rpcf::int_or(repl::rpcf::value_of(*c.rpc, c.params, path), 0).value_or(-1); }

const repl::CitizenshipWindowData* window_of(const repl::RpcCall* c) {
    if (!c) return nullptr;
    const repl::Value* v = repl::rpcf::value_of(*c->rpc, c->params, "EssentialCitizenshipData");
    return v ? v->get_if<repl::CitizenshipWindowData>() : nullptr;
}

void test_citizenship_flow() {
    std::printf("node citizenship through a session\n");
    CitizenHarness h;
    h.play();
    h.run(milliseconds(1500));
    spdlog::set_level(std::getenv("AOC_TEST_CITIZEN_LOG") ? spdlog::level::info : spdlog::level::off);
    CHECK(h.zoned, "the player stands in a zone of influence (the test's node is over it)");
    CHECK(h.s->inventory().count_of(fi::kPotion) == 5, "the character carries 5 potions, the test's tax receipts");

    // the window before joining: no citizenship
    size_t from = h.got.size();
    h.pc_call("ServerSetEssentialCitizenshipWindowData", [](const schema::RpcSchema&, repl::RpcParams&) {});
    h.run(milliseconds(300));
    auto calls = pc_calls_since(h, from);
    const repl::CitizenshipWindowData* before = window_of(last_call(calls, "ClientPopulateCitizenshipData"));
    CHECK(before && before->status.primary.name.empty() && before->dues_due_time == 0, "the window's data is answered: no citizenship yet");

    // an application before any offer: refused, nothing paid
    from = h.got.size();
    h.apply(kTestCitizenNode, 3);
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientCitizenshipApplicationRejection") == 1 && h.s->citizenship().node_guid == 0 && h.s->inventory().count_of(fi::kPotion) == 5,
          "an application no administrator offered is refused, with nothing paid");

    // the administrator's line: the join dialog
    from = h.got.size();
    h.s->open_citizenship_dialog_for_test(h.conn, h.npc(), /*join=*/true, kTestReceiptShop, h.t);
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    const repl::RpcCall* dialog = last_call(calls, "ClientShowAddNodeCitizenshipDialog");
    CHECK(dialog && call_int(*dialog, "NodeGuid") == kTestCitizenNode && call_int(*dialog, "CitizenshipCost") == 3 && call_int(*dialog, "VendorDefRecordGuid") == kTestReceiptShop,
          "the line opens the join dialog: the node, a cost of 3 and the shop of the tax receipts");
    const int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    CHECK(dialog && call_int(*dialog, "CitizenshipDuesDueTime") >= now_s + 14 * 86400 && call_int(*dialog, "CitizenshipDuesDueTime") < now_s + 21 * 86400,
          "... with the first dues due on a due day two to three weeks away (Unix seconds)");

    // an application for another node: refused
    from = h.got.size();
    h.apply(kTestCitizenNode + 1, 3);
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientCitizenshipApplicationRejection") == 1 && h.s->citizenship().node_guid == 0, "an application for another node is refused");

    // the application
    const size_t saves = h.sink.progress.size();
    from = h.got.size();
    h.apply(kTestCitizenNode, 3);
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    const repl::RpcCall* ok = last_call(calls, "ClientCitizenshipApplicationSuccessful");
    const repl::RpcCall* toast = last_call(calls, "ClientShowCitizenshipToast");
    CHECK(ok && call_int(*ok, "NumDuesFulfilled") == 3, "the application succeeds: 3 dues fulfilled");
    CHECK(toast && call_int(*toast, "NodeGuid") == kTestCitizenNode, "... with the citizenship toast for the node");
    CHECK(count_calls(calls, "ClientReceiveStorageEvent") >= 1 && h.s->inventory().count_of(fi::kPotion) == 2, "the 3 tax receipts leave the bags (5 -> 2), told as storage events");
    CHECK(h.s->citizenship().node_guid == kTestCitizenNode && h.s->citizenship().primary_character_id == kCyb && h.s->citizenship().joined_ms > 0 &&
              h.s->citizenship().dues_due_ms > h.s->citizenship().joined_ms,
          "the account is a citizen of the node, the player its primary character, with dues due later");
    bool saved = false;
    for (size_t i = saves; i < h.sink.progress.size(); ++i) saved = saved || (h.sink.progress[i].replace_citizenship && h.sink.progress[i].citizenship.node_guid == kTestCitizenNode);
    CHECK(saved, "the citizenship is saved with the progress (replace_citizenship)");
    const svc::Citizenship* shared = h.fx.world->account_citizenship(h.record.account_id);
    CHECK(shared && shared->node_guid == kTestCitizenNode, "the world keeps the account's citizenship for its other logins");

    // the window: the player as the primary character
    from = h.got.size();
    h.t += milliseconds(500);   // (the request budget)
    h.pc_call("ServerSetEssentialCitizenshipWindowData", [](const schema::RpcSchema&, repl::RpcParams&) {});
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    const repl::CitizenshipWindowData* after = window_of(last_call(calls, "ClientPopulateCitizenshipData"));
    CHECK(after && after->status.primary.name == "cyb" && after->status.dues_required == 1 && after->status.player_level == 12 && after->dues_state == 0 &&
              after->dues_due_time == h.s->citizenship().dues_due_ms / 1000,
          "the window names the player as the primary character, 1 Tax Receipt of dues, awaiting the due date");

    // a second offer: the account is a citizen already
    from = h.got.size();
    h.s->open_citizenship_dialog_for_test(h.conn, h.npc(), true, kTestReceiptShop, h.t);
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientShowAddNodeCitizenshipDialog") == 0 && count_calls(calls, "ClientCitizenshipApplicationRejection") == 1, "a second offer is refused: a citizen already");

    // the dues: paid ahead is refused; overdue costs the late fee and moves the date on
    from = h.got.size();
    h.pc_call("ServerFulfillCitizenshipDues", [](const schema::RpcSchema& r, repl::RpcParams& p) { repl::set_rpc_param(r, p, "DuesRequired", repl::Value::i64(1)); });
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientCitizenshipDuesFulfillmentRejection") == 1 && h.s->inventory().count_of(fi::kPotion) == 2, "dues a whole period ahead are refused");
    svc::Citizenship overdue = h.s->citizenship();
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    overdue.dues_due_ms = now_ms - 10LL * 86400000;   // ten days ago: past the 5 overdue days
    h.s->set_citizenship_for_test(overdue);
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerFulfillCitizenshipDues", [](const schema::RpcSchema& r, repl::RpcParams& p) { repl::set_rpc_param(r, p, "DuesRequired", repl::Value::i64(2)); });
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    const repl::RpcCall* paid = last_call(calls, "ClientCitizenshipDuesFulfillmentSuccessful");
    CHECK(paid && call_int(*paid, "NumFulfilled") == 2 && h.s->inventory().count_of(fi::kPotion) == 0, "overdue dues cost the dues and the late fee: 2 Tax Receipts");
    CHECK(h.s->citizenship().dues_due_ms > now_ms, "... and the next date is after now");

    // the renouncement: another's name is refused; the player's own ends it
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerRemoveNodeCitizen", [](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(kTestCitizenNode));
        repl::set_rpc_param(r, p, "PlayerName", repl::Value::str("SomebodyElse"));
    });
    h.run(milliseconds(300));
    CHECK(h.s->citizenship().node_guid == kTestCitizenNode && h.s->citizenship_renounces() == 0, "a removal in another player's name is refused");
    from = h.got.size();
    h.t += milliseconds(500);
    h.pc_call("ServerRemoveNodeCitizen", [](const schema::RpcSchema& r, repl::RpcParams& p) {
        repl::set_rpc_param(r, p, "NodeGuid", repl::Value::i64(kTestCitizenNode));
        repl::set_rpc_param(r, p, "PlayerName", repl::Value::str("Cyb"));
    });
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    const repl::RpcCall* left = last_call(calls, "ClientShowCitizenshipToast");
    CHECK(left && call_int(*left, "NodeGuid") == kTestCitizenNode && h.s->citizenship().node_guid == 0 && h.s->citizenship_renounces() == 1,
          "the player's own renouncement (the name in any case) ends the citizenship, with the toast");
    CHECK(h.s->citizenship().renounce_cooldown_until_ms > now_ms, "... and the account waits the node level's cooldown before it may join again");
    from = h.got.size();
    h.s->open_citizenship_dialog_for_test(h.conn, h.npc(), true, kTestReceiptShop, h.t);
    h.run(milliseconds(300));
    calls = pc_calls_since(h, from);
    CHECK(count_calls(calls, "ClientShowAddNodeCitizenshipDialog") == 0, "no join dialog during the cooldown");
}

}  // namespace

AOC_REGISTER_TEST(test_citizenship_flow);
