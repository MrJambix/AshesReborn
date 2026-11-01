# GM access and starting characters

Public starting areas no longer receive the runtime GM vendor NPCs. New
characters receive one `Mount_Grounded_Flailrunner_0` in a free backpack slot,
in the same transaction as their class kit. Missing mount content refuses
character creation. Existing characters are not retroactively granted mounts.

Migration 0020 pins the sole level 3 owner grant to the existing `mrjambix`
account. This deployment's account ID is 3. Names of characters and names sent
by the client have no effect on permissions. Registering a new matching username
after migration cannot acquire the grant. The table allows at most one owner.
Deleting the owner account removes its grant. On a fresh database with no
matching account, an operator must explicitly insert a verified account ID into
`gm_accounts`; no registration endpoint grants GM access.

Enter the world and type `.GM On` in chat. `.GM Off` disables commands and resets
your movement speed to normal. Relogging always starts with GM mode off.
`.GM help` lists the implemented commands. Keywords ignore case; record names
retain their case. Use `self` for your character, and quote arguments containing
spaces, for example `.GM teleport self "Joeva"`.

The updated Reborn Launcher draws a Windows panel over the game after `.GM On`.
Keep that launcher running and use Windowed or Borderless mode. This overlay
uses ordinary Windows controls; it does not modify the shipping client. Closing
the panel hides it; `.GM ui` opens it again. Categories and fields are loaded
from worldd's command catalog. Chat commands also work with older launchers.

| Category | Commands |
| --- | --- |
| Session | on, off, ui, help |
| Travel | players, places, teleport, teleport-xyz, speed |
| Inventory | money (copper), item (record name/ID, count, optional rarity) |
| Crafting | finish-jobs, teach-recipe, forget-recipe, recipes, place-station, remove-station |
| Progression | reset-tutorials, weapon-level, weapon-xp |
| Nodes | node-level (0–4), node-xp, node-info |
| World | clock, advance-clock |
| Quests | quest-list, quest-give, quest-finish, quest-fail, quest-abandon, quest-clear, quest-variable |
| NPCs | npc-kill, npc-heal, npc-reset, npc-respawn, npc-despawn, npc-move |

The HTTPS `/api/v1/gm` endpoint validates the player's login token and the
database account grant on every request. It accepts only a command, derives the
account ID itself and forwards it over the existing service-token-protected
loopback gRPC connection. worldd independently checks the joined account ID,
its grant and GM mode. Service credentials are never sent to the launcher.
Changing a database grant takes effect for in-game chat/RPCs on the next login;
HTTPS checks see it immediately. Node changes are queued to the node service;
diagnostic node/time reads are written to its server log.

For a binary rollback, restore the previous auth, character and worldd programs
and restart the stack. Migration 0020 is additive and can remain installed.
To remove its grant separately, `DELETE FROM gm_accounts;` and relog the owner.
