package db

import (
	"aocrop/services/internal/dbtest"
	"context"
	"testing"
)

func TestGMOwnerIsPinnedToExistingAccount(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := migrate(ctx, pool, upTo(t, 20)); err != nil {
		t.Fatal(err)
	}
	var owner int64
	if err := pool.QueryRow(ctx, `INSERT INTO accounts(username,password_hash) VALUES ('MrJaMbIx','unused') RETURNING id`).Scan(&owner); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO accounts(username,password_hash) VALUES ('Other','unused')`); err != nil {
		t.Fatal(err)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	var id int64
	var level int
	if err := pool.QueryRow(ctx, `SELECT account_id,level FROM gm_accounts`).Scan(&id, &level); err != nil || id != owner || level != 3 {
		t.Fatalf("owner %d level %d: %v", id, level, err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO gm_accounts(account_id) SELECT id FROM accounts WHERE username='Other'`); err == nil {
		t.Fatal("a second GM must be rejected")
	}
	if _, err := pool.Exec(ctx, `UPDATE accounts SET username='Renamed' WHERE id=$1`, owner); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO accounts(username,password_hash) VALUES ('mrjambix','unused')`); err != nil {
		t.Fatal(err)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if err := pool.QueryRow(ctx, `SELECT account_id FROM gm_accounts`).Scan(&id); err != nil || id != owner {
		t.Fatal("renaming or registering must not transfer GM")
	}
}

func TestLateRegistrationDoesNotGrantGM(t *testing.T) {
	ctx := context.Background()
	pool := dbtest.Pool(t)
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	if _, err := pool.Exec(ctx, `INSERT INTO accounts(username,password_hash) VALUES ('mrjambix','unused')`); err != nil {
		t.Fatal(err)
	}
	if err := Migrate(ctx, pool); err != nil {
		t.Fatal(err)
	}
	var count int
	if err := pool.QueryRow(ctx, `SELECT count(*) FROM gm_accounts`).Scan(&count); err != nil || count != 0 {
		t.Fatal("new registration received GM privileges")
	}
}
