// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"errors"
	"fmt"
	"strings"

	"github.com/jackc/pgx/v5"
)

// The shared warehouse (migration 0026): one warehouse for all the characters of an account, in account_warehouse_items and account_warehouse_containers, with the revision worldd last
// wrote in account_warehouses. worldd keeps it while any character of the account plays and counts each change; a save writes it only when its revision is above the stored one. The
// characters' own warehouses from before (character_warehouse_items) are its legacy items: worldd puts each where it fits and names it in a save, and the row then leaves the old table.

// SharedWarehouse is an account's warehouse as GetCharacter sends it.
type SharedWarehouse struct {
	Revision   int64                // the revision last written; 0 when none was
	Items      []WarehouseItem      // by area, town, container and slot
	Containers []WarehouseContainer // what the account bought, and what any of its characters bought before
	Legacy     []WarehouseItem      // the items still in the characters' own warehouses, by character (oldest first), then as Items
}

// SharedWarehouseSave is what a save of the shared warehouse carries besides its items: their revision and the legacy instances now in them.
type SharedWarehouseSave struct {
	Revision     int64
	MergedLegacy []string // instance guids
}

// Validate checks the revision and the guids.
func (w SharedWarehouseSave) Validate() error {
	switch {
	case w.Revision < 0:
		return fmt.Errorf("%w: warehouse revision %d is negative", ErrInvalid, w.Revision)
	case len(w.MergedLegacy) > MaxWarehouseItems:
		return fmt.Errorf("%w: %d merged legacy warehouse items (at most %d)", ErrInvalid, len(w.MergedLegacy), MaxWarehouseItems)
	}
	for i, g := range w.MergedLegacy {
		if !guidPattern.MatchString(g) {
			return fmt.Errorf("%w: merged legacy warehouse item %d: %q is not an instance guid", ErrInvalid, i, g)
		}
	}
	return nil
}

func scanWarehouseItems(rows pgx.Rows) ([]WarehouseItem, error) {
	defer rows.Close()
	var out []WarehouseItem
	for rows.Next() {
		var w WarehouseItem
		if err := rows.Scan(&w.Area, &w.NodeGUID, &w.ContainerGUID, &w.Slot, &w.ItemRecordID, &w.InstanceGUID, &w.Quantity, &w.Quality, &w.Durability, &w.CreatedAt,
			&w.PersistedHealth, &w.PersistedMana, &w.LockExpiration); err != nil {
			return nil, err
		}
		out = append(out, w)
	}
	return out, rows.Err()
}

// prefixed puts `p` before each column of a comma-separated list.
func prefixed(p, columns string) string {
	parts := strings.Split(columns, ", ")
	for i := range parts {
		parts[i] = p + parts[i]
	}
	return strings.Join(parts, ", ")
}

// SharedWarehouseOf returns the warehouse of the character's account. An unknown character is ErrNotFound.
func (s *Store) SharedWarehouseOf(ctx context.Context, characterID string) (SharedWarehouse, error) {
	var out SharedWarehouse
	if !idPattern.MatchString(characterID) {
		return out, ErrNotFound
	}
	var account int64
	if err := s.pool.QueryRow(ctx, `SELECT account_id FROM characters WHERE id = $1`, characterID).Scan(&account); err != nil {
		if errors.Is(err, pgx.ErrNoRows) {
			return out, ErrNotFound
		}
		return out, err
	}
	if err := s.pool.QueryRow(ctx, `SELECT COALESCE((SELECT revision FROM account_warehouses WHERE account_id = $1), 0)`, account).Scan(&out.Revision); err != nil {
		return out, err
	}
	rows, err := s.pool.Query(ctx, `SELECT `+warehouseColumns+` FROM account_warehouse_items WHERE account_id = $1
		ORDER BY CASE area WHEN 'Items' THEN 0 WHEN 'Materials' THEN 1 ELSE 2 END, node_guid, container_guid, slot`, account)
	if err != nil {
		return out, err
	}
	if out.Items, err = scanWarehouseItems(rows); err != nil {
		return out, err
	}
	rows, err = s.pool.Query(ctx, `SELECT `+prefixed("w.", warehouseColumns)+` FROM character_warehouse_items w JOIN characters c ON c.id = w.character_id WHERE c.account_id = $1
		ORDER BY c.created_at, c.id, CASE w.area WHEN 'Items' THEN 0 WHEN 'Materials' THEN 1 ELSE 2 END, w.node_guid, w.container_guid, w.slot`, account)
	if err != nil {
		return out, err
	}
	if out.Legacy, err = scanWarehouseItems(rows); err != nil {
		return out, err
	}
	// (the characters' own purchases too: the migration copied them, and an older worldd run after it would have saved new ones there)
	crows, err := s.pool.Query(ctx, `SELECT area, node_guid, container_guid FROM account_warehouse_containers WHERE account_id = $1
		UNION SELECT w.area, w.node_guid, w.container_guid FROM character_warehouse_containers w JOIN characters c ON c.id = w.character_id WHERE c.account_id = $1
		ORDER BY 1, 2, 3`, account)
	if err != nil {
		return out, err
	}
	defer crows.Close()
	for crows.Next() {
		var c WarehouseContainer
		if err := crows.Scan(&c.Area, &c.NodeGUID, &c.ContainerGUID); err != nil {
			return out, err
		}
		out.Containers = append(out.Containers, c)
	}
	return out, crows.Err()
}

