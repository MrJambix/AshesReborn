-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- The shared warehouse: every character of an account uses one warehouse.
--
-- Three NEW tables, keyed by the account where 0017 and 0022 key by the character:
--   * account_warehouses: one row per account that saved its warehouse, with the revision worldd last wrote. worldd keeps one warehouse per account while any of its characters plays
--     and counts each change; a save (CharacterService.SaveInventory with replace_warehouse and warehouse_revision) writes the warehouse only when its revision is above the stored one,
--     so the save of a character that moved nothing lately, arriving after another character's newer one, changes nothing.
--   * account_warehouse_items: the items, with the columns and the rules of character_warehouse_items. The item chunks are the same in every town; a material or cargo tab is a town's,
--     so a character sees the account's tabs of a town in that town only.
--   * account_warehouse_containers: what the account bought, with the columns of character_warehouse_containers. Backfilled below: what any character of the account bought, each once.
--
-- The items of character_warehouse_items are NOT copied here: two characters of an account may hold items in the same place, and only worldd knows the shapes of the containers and the
-- items. worldd gets them with the account's warehouse (legacy_warehouse_items), puts each where it fits, and names the ones it put in its save (merged_legacy_warehouse_guids): those rows
-- are deleted from character_warehouse_items in the transaction that writes them here. One that does not fit stays where it is and is tried again.
--
-- This ADDS THREE TABLES AND ONLY WRITES TO THEM (and deletes the merged rows of character_warehouse_items, at run time, never here): no existing table, row, column or constraint is
-- touched, nothing is dropped. Rolling it back is a documented block in services/README.md ("Rolling back migration 0026").

SET LOCAL lock_timeout = '10s';

CREATE TABLE account_warehouses (
    account_id BIGINT      PRIMARY KEY REFERENCES accounts (id) ON DELETE CASCADE,
    revision   BIGINT      NOT NULL DEFAULT 0
               CONSTRAINT account_warehouses_revision CHECK (revision >= 0),
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE TABLE account_warehouse_items (
    account_id     BIGINT      NOT NULL REFERENCES accounts (id) ON DELETE CASCADE,
    area           TEXT        NOT NULL
                   CONSTRAINT account_warehouse_items_area CHECK (area IN ('Items', 'Materials', 'Cargo')),
    node_guid      BIGINT      NOT NULL DEFAULT 0
                   CONSTRAINT account_warehouse_items_node CHECK (node_guid >= 0),
    container_guid BIGINT      NOT NULL
                   CONSTRAINT account_warehouse_items_container_positive CHECK (container_guid > 0),
    slot           INT         NOT NULL
                   CONSTRAINT account_warehouse_items_slot CHECK (slot >= 0 AND slot < 1024),
    item_record_id BIGINT      NOT NULL
                   CONSTRAINT account_warehouse_items_record_positive CHECK (item_record_id > 0),
    instance_guid  TEXT        NOT NULL
                   CONSTRAINT account_warehouse_items_guid_hex CHECK (instance_guid ~ '^[0-9a-f]{32}$' AND instance_guid <> repeat('0', 32)),
    quantity       INT         NOT NULL DEFAULT 1
                   CONSTRAINT account_warehouse_items_quantity CHECK (quantity BETWEEN 1 AND 1000000),
    quality        INT         NOT NULL DEFAULT 1
                   CONSTRAINT account_warehouse_items_quality CHECK (quality BETWEEN 1 AND 200),
    durability     REAL        NOT NULL DEFAULT 0
                   CONSTRAINT account_warehouse_items_durability CHECK (durability >= 0 AND durability <= 1e9),   -- NaN is above 1e9 in PostgreSQL
    created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
    persisted_health REAL,
    persisted_mana   REAL,
    lock_expiration  BIGINT    NOT NULL DEFAULT 0,
    CONSTRAINT account_warehouse_items_where
        CHECK ((area = 'Items' AND node_guid = 0) OR (area <> 'Items' AND node_guid > 0)),
    CONSTRAINT account_warehouse_items_persisted_health CHECK (persisted_health IS NULL OR (persisted_health >= 0 AND persisted_health <= 1e9)),
    CONSTRAINT account_warehouse_items_persisted_mana   CHECK (persisted_mana   IS NULL OR (persisted_mana   >= 0 AND persisted_mana   <= 1e9)),
    CONSTRAINT account_warehouse_items_lock_expiration  CHECK (lock_expiration >= 0 AND lock_expiration <= 253402300799),
    CONSTRAINT account_warehouse_items_pkey PRIMARY KEY (account_id, area, node_guid, container_guid, slot),
    CONSTRAINT account_warehouse_items_guid_unique UNIQUE (account_id, instance_guid)
);

CREATE TABLE account_warehouse_containers (
    account_id     BIGINT NOT NULL REFERENCES accounts (id) ON DELETE CASCADE,
    area           TEXT   NOT NULL
                   CONSTRAINT account_warehouse_containers_area CHECK (area IN ('Items', 'Materials', 'Cargo')),
    node_guid      BIGINT NOT NULL DEFAULT 0
                   CONSTRAINT account_warehouse_containers_node CHECK (node_guid >= 0),
    container_guid BIGINT NOT NULL
                   CONSTRAINT account_warehouse_containers_container_positive CHECK (container_guid > 0),
    bought_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT account_warehouse_containers_where
        CHECK ((area = 'Items' AND node_guid = 0) OR (area <> 'Items' AND node_guid > 0)),
    CONSTRAINT account_warehouse_containers_pkey PRIMARY KEY (account_id, area, node_guid, container_guid)
);

-- what the account's characters bought becomes the account's (the first purchase's time kept)
INSERT INTO account_warehouse_containers (account_id, area, node_guid, container_guid, bought_at)
SELECT c.account_id, w.area, w.node_guid, w.container_guid, min(w.bought_at)
FROM character_warehouse_containers w JOIN characters c ON c.id = w.character_id
GROUP BY c.account_id, w.area, w.node_guid, w.container_guid;

COMMENT ON TABLE account_warehouses IS 'The shared warehouse: the revision of each account''s warehouse worldd last wrote. A save with a revision not above it leaves the warehouse alone.';
COMMENT ON TABLE account_warehouse_items IS 'The shared warehouse: the items an account keeps in the warehouse, one row per instance, for all its characters. worldd saves them with a character''s items (SaveInventory).';
COMMENT ON TABLE account_warehouse_containers IS 'The shared warehouse: the containers an account bought, one row each. Free containers have no row; nothing removes a row but the account''s deletion.';
