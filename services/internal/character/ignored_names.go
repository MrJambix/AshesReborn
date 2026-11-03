// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"fmt"
	"strings"
	"unicode"
	"unicode/utf8"

	"github.com/jackc/pgx/v5"
)

// The chat ignore list (migration 0025): the names a character ignores, one row each in character_ignored_names, with their place in the character's list. worldd keeps the list
// while the character plays and saves it with the progress.

// MaxIgnoredNames bounds one save: worldd's chat room holds at most 200 names for a character (session/chat.h kMaxIgnored).
const MaxIgnoredNames = 200

// ValidateIgnoredNames checks the ignore list of a save: each name 1 to 64 characters of valid UTF-8 with no control character, each once (without regard to case), and not more
// than a character can have.
func ValidateIgnoredNames(names []string) error {
	if len(names) > MaxIgnoredNames {
		return fmt.Errorf("%w: %d ignored names (at most %d)", ErrInvalid, len(names), MaxIgnoredNames)
	}
	seen := make(map[string]bool, len(names))
	for i, n := range names {
		if !utf8.ValidString(n) || n == "" || utf8.RuneCountInString(n) > 64 || strings.IndexFunc(n, unicode.IsControl) >= 0 {
			return fmt.Errorf("%w: ignored name %d is not a name", ErrInvalid, i)
		}
		key := strings.ToLower(n)
		if seen[key] {
			return fmt.Errorf("%w: ignored name %d: %q again", ErrInvalid, i, n)
		}
		seen[key] = true
	}
	return nil
}

// IgnoredNames returns the names a character ignores, in the order they were added. A character that ignores no one has an empty list; an unknown character is ErrNotFound.
func (s *Store) IgnoredNames(ctx context.Context, characterID string) ([]string, error) {
	if !idPattern.MatchString(characterID) {
		return nil, ErrNotFound
	}
	rows, err := s.pool.Query(ctx, `SELECT name FROM character_ignored_names WHERE character_id = $1 ORDER BY position`, characterID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []string
	for rows.Next() {
		var n string
		if err := rows.Scan(&n); err != nil {
			return nil, err
		}
		out = append(out, n)
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	if len(out) == 0 {
		var exists bool
		if err := s.pool.QueryRow(ctx, `SELECT EXISTS (SELECT 1 FROM characters WHERE id = $1)`, characterID).Scan(&exists); err != nil {
			return nil, err
		}
		if !exists {
			return nil, ErrNotFound
		}
	}
	return out, nil
}

// replaceIgnoredNames makes the character's ignore list exactly names, in this order, in tx.
func replaceIgnoredNames(ctx context.Context, tx pgx.Tx, characterID string, names []string) error {
	if _, err := tx.Exec(ctx, `DELETE FROM character_ignored_names WHERE character_id = $1`, characterID); err != nil {
		return err
	}
	if len(names) == 0 {
		return nil
	}
	_, err := tx.Exec(ctx, `INSERT INTO character_ignored_names (character_id, name, position)
		SELECT $1, t.name, t.n - 1 FROM unnest($2::text[]) WITH ORDINALITY AS t(name, n)`, characterID, names)
	return itemsError(err)
}
