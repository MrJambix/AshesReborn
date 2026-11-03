-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- The chat ignore list survives a restart: the names a character ignores.
--
-- One NEW table, character_ignored_names: one row per name a character ignores, as the character typed it (or as the ignored character spells it, when he was online), with its
-- place in the list. worldd keeps the list while a character plays, shows it in the ignore list window (UOwnerInfoComponent.IgnoredCharacterNames) and saves it with the progress
-- (CharacterService.SaveProgress with replace_ignored_names). Before this the list lasted only while worldd ran.
--
-- Nothing is backfilled: a character that ignores no one has no row.
--
-- This ADDS ONE TABLE AND ONLY WRITES TO IT: no existing table, row, column or constraint is touched, nothing is dropped. An older character service ignores it (the names stay in
-- the table). Rolling it back: DROP TABLE character_ignored_names; and DELETE FROM schema_migrations WHERE version = 25 (see services/README.md).

SET LOCAL lock_timeout = '10s';

CREATE TABLE character_ignored_names (
    character_id TEXT    NOT NULL REFERENCES characters (id) ON DELETE CASCADE,
    name         TEXT    NOT NULL
                 CONSTRAINT character_ignored_names_name CHECK (char_length(name) BETWEEN 1 AND 64),
    position     INTEGER NOT NULL
                 CONSTRAINT character_ignored_names_position CHECK (position >= 0),
    CONSTRAINT character_ignored_names_pkey PRIMARY KEY (character_id, name),
    CONSTRAINT character_ignored_names_order UNIQUE (character_id, position)
);

COMMENT ON TABLE character_ignored_names IS 'The names a character ignores in chat, in the order they were added. worldd saves them with the progress (SaveProgress).';
COMMENT ON COLUMN character_ignored_names.position IS 'The place in the character''s list, from 0.';
