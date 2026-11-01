// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

import (
	"encoding/json"
	"fmt"
)

// PlayerStart is one of the places a new character of a starting zone may appear at: where (cm) and the yaw (degrees).
type PlayerStart struct {
	X, Y, Z, Yaw float64
}

// PlayerStarts returns the game's start places by their tag (PlayerStartsRecordBase: "Riverlands", "Anvils"), the string
// the client's character creator sends as CreateCharacterInfo.starting_location.
func (d *DB) PlayerStarts() (map[string][]PlayerStart, error) {
	rows, err := d.sql.Query(`SELECT name, data FROM record WHERE schema = 'PlayerStartsRecordBase' ORDER BY guid`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := map[string][]PlayerStart{}
	for rows.Next() {
		var name string
		var data []byte
		if err := rows.Scan(&name, &data); err != nil {
			return nil, err
		}
		js, err := DecodeData(d.encoding, data)
		if err != nil {
			return nil, fmt.Errorf("content: the player starts %q: %w", name, err)
		}
		var r struct {
			PlayerStarts struct {
				StartLocations []struct {
					Location struct{ X, Y, Z float64 }
					Rotation struct{ Yaw float64 }
				}
				Tag struct{ TagName string }
			}
		}
		if err := json.Unmarshal(js, &r); err != nil {
			return nil, fmt.Errorf("content: the player starts %q: %w", name, err)
		}
		tag := r.PlayerStarts.Tag.TagName
		for _, l := range r.PlayerStarts.StartLocations {
			out[tag] = append(out[tag], PlayerStart{X: l.Location.X, Y: l.Location.Y, Z: l.Location.Z, Yaw: l.Rotation.Yaw})
		}
	}
	return out, rows.Err()
}
