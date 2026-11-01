// SPDX-License-Identifier: AGPL-3.0-only
package main

import (
	"aocrop/services/internal/config"
	"aocrop/services/internal/ics"
)

// Rename the advertised world without changing the identity used by characters
// and the client's remembered world selection.
func clientWorldConfig(cfg config.Config) ics.Config {
	world := ics.DefaultConfig()
	if cfg.WorldName != "" {
		world.WorldName = cfg.WorldName
	}
	return world
}
