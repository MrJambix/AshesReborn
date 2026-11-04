// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"encoding/json"
	"fmt"
	"regexp"

	"github.com/jackc/pgx/v5"
)

// MarketplaceRow is one row of the marketplace_rows table (migration 0024): a listing or a storefront. Data is worldd's own JSON;
// the store only checks that it is a JSON object. Gone marks a tombstone.
type MarketplaceRow struct {
	ID       string
	Revision int64
	Gone     bool
	Data     string
}

// MaxMarketplaceRows bounds the rows one save carries (an act changes one or two; collecting proceeds, one per listing that sold).
const MaxMarketplaceRows = 1000

var marketplaceIDPattern = regexp.MustCompile(`^(l[0-9a-f]{32}|s-?[0-9]{1,19}:[0-9a-f]{32})$`)

// Validate refuses what the table's constraints would, so the caller sees ErrInvalid rather than a database error.
func (r MarketplaceRow) Validate() error {
	if !marketplaceIDPattern.MatchString(r.ID) {
		return fmt.Errorf("%w: marketplace row id %q", ErrInvalid, r.ID)
	}
	if r.Revision < 0 {
		return fmt.Errorf("%w: marketplace row %s has revision %d", ErrInvalid, r.ID, r.Revision)
	}
	var object map[string]json.RawMessage
	if err := json.Unmarshal([]byte(r.Data), &object); err != nil {
		return fmt.Errorf("%w: marketplace row %s: data is not a JSON object", ErrInvalid, r.ID)
	}
	return nil
}

// ValidateMarketplaceRows checks every row and that no id comes twice.
func ValidateMarketplaceRows(rows []MarketplaceRow) error {
	if len(rows) > MaxMarketplaceRows {
		return fmt.Errorf("%w: %d marketplace rows in one save (at most %d)", ErrInvalid, len(rows), MaxMarketplaceRows)
	}
	seen := make(map[string]bool, len(rows))
	for _, r := range rows {
		if err := r.Validate(); err != nil {
			return err
		}
		if seen[r.ID] {
			return fmt.Errorf("%w: marketplace row %s twice in one save", ErrInvalid, r.ID)
		}
		seen[r.ID] = true
	}
	return nil
}

// ListMarketplace deletes the tombstones and returns every row left, in id order. worldd calls it once at its start, when no save of
// an earlier run can still arrive, so the tombstones have done their work.
func (s *Store) ListMarketplace(ctx context.Context) ([]MarketplaceRow, error) {
	if _, err := s.pool.Exec(ctx, `DELETE FROM marketplace_rows WHERE gone`); err != nil {
		return nil, err
	}
	rows, err := s.pool.Query(ctx, `SELECT id, revision, gone, data::text FROM marketplace_rows ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []MarketplaceRow
	for rows.Next() {
		var r MarketplaceRow
		if err := rows.Scan(&r.ID, &r.Revision, &r.Gone, &r.Data); err != nil {
			return nil, err
		}
		out = append(out, r)
	}
	return out, rows.Err()
}

// upsertMarketplace writes rows inside tx, each only where the stored row's revision is lower (or there is none).
func upsertMarketplace(ctx context.Context, tx pgx.Tx, rows []MarketplaceRow) error {
	for _, r := range rows {
		if _, err := tx.Exec(ctx, `
			INSERT INTO marketplace_rows (id, revision, gone, data) VALUES ($1, $2, $3, $4::jsonb)
			ON CONFLICT (id) DO UPDATE SET revision = EXCLUDED.revision, gone = EXCLUDED.gone, data = EXCLUDED.data, updated_at = now()
			WHERE marketplace_rows.revision < EXCLUDED.revision`,
			r.ID, r.Revision, r.Gone, r.Data); err != nil {
			return err
		}
	}
	return nil
}

// SaveInventoryWithMarketplace is SaveInventoryEverything that also writes the marketplace rows the character's act changed, in the
// same transaction. ErrInvalid, with nothing changed, for rows ValidateMarketplaceRows refuses.
func (s *Store) SaveInventoryWithMarketplace(ctx context.Context, characterID string, items []Item, wallet []WalletEntry, replaceWallet bool, jobs []ProcessingJob, replaceJobs bool,
	known []int64, replaceKnown bool, warehouse []WarehouseItem, replaceWarehouse bool, containers []WarehouseContainer, market []MarketplaceRow) error {
	if err := ValidateMarketplaceRows(market); err != nil {
		return err
	}
	return s.saveInventoryEverything(ctx, characterID, items, wallet, replaceWallet, jobs, replaceJobs, known, replaceKnown, warehouse, replaceWarehouse, containers, market, nil)
}
