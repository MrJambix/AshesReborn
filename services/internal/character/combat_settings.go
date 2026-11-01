// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors.
package character

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"

	"github.com/jackc/pgx/v5"
)

// Auto attack options belong to the account, including explicit false values.
func validateAutoAttackSettings(values map[string]bool) error {
	for key := range values {
		switch key {
		case "bEnabled", "bToggle", "bStartsAfterMeleeAbility", "bStartsAfterRangedAbility", "bStopsAfterAbility":
		default:
			return fmt.Errorf("%w: unknown auto attack setting %q", ErrInvalid, key)
		}
	}
	return nil
}

func (s *Store) AutoAttackSettings(ctx context.Context, accountID int64) (map[string]bool, error) {
	var data []byte
	err := s.pool.QueryRow(ctx, `SELECT COALESCE(s.auto_attack, '{}'::jsonb)
        FROM accounts a LEFT JOIN account_combat_settings s ON s.account_id = a.id
        WHERE a.id = $1`, accountID).Scan(&data)
	if errors.Is(err, pgx.ErrNoRows) {
		return nil, ErrNotFound
	}
	if err != nil {
		return nil, err
	}
	var values map[string]bool
	if err := json.Unmarshal(data, &values); err != nil {
		return nil, err
	}
	return values, nil
}

func saveAutoAttackSettings(ctx context.Context, tx pgx.Tx, accountID int64, values map[string]bool) error {
	if len(values) == 0 {
		return nil
	}
	data, err := json.Marshal(values)
	if err != nil {
		return err
	}
	_, err = tx.Exec(ctx, `INSERT INTO account_combat_settings (account_id, auto_attack) VALUES ($1, $2::jsonb)
        ON CONFLICT (account_id) DO UPDATE SET auto_attack = account_combat_settings.auto_attack || EXCLUDED.auto_attack`, accountID, string(data))
	return err
}
