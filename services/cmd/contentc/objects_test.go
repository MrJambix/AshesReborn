// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.

package main

import (
	"reflect"
	"testing"

	"aocrop/services/internal/designdb"
)

func TestRewardTableItemsFindsEveryItemOnce(t *testing.T) {
	item := func(g int64) designdb.Value {
		return designdb.Struct{"Item": designdb.Struct{"ItemId": designdb.Struct{"Guid": g}}}
	}
	table := designdb.Struct{
		"Name": "a table",
		"Categories": []designdb.Value{
			designdb.Struct{"ItemRewards": []designdb.Value{item(30), item(10)}},
			designdb.Struct{"ItemRewards": []designdb.Value{item(10), item(0)}},
		},
		"ByKey": designdb.Pairs{{Value: item(20)}},
	}
	if got, want := rewardTableItems(table), []int64{10, 20, 30}; !reflect.DeepEqual(got, want) {
		t.Errorf("rewardTableItems = %v, want %v", got, want)
	}
	if got := rewardTableItems(designdb.Struct{"Name": "empty"}); len(got) != 0 {
		t.Errorf("a table with no items gives %v", got)
	}
}
