// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package content

// the effects of the gear sets (contentc version 43): set_effect_bonus, in schema.go beside set_bonus
func init() {
	extraDocumentedTables = append(extraDocumentedTables, "set_effect_bonus")
}
