// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"unicode/utf8"

	"github.com/jackc/pgx/v5/pgconn"
)

// Guild is one row of the guilds table. Data is worldd's own JSON (its ranks and members); the store only
// checks that it is a JSON object.
type Guild struct {
	GUID string
	Name string
	Data string
}

var guildGUIDPattern = regexp.MustCompile(`^[0-9a-f]{32}$`)

// Validate refuses what the table's constraints would, so the caller sees ErrInvalid rather than a database error.
func (g Guild) Validate() error {
	if !guildGUIDPattern.MatchString(g.GUID) {
		return fmt.Errorf("%w: guild guid %q", ErrInvalid, g.GUID)
	}
	if n := utf8.RuneCountInString(g.Name); n < 1 || n > 64 || !utf8.ValidString(g.Name) {
		return fmt.Errorf("%w: guild name of %d characters", ErrInvalid, n)
	}
	var object map[string]json.RawMessage
	if err := json.Unmarshal([]byte(g.Data), &object); err != nil {
		return fmt.Errorf("%w: guild data is not a JSON object", ErrInvalid)
	}
	return nil
}

// ListGuilds returns every guild, in guid order.
func (s *Store) ListGuilds(ctx context.Context) ([]Guild, error) {
	rows, err := s.pool.Query(ctx, `SELECT guid, name, data::text FROM guilds ORDER BY guid`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Guild
	for rows.Next() {
		var g Guild
		if err := rows.Scan(&g.GUID, &g.Name, &g.Data); err != nil {
			return nil, err
		}
		out = append(out, g)
	}
	return out, rows.Err()
}

// SaveGuild writes a guild whole; the latest save wins. ErrInvalid, with nothing changed, for a guild Validate
// refuses or a name another guild holds in any case.
func (s *Store) SaveGuild(ctx context.Context, g Guild) error {
	if err := g.Validate(); err != nil {
		return err
	}
	_, err := s.pool.Exec(ctx, `
		INSERT INTO guilds (guid, name, data) VALUES ($1, $2, $3::jsonb)
		ON CONFLICT (guid) DO UPDATE SET name = EXCLUDED.name, data = EXCLUDED.data, updated_at = now()`,
		g.GUID, g.Name, g.Data)
	var pg *pgconn.PgError
	if errors.As(err, &pg) && pg.Code == "23505" {
		return fmt.Errorf("%w: guild name %q is taken", ErrInvalid, g.Name)
	}
	return err
}

// DeleteGuild removes a guild. A guild that is not there is not an error: worldd may repeat a delete after a
// failed call.
func (s *Store) DeleteGuild(ctx context.Context, guid string) error {
	if !guildGUIDPattern.MatchString(guid) {
		return fmt.Errorf("%w: guild guid %q", ErrInvalid, guid)
	}
	_, err := s.pool.Exec(ctx, `DELETE FROM guilds WHERE guid = $1`, guid)
	return err
}
