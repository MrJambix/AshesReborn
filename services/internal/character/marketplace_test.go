// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package character

import (
	"context"
	"errors"
	"testing"
)

const testListing = "l0123456789abcdef0123456789abcdef"

func TestMarketplaceRowValidate(t *testing.T) {
	for _, id := range []string{testListing, "s6064633652044300331:0123456789abcdef0123456789abcdef", "s-12:0123456789abcdef0123456789abcdef"} {
		if err := (MarketplaceRow{ID: id, Revision: 1, Data: `{}`}).Validate(); err != nil {
			t.Errorf("%s: %v", id, err)
		}
	}
	for name, r := range map[string]MarketplaceRow{
		"short id":          {ID: "l0123", Data: `{}`},
		"upper id":          {ID: "l0123456789ABCDEF0123456789abcdef", Data: `{}`},
		"storefront no ':'": {ID: "s12", Data: `{}`},
		"other kind":        {ID: "x0123456789abcdef0123456789abcdef", Data: `{}`},
		"negative revision": {ID: testListing, Revision: -1, Data: `{}`},
		"not json":          {ID: testListing, Data: `nope`},
		"json no object":    {ID: testListing, Data: `[1]`},
	} {
		if err := r.Validate(); !errors.Is(err, ErrInvalid) {
			t.Errorf("%s: got %v, want ErrInvalid", name, err)
		}
	}
	twice := []MarketplaceRow{{ID: testListing, Revision: 1, Data: `{}`}, {ID: testListing, Revision: 2, Data: `{}`}}
	if err := ValidateMarketplaceRows(twice); !errors.Is(err, ErrInvalid) {
		t.Errorf("an id twice in one save: got %v, want ErrInvalid", err)
	}
}

func TestMarketplaceRowsRideTheInventorySave(t *testing.T) {
	s, acc := setup(t)
	ctx := context.Background()
	c, err := s.Create(ctx, New{AccountID: acc, Name: "seller", ClassID: 2, ClassName: "Bard", ArchetypeID: 17747, Items: smallKit()})
	if err != nil {
		t.Fatal(err)
	}
	items, err := s.Inventory(ctx, c.ID)
	if err != nil {
		t.Fatal(err)
	}
	save := func(rows ...MarketplaceRow) error {
		return s.SaveInventoryWithMarketplace(ctx, c.ID, items, nil, false, nil, false, nil, false, nil, true, nil, rows)
	}
	list := func() map[string]MarketplaceRow {
		t.Helper()
		got, err := s.ListMarketplace(ctx)
		if err != nil {
			t.Fatal(err)
		}
		out := map[string]MarketplaceRow{}
		for _, r := range got {
			out[r.ID] = r
		}
		return out
	}
	if got := list(); len(got) != 0 {
		t.Fatalf("no rows at first: %v", got)
	}
	if err := save(MarketplaceRow{ID: testListing, Revision: 5, Data: `{"price":10}`}); err != nil {
		t.Fatal(err)
	}
	// an older revision that arrives later changes nothing; a newer one replaces
	if err := save(MarketplaceRow{ID: testListing, Revision: 4, Data: `{"price":99}`}); err != nil {
		t.Fatal(err)
	}
	if got := list()[testListing]; got.Revision != 5 || got.Data != `{"price": 10}` {
		t.Fatalf("an older revision is not kept: %+v", got)
	}
	if err := save(MarketplaceRow{ID: testListing, Revision: 6, Data: `{"price":11}`}); err != nil {
		t.Fatal(err)
	}
	if got := list()[testListing]; got.Revision != 6 || got.Data != `{"price": 11}` {
		t.Fatalf("a newer revision replaces: %+v", got)
	}
	// a tombstone hides the row from an older save, and listing at the start deletes it
	if err := save(MarketplaceRow{ID: testListing, Revision: 7, Gone: true, Data: `{}`}); err != nil {
		t.Fatal(err)
	}
	if err := save(MarketplaceRow{ID: testListing, Revision: 6, Data: `{"price":11}`}); err != nil {
		t.Fatal(err)
	}
	if got := list(); len(got) != 0 {
		t.Fatalf("the tombstone is deleted by the listing and the stale save did not bring the row back: %v", got)
	}
	// a refused row changes nothing, not even the items
	if err := save(MarketplaceRow{ID: "bad", Revision: 1, Data: `{}`}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("a malformed row: got %v, want ErrInvalid", err)
	}
	// the rows and the items are one transaction: a save the items rules refuse writes no row
	if err := s.SaveInventoryWithMarketplace(ctx, c.ID, nil, nil, false, nil, false, nil, false, nil, true, nil,
		[]MarketplaceRow{{ID: testListing, Revision: 9, Data: `{}`}}); !errors.Is(err, ErrInvalid) {
		t.Fatalf("an empty inventory: got %v, want ErrInvalid", err)
	}
	if got := list(); len(got) != 0 {
		t.Fatalf("a refused save wrote a row: %v", got)
	}
}
