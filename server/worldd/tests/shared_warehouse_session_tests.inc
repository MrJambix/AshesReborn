// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// The shared warehouse (included by test_worldd_area_session.cpp): two characters of one account, online at once, hold one Bank; the first join puts the legacy items in; a later login's
// older read of the warehouse is not used; the save carries the revision and the merged instances.

static void test_shared_warehouse() {
    std::printf("\n-- the shared warehouse: the characters of an account hold one warehouse --\n");
    constexpr int64_t kChunkRecord = 6064631806178426880ll, kNodeRecord = 108646121231993ll;
    content::WarehouseData d;
    d.nodes[kNodeRecord] = {/*resource_def=*/1, /*crate_def=*/2, /*chunks_def=*/3};
    d.chunk_defs[3] = {kChunkRecord};
    d.chunks[kChunkRecord] = {kChunkRecord, "Default", 40, 0, "", {}};
    d.defs[1] = {1, "Default", 8, 16, {}};
    const warehouse::Key chunk{warehouse::Area::Items, 0, kChunkRecord};

    svc::WarehouseItem old_potion;   // what the main kept in its own warehouse before it was shared
    old_potion.area = "Items";
    old_potion.container_guid = kChunkRecord;
    old_potion.item = make_item(Bag, 2, fi::kPotion, 901, 3);
    svc::Character main_record = cyb_record();
    main_record.shared_warehouse = svc::SharedWarehouse{};
    main_record.shared_warehouse->revision = 4;
    main_record.shared_warehouse->legacy = {old_potion};
    Harness main_player(main_record);
    main_player.fx.world->set_warehouses(warehouse::Book(d));
    main_player.start();
    const warehouse::Bank& bank = main_player.s->warehouse_bank();
    CHECK(bank.shared && bank.legacy.empty() && bank.storage.at(chunk, 2) && bank.storage.at(chunk, 2)->instance_guid == old_potion.item.instance_guid,
          "the first join of the account puts the legacy item in its own place");
    CHECK(bank.revision == 5 && bank.merged.size() == 1, "the merge counts one change and names the instance for the save");

    svc::Character alt_record = cyb_record();
    alt_record.id = "0f1e2d3c4b5a69788796a5b4c3d2e1f0";
    alt_record.name = "cybalt";
    alt_record.shared_warehouse = svc::SharedWarehouse{};   // (what its login read: the warehouse before the main's merge was saved)
    Harness alt(alt_record);
    alt.start(main_player.fx.world.get());
    CHECK(&alt.s->warehouse_bank() == &bank && main_player.fx.world->warehouse_banks().size() == 1, "a second character of the account online at once holds the same warehouse");
    CHECK(alt.s->warehouse_storage().at(chunk, 2) != nullptr, "and sees the item, not its login's older read");

    main_player.s->finish("the test ends");
    const std::vector<svc::WarehouseItem>* saved = main_player.sink.warehouses.empty() ? nullptr : &main_player.sink.warehouses.back();
    int64_t revision = -1;
    size_t merged = 0, stored = 0;
    for (const svc::WarehouseItem& w : saved ? *saved : std::vector<svc::WarehouseItem>{}) {
        if (w.meta == svc::WarehouseItem::Meta::Revision) revision = w.revision;
        else if (w.meta == svc::WarehouseItem::Meta::Merged) merged += w.item.instance_guid == old_potion.item.instance_guid ? 1 : 0;
        else if (!w.container_only) ++stored;
    }
    CHECK(saved && revision == 5 && merged == 1 && stored == 1, "the main's save carries the warehouse at revision 5 and names the merged instance");
    alt.s->finish("the test ends");
}