// saveSharedWarehouse writes the account's warehouse in tx when save.Revision is above the stored one (and then takes the merged legacy rows out of the characters' warehouses), and
// adds the containers to what the account owns either way.
func saveSharedWarehouse(ctx context.Context, tx pgx.Tx, characterID string, items []WarehouseItem, containers []WarehouseContainer, save SharedWarehouseSave) error {
	var account int64
	if err := tx.QueryRow(ctx, `SELECT account_id FROM characters WHERE id = $1`, characterID).Scan(&account); err != nil {
		return err
	}
	for _, c := range containers {
		if _, err := tx.Exec(ctx, `INSERT INTO account_warehouse_containers (account_id, area, node_guid, container_guid) VALUES ($1, $2, $3, $4) ON CONFLICT DO NOTHING`,
			account, c.Area, c.NodeGUID, c.ContainerGUID); err != nil {
			return err
		}
	}
	// the account's row is the lock: two characters' saves take their turns here, and the later one sees the revision the earlier one wrote
	if _, err := tx.Exec(ctx, `INSERT INTO account_warehouses (account_id) VALUES ($1) ON CONFLICT DO NOTHING`, account); err != nil {
		return err
	}
	var stored int64
	if err := tx.QueryRow(ctx, `SELECT revision FROM account_warehouses WHERE account_id = $1 FOR UPDATE`, account).Scan(&stored); err != nil {
		return err
	}
	if save.Revision <= stored {
		return nil // a newer warehouse is stored already (it holds every change this one does)
	}
	if _, err := tx.Exec(ctx, `UPDATE account_warehouses SET revision = $2, updated_at = now() WHERE account_id = $1`, account, save.Revision); err != nil {
		return err
	}
	if _, err := tx.Exec(ctx, `DELETE FROM account_warehouse_items WHERE account_id = $1`, account); err != nil {
		return err
	}
	if len(save.MergedLegacy) > 0 {
		if _, err := tx.Exec(ctx, `DELETE FROM character_warehouse_items WHERE character_id IN (SELECT id FROM characters WHERE account_id = $1) AND instance_guid = ANY($2)`,
			account, save.MergedLegacy); err != nil {
			return err
		}
	}
	if len(items) == 0 {
		return nil
	}
	var batch pgx.Batch
	for _, w := range items {
		batch.Queue(`INSERT INTO account_warehouse_items (account_id, `+warehouseColumns+`) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, $14)`,
			account, w.Area, w.NodeGUID, w.ContainerGUID, w.Slot, w.ItemRecordID, w.InstanceGUID, w.Quantity, w.Quality, w.Durability, w.CreatedAt,
			w.PersistedHealth, w.PersistedMana, w.LockExpiration)
	}
	res := tx.SendBatch(ctx, &batch)
	for range items {
		if _, err := res.Exec(); err != nil {
			res.Close()
			return itemsError(err)
		}
	}
	return res.Close()
}

// SaveInventoryShared is SaveInventoryWithMarketplace with the account's warehouse in place of the character's: warehouse is the account's whole warehouse at save.Revision, written only
// when that is above the stored revision; containers are added to what the account owns. The warehouse is checked against the bag's items (no instance on both) before anything is written.
func (s *Store) SaveInventoryShared(ctx context.Context, characterID string, items []Item, wallet []WalletEntry, replaceWallet bool, jobs []ProcessingJob, replaceJobs bool, known []int64,
	replaceKnown bool, warehouse []WarehouseItem, containers []WarehouseContainer, save SharedWarehouseSave, market []MarketplaceRow) error {
	if err := ValidateMarketplaceRows(market); err != nil {
		return err
	}
	return s.saveInventoryEverything(ctx, characterID, items, wallet, replaceWallet, jobs, replaceJobs, known, replaceKnown, warehouse, true, containers, market, &save)
}
