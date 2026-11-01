# Test failures handoff

Snapshot: 2026-10-31 21:05 EDT, branch `vps-deploy`, HEAD `a183c9b`, on top of a large uncommitted working tree. Line numbers are from that tree and may drift.

**Goal:** get the test gate back to the baseline so the uncommitted work, including Summoner pets, can be deployed.

**Delete this file when the work is done.**

## Rules

- Do not deploy until the gate is resolved. The live `worldd.exe` (built 16:31) has none of this work.
- Do not deploy `build/gm-deploy` or any package built before 20:46. It may contain the startup crash described under "Already fixed".
- Live server changes follow `AGENTS.md`, including the Discord notices.
- Do not make a check pass by weakening it, or by adding an entry to `kKnown` or the baseline, until the cause is understood. A failure may be a real server regression.
- Other agents may be working in this tree. Check `git status` and running processes before a build. `scripts/run_tests.ps1` deletes and rebuilds `dist\Release\test_*.exe`.
- Limit build jobs with `-Jobs 6`. The script header warns that this PC can hard-reset under load.

## Commands

```powershell
.\scripts\build.ps1 -Target <test_name> -Config Release -Jobs 6      # one test
& dist\Release\<test_name>.exe                                       # run from the repo root
.\scripts\run_tests.ps1 -Jobs 6 -CompareTo fixtures\test-baseline.txt   # full gate
```

`test_worldd_ability_all.exe -v` lists each mismatch. `AOC_TEST_LOG_FILTER=<text>` prints matching log lines at exit in `test_worldd_ability_session` only.

## Status

The last full gate ran 109 tests with 6 failing. One of those, `test_worldd_area`, has since been fixed, leaving 5.

| # | Test | Failed checks | Cause | Priority |
|---|---|---|---|---|
| 1 | `test_worldd_items_session` | 5 of 6 | Strong lead: new consumable gate refuses triggers that name an equipped item | **High: may be a live regression** |
| 2 | `test_worldd_items_session` | 1 of 6 | Unknown: stack split | Medium |
| 3 | `test_worldd_session` | 4 summary checks | Strong lead: test sends part of a struct parameter | Medium |
| 4 | `test_worldd_area_session` | 5 | First failure confirmed: test character is not a GM | Low: test setup |
| 5 | `test_worldd_gather_session` | 1 | Unknown: an extra PlayerController call | Medium |
| 6 | `test_worldd_ability_all` | 1 | Unknown: General_Guard effect missing | Medium |

`test_worldd_area_session` is already `FAIL` in `fixtures/test-baseline.txt`, so it does not differ from the baseline. It is listed because it still exits 1, and any failing test makes `run_tests.ps1` exit 1. The baseline does not record why it failed then.

None of the failing checks are about pets, but the other uncommitted ability and session changes, including the pet code, have not been ruled out except where stated below.

## 1. Item triggers refused (`test_worldd_items_session`)

**Check this one first.**

Failed checks:

- `test_worldd_items_session.cpp:2471` the runtime holds 150 and 150 and the Rest is on
- `test_worldd_items_session.cpp:2481` (a Dodge took 50 Stamina)
- `test_worldd_items_session.cpp:2485` the ring back on: MaxStamina is 150 again and Stamina is not raised by it
- `test_worldd_items_session.cpp:2628` (the first Dodge took 50)
- `test_worldd_items_session.cpp:2632` (the second Dodge landed too: Stamina is about 50)

Evidence:

- These tests send Rest and Dodge through `trigger_with_item(...)`, naming the worn Scepter: slot 15, location 4 (not the Backpack).
- `player_session.cpp:1747` sets `info.item = t->item_slot != -1`.
- `player_session.cpp:1781` now calls `prepare_consumable` for any trigger with `info.item` set.
- `player_consumables.inc`, in `prepare_consumable`, refuses first with "the item is not in the Backpack" unless the location is Backpack.

Lead, not yet run: the gate treats every item-naming trigger as a consumable, so a trigger naming an equipped weapon is refused.

Why it matters: the tests model the client naming its worn weapon on a trigger. If the real client does this for ordinary abilities, then deploying this code would refuse those abilities for players, possibly including Summoner pet casts, which `test_summoner_pet_session` sends without an item.

To do:

1. Confirm with the `[consumable]` log line that this is the refusal reason.
2. Check the retail captures for what `ItemData` the client sends on a normal ability while a weapon is equipped.
3. If it does name the weapon, limit the consumable path to Backpack items and let other triggers continue as they did before. Keep refusing forged Backpack claims, and keep the tests in `consumable_session_tests.inc` passing.

## 2. Stack split (`test_worldd_items_session`)

- Failed check: `test_worldd_items_session.cpp:4468` a Move of part of a stack splits it, and the client agrees.
- The Fill check straight after it passes.
- The test sets up a Move of 5 from slot 2 to slot 10 after seven deliberate refusals and a 2.1 s wait for budgets to refill.
- Server path: `player_session.cpp:3409-3420` tries `inventory_.fill`, then `inventory_.split`, then `inventory_.move`. `Inventory::split` is in `items/inventory.cpp`.
- Not investigated. First find out which part of the check is false: `parted`, its quantity, its guid, the source quantity or `mirror_matches`. Then read the refusal reason from the log.
- `KNOWN_ISSUES.md` says this feature is built and tested on the server, so correct either the code or that row.

