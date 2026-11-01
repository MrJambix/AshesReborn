package character

import "context"

// GMLevel is read from the operator-owned account grant, never client input.
func (s *Store) GMLevel(ctx context.Context, accountID int64) (int32, error) {
	var level int32
	err := s.pool.QueryRow(ctx, `SELECT COALESCE((SELECT level FROM gm_accounts WHERE account_id = $1), 0)`, accountID).Scan(&level)
	return level, err
}
