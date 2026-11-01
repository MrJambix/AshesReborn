// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package kit

import (
	"fmt"
	"math/rand/v2"
	"strings"

	"aocrop/services/internal/character"
	"aocrop/services/internal/content"
)

// defaultStartTag is the starting zone of worldd's default spawn (bootstrap/character_setup.h kDefaultSpawn, one spot among
// these starts): a character from there is stored with no position, as every character was before the zones were read.
const defaultStartTag = "Riverlands"

// Starts holds where a new character of each starting zone appears, by the tag the client's character creator sends.
type Starts struct {
	byTag map[string][]content.PlayerStart
}

// LoadStarts reads the start places from content.db.
func LoadStarts(db *content.DB) (*Starts, error) {
	byTag, err := db.PlayerStarts()
	if err != nil {
		return nil, fmt.Errorf("kit: %w", err)
	}
	if len(byTag) == 0 {
		return nil, fmt.Errorf("kit: content.db has no player starts")
	}
	return &Starts{byTag: byTag}, nil
}

// Start returns the first position of a character the creator sent with tag: one of the zone's places at random, so that
// new characters do not stand on each other. A nil position with true is the default spawn: an empty tag (a payload
// without one) or the default zone. False is a tag no zone has.
func (s *Starts) Start(tag string) (*character.Position, bool) {
	if tag == "" || strings.EqualFold(tag, defaultStartTag) {
		return nil, true
	}
	for name, places := range s.byTag {
		if strings.EqualFold(tag, name) && len(places) > 0 {
			p := places[rand.IntN(len(places))]
			return &character.Position{X: p.X, Y: p.Y, Z: p.Z, Yaw: float32(p.Yaw)}, true
		}
	}
	return nil, false
}
