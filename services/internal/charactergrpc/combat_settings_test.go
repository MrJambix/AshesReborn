// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors.
package charactergrpc

import (
	"context"
	"reflect"
	"testing"

	characterv1 "aocrop/services/gen/aoc/character/v1"
)

type combatSettingsStore struct {
	*recStore
	options   map[string]bool
	accountID int64
}

func (s *combatSettingsStore) AutoAttackSettings(_ context.Context, accountID int64) (map[string]bool, error) {
	s.accountID = accountID
	return s.options, nil
}

func TestCombatSettingsSaveAndLoginTransport(t *testing.T) {
	st := &combatSettingsStore{recStore: &recStore{ret: fullCharacter()}, options: map[string]bool{"bEnabled": true, "bToggle": false}}
	client := dialRec(t, st)
	_, err := client.SaveProgress(context.Background(), &characterv1.SaveProgressRequest{Id: st.ret.ID, AutoAttackSettings: st.options})
	if err != nil || !reflect.DeepEqual(st.gotProgress.AutoAttackSettings, st.options) {
		t.Fatalf("save options = %v, %v", st.gotProgress.AutoAttackSettings, err)
	}
	got, err := client.GetCharacter(context.Background(), &characterv1.GetCharacterRequest{Id: st.ret.ID, IncludeItems: true})
	if err != nil || !reflect.DeepEqual(got.GetCharacter().GetAutoAttackSettings(), st.options) || st.accountID != st.ret.AccountID {
		t.Fatalf("login options = %v, %v; account %d", got, err, st.accountID)
	}
}
