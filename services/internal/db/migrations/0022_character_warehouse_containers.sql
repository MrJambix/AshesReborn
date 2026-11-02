-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Storage clerks, buying room: the warehouse containers a character bought.
--
-- One NEW table, character_warehouse_containers. The keys are those of character_warehouse_items (migration 0017): an item chunk (area 'Items', node_guid 0, the same in every town), a material
-- tab or a cargo tab (area 'Materials' or 'Cargo', the town's record guid). The free containers have no row. worldd saves the rows in the SaveInventory call that saves the money paid for them
-- (CharacterService.SaveInventory with replace_warehouse), and a save only adds: nothing ever removes a row but the character's deletion.
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing is dropped. Rolling it back is a documented block in services/README.md ("Rolling
-- back migration 0022").

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_warehouse_containers (
    character_id   TEXT   NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    area           TEXT   NOT NULL
                   CONSTRAINT character_warehouse_containers_area CHECK (area IN ('Items', 'Materials', 'Cargo')),
    node_guid      BIGINT NOT NULL DEFAULT 0
                   CONSTRAINT character_warehouse_containers_node CHECK (node_guid >= 0),
    container_guid BIGINT NOT NULL
                   CONSTRAINT character_warehouse_containers_container_positive CHECK (container_guid > 0),
    bought_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT character_warehouse_containers_where
        CHECK ((area = 'Items' AND node_guid = 0) OR (area <> 'Items' AND node_guid > 0)),
    CONSTRAINT character_warehouse_containers_pkey PRIMARY KEY (character_id, area, node_guid, container_guid)
);

COMMENT ON TABLE character_warehouse_containers IS 'Storage clerks, buying room: the warehouse containers a character bought, one row each. Free containers have no row.';
