// SPDX-License-Identifier: AGPL-3.0-only
// Item-bearing ability requests are authorized from server inventory and content.
// Included inside aoc::worldd by player_session.cpp.

bool PlayerSession::prepare_consumable(Connection& conn, int64_t ability, int32_t instance, int slot, uint32_t location,
                                      const repl::ItemInstance& claimed, TriggerInfo& info, Clock::time_point now) {
    const auto refuse = [&](const std::string& reason) {
        ++uses_refused_;
        spdlog::info("[consumable] {}: ability {} refused: {}", name(), ability, reason);
        end_instance(conn, instance, false);
        return false;
    };
    if (location != static_cast<uint32_t>(repl::ItemLocation::Backpack)) return refuse("the item is not in the Backpack");
    const svc::InventoryItem* held = inventory_.at(svc::ItemContainer::Backpack, slot);
    if (!held || held->quantity <= 0 || !claimed.has_item || held->instance_guid != items::guid_hex(claimed.guid)
        || static_cast<uint64_t>(held->item_record_id) != claimed.record_id)
        return refuse("the named item does not match the owned instance in that slot");
    const content::ContentDb* db = world_ ? world_->content() : nullptr;
    if (!db || !pipeline_) return refuse("the consumable runtime is unavailable");
    std::string error;
    const auto activation = db->item_use(held->item_record_id, error);
    if (!error.empty()) return refuse(error);
    if (!activation || activation->activation != "CastAbility") return refuse("the item has no CastAbility activation");
    const auto configured = db->item_cast(held->item_record_id, error);
    if (!error.empty()) return refuse(error);
    if (!configured || *configured != ability) return refuse("the requested ability is not configured for this item");
    if (activation->disabled_in_combat && combat_flag_) return refuse("the item cannot be used in combat");
    const auto gear = db->item_gear(held->item_record_id, error);
    if (!error.empty()) return refuse(error);
    if (gear) {
        for (const stats::BlockStat& stat : gear->stats) {
            const double value = gear->stat_at(stat, static_cast<double>(held->quality), stat.whole);
            if (!std::isfinite(value)) return refuse("the item has an invalid potency");
            info.item_stats[stat.stat] = value;
        }
    }
    const std::string guid = held->instance_guid;
    const int64_t record = held->item_record_id;
    const bool consume = activation->consume_on_use, disabled_in_combat = activation->disabled_in_combat;
    info.consume_item = [this, slot, guid, record, consume, disabled_in_combat]() {
        if (finished_ || failed_ || dead_ || (disabled_in_combat && combat_flag_)) return false;
        const svc::InventoryItem* item = inventory_.at(svc::ItemContainer::Backpack, slot);
        if (!item || item->instance_guid != guid || item->item_record_id != record || item->quantity < 1) return false;
        if (consume) {
            items::Outcome outcome = inventory_.consume(svc::ItemContainer::Backpack, slot, guid, 1);
            if (!outcome.ok) return false;
            // A disconnect before the client reply must still save the spent item.
            inventory_dirty_ = true;
            inventory_changed_ = true;
            pending_consumables_.push_back(std::move(outcome));
        }
        ++uses_applied_;
        spdlog::info("[consumable] {}: item {} used once", name(), record);
        return true;
    };
    return true;
}
