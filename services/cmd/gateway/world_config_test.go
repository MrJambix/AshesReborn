// SPDX-License-Identifier: AGPL-3.0-only
package main

import (
	"testing"

	"aocrop/services/internal/config"
	"aocrop/services/internal/ics"
)

func TestWorldDisplayNameKeepsIdentity(t *testing.T) {
	legacy := ics.DefaultConfig()
	for _, name := range []string{"Ashes Reborn", "Ashes Reborn Test", ""} {
		got := clientWorldConfig(config.Config{WorldName: name})
		if got.WorldID != legacy.WorldID || got.ServerTag != legacy.ServerTag || got.MaxCharacters != legacy.MaxCharacters {
			t.Fatalf("renaming the world changed its identity or character settings: %+v", got)
		}
		want := name
		if want == "" {
			want = "Ashes Reborn"
		}
		if got.WorldName != want {
			t.Errorf("WorldName = %q, want %q", got.WorldName, want)
		}
	}
}
