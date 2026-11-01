-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors.
CREATE TABLE account_combat_settings (
    account_id BIGINT PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    auto_attack JSONB NOT NULL DEFAULT '{}'::jsonb,
    CHECK (jsonb_typeof(auto_attack) = 'object'),
    CHECK (auto_attack - ARRAY['bEnabled', 'bToggle', 'bStartsAfterMeleeAbility',
        'bStartsAfterRangedAbility', 'bStopsAfterAbility'] = '{}'::jsonb)
);
