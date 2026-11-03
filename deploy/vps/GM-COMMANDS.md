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
| Inventory | money (copper), item (record name/ID, count, optional rarity `Epic` or exact quality `q65`/`65`, 1-200) |
| Character | progress (one fact a line: Adventuring, the 22 professions, weapon lines, purse), heal |
| Crafting | finish-jobs, teach-recipe, forget-recipe, recipes, place-station, remove-station |
| Progression | xp, level (raises only), clear-debt, profession-xp, profession-level (1-50), profession-tier (0-4 or Novice..GrandMaster), reset-tutorials, weapon-level, weapon-xp |
| Nodes | node-level (0–4), node-xp, node-info |

`*` as the player means everyone online, for money, item, xp, level, clear-debt,
heal, profession-xp, profession-level, profession-tier, weapon-level, weapon-xp and
teach-recipe; the answer has a line per player. `*` as the profession means all 22.
A level set by `level` keeps the experience debt; a profession level past its
certification's cap raises the certification to the lowest that holds it.
| World | clock, advance-clock |
| Quests | quest-list, quest-give, quest-finish, quest-fail, quest-abandon, quest-clear, quest-variable |
| NPCs | npc-kill, npc-heal, npc-reset, npc-respawn, npc-despawn, npc-move |

## GM web tools

`auth` serves a web version of the panel at `https://<server>:8444/gm/`, compiled
into `auth.exe`. Sign in with the GM account; any other account is turned away.
The page has no secret of its own and every power stays behind the API's checks.

| Needs | Works with |
| --- | --- |
| The account's grant only | Overview, Map, Players, Find, Places and the pickers' searches (`GET /api/v1/gm/{status,players,places,search,inspect,catalog,map,nodes}`, read-only) |
| A character in the world and GM mode | Every button that changes something: Give items, Give money, Progression and the Commands view (`POST /api/v1/gm`, the same checks as chat); Progression also reads through `progress` |

The Map follows worldd's world stream (`AOC_WORLD_STREAM_ADDR`): `auth` opens one
stream for the whole of Verra at the first map read and closes it a minute after the
last, so a closed map costs worldd nothing (the stream allows 8 at once). Players are
always sent; creatures, resource nodes, mounts and loot only for a view under 8 km
across. The nodes are the places list's positions with the levels of the node service
(`AOC_CITYNODE_GRPC_ADDR`); without it they show with no level. Behind them is the
game's own world map, which `mapc minimap` (the `minimap` step of
`extract_game_data`) exports from the client's tiles into `dist/content/minimap`:
110 tiles of 2.5 km and an overview, 58 MB, shipped in the package with the rest of
`dist/content`. `auth` reads them from `minimap` beside `AOC_CONTENT_DB` and serves
them only to the GM login (`GET /api/v1/gm/minimap/...`); without them the map is a
plain grid. World +X is up until Rotate is pressed.

Every command has a ready form (`web/commands.js`; a test compares its arguments
with `gm_commands.inc`, so add a new command there too). Pickers search the game's
own records from `content.db`, which `auth` reads once at the first search. The data
has record names only, not the in-game names, so search by the words in those. Players
can be picked only while online, because every player command needs them in the world.
Quests are named by name: `QuestBook::find` cannot read their 19-digit Guids, so
the 27 names that several quests share cannot be told apart.

GM mode can be turned on from the page once the character is in the world, and
relogging still turns it off. Commands that take something away or change the
world for everybody ask first. A sign-in lasts as long as the login token,
24 hours by default, and the page forgets it when the tab closes; Sign out clears the
page but does not end the token on the server.

The server's certificate comes from this deployment's own CA, so a browser warns
about it. Trust `ca.crt` on that device rather than clicking through: the warning
is the only thing that would show someone intercepting the GM password. On
Windows: `certutil -user -addstore Root ca.crt`.

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