## 3. Auto-attack settings (`test_worldd_session`)

Failed checks, in `combat_settings_session_tests.inc`:

- `:40` all five auto attack choices are handed to durable storage immediately
- `:70` a reliable property block restores all five saved values, including false
- `:72` the player's later change can disable and save every option
- one more summary check: "explicitly disabled options also survive a new login"
- repeated `[FAIL] ServerUpdateAccountCombatSettings does not write`, printed by the test's `client_rpc` helper. The test's final count is 4 failed, because these lines are printed directly and not counted.

Lead: the test's own message fails to encode, so the server may never receive it.

- `send_options` sets only the five `kAutoAttackPaths` values (`player_session.h:375`).
- `rpc_params_match` in `libs/aocnet/protocol/repl/rpc.cpp:165` rejects a struct parameter that has some but not all of its fields set.

The first thing to verify is that `write_rpc_field` fails for this reason. The likely fix is for the test to fill every field of the parameter. After that, check that `on_combat_settings` (`player_session.cpp:3428`) really saves and restores the values, because that path has not run in this test yet. Also check that the budget and `room_for(1)` early return at that line cannot silently lose a player's setting.

## 4. GM quest commands (`test_worldd_area_session`)

Failed checks, in `quest_session_tests.inc`:

- `:886` ServerDebugStartQuest takes Corrupted Memories (GM commands on)
- `:894` ... and with the rules bypassed it takes it
- `:909` K5: four quests in the log
- `:912` K7: every item of each array has a replication id of its own
- `:937` the saved log holds the three quests that are left

Evidence: the test prints `after the debug start: 0 quest(s) in the log, 0 quest block(s) sent, GM RPCs seen 1, answered 0`.

Cause of the first failure: `gm_rpc_admitted` (`player_session.cpp:7383`) now requires `gm_level_ == 3 && gm_enabled_`, while the test only calls `fx.world->set_gm_commands(gm)` (`quest_session_tests.inc:151`). The other four look like knock-on failures, since they need those quests in the log. Confirm that after the fix.

Likely fix: give this test's character GM level 3 and enable GM, without loosening the server check.

## 5. Extra PlayerController call (`test_worldd_gather_session`)

- Failed check: `test_worldd_gather_session.cpp:850` and none of this sent the PlayerController a call.
- The count is taken at `:832`, then a series of refused gathering attempts run. The refusal counts themselves are correct.
- `player_gather.cpp` has only an XP-rate change in its diff, so the new call probably comes from elsewhere in the session.
- Not investigated. Print the RPC names from `pc_calls_since(novice, 0)` beyond `pc_before`. If the call is unrelated to gathering, such as a periodic message from the new chat, presence or settings work, decide whether the check should filter it. If it is a refusal side effect, fix the server.

## 6. General_Guard against retail (`test_worldd_ability_all`)

- Failed check: `test_worldd_ability_all.cpp:763` every accepted cast matches retail's reply (510 of 511).
- `-v` shows `General_Guard@821:69 EFF:missing General_Guard`. The Guard casts in captures 822 and 823 match.
- All 3 Summoner casts match, so this is not the summon timing change.
- `session/abilities.h`, `session/ability_catalog.cpp` and `session/ability_pipeline.cpp` all have uncommitted changes. Start with `git diff` on those files and what happens to the Guard effect at this capture's line.
- `R_A7` entries in `kKnown` (`test_worldd_ability_all.cpp:206`) cover other Guard differences. Do not add this one unless retail really differs for a stated reason.

## Baseline

Once the failures are resolved, compare with the gate again. It also reports `test_tether_player PASS exit=0` as not listed in `fixtures/test-baseline.txt`, which needs adding. Only update the baseline after each difference is understood.

## Already fixed

Do not redo these.

- **Startup crash:** the uncommitted work stopped loading GM vendors, but `PlayerWorld::load_area` still logged through the empty `gm_`. The logging is now guarded by `if (gm_)` (`player_world.cpp:422`).
- **Summoner session tests:** they recast without enough mana (853 per summon at level 25). `summoner_pet_session_tests.inc` now refills mana before each cast and checks that an unaffordable summon keeps the current pet. `test_worldd_ability_session` passes 480 of 480.
- **`test_worldd_area`:** it now expects no GM vendors in the public area, and passes 131 of 131.

## Still open after the gate is clean

- Deploy, following `AGENTS.md`, then test with the Summoner character SuSu in the game client.
- Follow and Stay have no automated test. Dismiss is only tested on the server side.
- Confirm the pet data (`Pet_Summoner_*` in `npc_motion.tsv` and the captured templates) is present in the VPS content files. Without it, the pet fails to spawn and logs a `[pets]` warning.
- Pet combat and special abilities are not built, and `KNOWN_ISSUES.md` says so.
