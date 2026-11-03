-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- The player marketplace: one row per listing ("l" and its guid) and per storefront description ("s", the node, ":" and the
-- seller's character id). worldd holds the marketplace while it runs and writes the rows a character's act changed in that
-- character's SaveInventory, in the same transaction as its items and wallet (a listed item leaves the bags and enters the
-- market together). The row with the higher revision is kept; `gone` marks a row worldd removed (a tombstone, so an older
-- save that arrives later cannot bring it back); ListMarketplace deletes the tombstones when worldd starts. `data` is worldd's.
-- Sellers are named inside `data` by character id, with no foreign key: a listing of a deleted character stays until removed.
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing in another
-- table depends on it, and dropping it loses the listings (and the items held in them) and nothing else.

SET LOCAL lock_timeout = '10s';

CREATE TABLE marketplace_rows (
    id         TEXT        PRIMARY KEY
               CONSTRAINT marketplace_rows_id CHECK (id ~ '^(l[0-9a-f]{32}|s-?[0-9]{1,19}:[0-9a-f]{32})$'),
    revision   BIGINT      NOT NULL CONSTRAINT marketplace_rows_revision CHECK (revision >= 0),
    gone       BOOLEAN     NOT NULL DEFAULT false,
    data       JSONB       NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
