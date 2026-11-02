-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Guilds: one row per guild. worldd holds the guilds while it runs and writes a guild whole after a change
-- (CharacterService.SaveGuild); it reads them all once at its start (ListGuilds). `data` is worldd's: its ranks and
-- members. Characters are named inside it by guid, with no foreign key: a deleted character's row stays until worldd
-- next changes the guild, as the name and level it shows are only a record of what the member was.
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing
-- in another table depends on it, and dropping it loses the guilds and nothing else (services/README.md, "Rolling
-- back migration 0023").

SET LOCAL lock_timeout = '10s';

CREATE TABLE guilds (
    guid       TEXT        PRIMARY KEY
               CONSTRAINT guilds_guid CHECK (guid ~ '^[0-9a-f]{32}$'),
    name       TEXT        NOT NULL
               CONSTRAINT guilds_name CHECK (char_length(name) BETWEEN 1 AND 64),
    data       JSONB       NOT NULL,
    updated_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

CREATE UNIQUE INDEX guilds_name_lower ON guilds (lower(name));
