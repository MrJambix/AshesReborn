-- The sole owner is pinned to an existing account ID, never a character name.
-- If mrjambix does not exist when this runs, no account receives privileges.
SET LOCAL lock_timeout = '10s';
CREATE TABLE gm_accounts (
    account_id BIGINT PRIMARY KEY REFERENCES accounts(id) ON DELETE CASCADE,
    singleton BOOLEAN NOT NULL DEFAULT TRUE UNIQUE CHECK (singleton),
    level INTEGER NOT NULL DEFAULT 3 CHECK (level = 3)
);
INSERT INTO gm_accounts (account_id)
SELECT id FROM accounts WHERE lower(username) = 'mrjambix';
