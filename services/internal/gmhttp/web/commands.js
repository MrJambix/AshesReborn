// Every GM command, ready to use. The argument list is worldd's own (gm_commands.inc): a test compares them.
"use strict";

const LEVELS = [{ value: "0", title: "0" }, { value: "1", title: "1" }, { value: "2", title: "2" }, { value: "3", title: "3" }, { value: "4", title: "4" }];
const RARITIES = ["Common", "Uncommon", "Rare", "Heroic", "Epic", "Legendary", "Artifact"];

// How each argument is filled in, when its name alone does not say.
const player = { type: "player", label: "Player" };
const fields = {
  character: player,
  place: { type: "place", label: "Place" },
  x: { type: "number", label: "X (cm)" },
  y: { type: "number", label: "Y (cm)" },
  z: { type: "number", label: "Z (cm)", hint: "Empty: the ground" },
  multiplier: { type: "number", label: "Speed", min: 0.5, max: 10, step: "any", presets: [1, 2, 3, 5, 10], hint: "1 is normal speed" },
  copper: { type: "number", label: "Copper", min: 1, max: 10000000, step: 1 },
  item: { type: "catalog", kind: "item", label: "Item" },
  count: { type: "number", label: "Count", min: 1, max: 100000, step: 1 },
  rarity: { type: "rarity", label: "Rarity" },
  station: { type: "catalog", kind: "station", label: "Station" },
  recipe: { type: "catalog", kind: "recipe", label: "Recipe" },
  node: { type: "catalog", kind: "node", label: "Node" },
  plot: { type: "catalog", kind: "plot", label: "Plot" },
  line: { type: "catalog", kind: "line", label: "Weapon line" },
  quest: { type: "catalog", kind: "quest", label: "Quest" },
  xp: { type: "number", label: "XP", step: 1 },
  seconds: { type: "number", label: "Seconds", min: 1, step: 1, presets: [3600, 21600, 86400] },
  variable: { type: "text", label: "Variable" },
  value: { type: "number", label: "Value", step: 1 },
  actor: { type: "actor", label: "Creature" },
};

const COMMANDS = [];
function cmd(name, category, args, title, help, extra = {}) {
  COMMANDS.push({ name, category, args, title, help, ...extra });
}

cmd("on", "Session", "", "GM mode on", "Turns GM mode on for your character.");
cmd("off", "Session", "", "GM mode off", "Turns GM mode off and puts your speed back to normal.");
cmd("ui", "Session", "", "Open launcher panel", "Turns GM mode on and opens the panel in the launcher.");
cmd("help", "Session", "", "Command list", "Lists the chat commands.");

cmd("players", "Travel", "", "List players", "Lists the players in the world and where they are.");
cmd("places", "Travel", "", "List places", "Lists every place you can teleport to by name.");
cmd("teleport", "Travel", "character place", "Teleport to a place", "Moves a player to a named place.");
cmd("teleport-xyz", "Travel", "character x y z?", "Teleport to a point", "Moves a player to a point on the map.");
cmd("speed", "Travel", "character multiplier", "Run speed", "Sets a player's run speed until they relog.");

cmd("money", "Inventory", "character copper", "Give money", "Adds money to a player's purse.", { view: "give-money" });
cmd("item", "Inventory", "character item count rarity?", "Give items", "Gives a player items.", { view: "give-items" });

cmd("finish-jobs", "Crafting", "character station?", "Finish jobs", "Finishes a player's processing jobs now, at one station or all of them.");
cmd("teach-recipe", "Crafting", "character recipe", "Teach recipe", "Teaches a player a recipe. It is saved with the character.");
cmd("forget-recipe", "Crafting", "character recipe", "Forget recipe", "Takes a taught recipe away.", { confirm: "This takes the recipe away from the character." });
cmd("recipes", "Crafting", "character", "Taught recipes", "Lists the recipes a player has been taught.");
cmd("place-station", "Crafting", "node plot station", "Place station", "Puts a station on a free service plot of a Village or Town. It lasts until removed or until the server restarts.");
cmd("remove-station", "Crafting", "node plot", "Remove station", "Takes a placed station off its plot.", { confirm: "This takes the station off its plot." });

cmd("reset-tutorials", "Progression", "character", "Reset tutorials", "Puts a player's finished tutorials back.", { confirm: "This puts back the character's finished tutorials." });
cmd("weapon-level", "Progression", "character line level", "Set weapon level", "Sets a weapon line's level. A level below the ranks bought in its tree is refused.", { fields: { level: { type: "number", label: "Level", min: 1, max: 25, step: 1 } } });
cmd("weapon-xp", "Progression", "character line xp", "Set weapon XP", "Sets a weapon line's experience total.", { fields: { xp: { type: "number", label: "XP total", min: 0, max: 5718160, step: 1 } } });

cmd("node-level", "Nodes", "node level", "Set node level", "Sets a node's level.", { confirm: "This changes the node's level for everybody on the server.", fields: { level: { type: "select", label: "Level", options: LEVELS } } });
cmd("node-xp", "Nodes", "node xp", "Add node XP", "Adds experience to a node.", { confirm: "This changes the node's experience for everybody on the server." });
cmd("node-info", "Nodes", "node", "Node details", "Writes a node's details to the server log, not to this page.");

cmd("clock", "World", "", "World clock", "Writes the world clock to the server log, not to this page.");
cmd("advance-clock", "World", "seconds", "Advance clock", "Moves the world clock forward.", { confirm: "This moves the world clock for everybody on the server." });

cmd("quest-list", "Quests", "character", "Active quests", "Lists a player's active quests.");
cmd("quest-give", "Quests", "character quest", "Give quest", "Starts a quest for a player.");
cmd("quest-finish", "Quests", "character quest", "Finish quest", "Finishes a quest a player has active.");
cmd("quest-fail", "Quests", "character quest", "Fail quest", "Fails a quest a player has active.", { confirm: "This fails the quest for the character." });
cmd("quest-abandon", "Quests", "character quest", "Abandon quest", "Abandons a quest a player has active.", { confirm: "This abandons the quest for the character." });
cmd("quest-clear", "Quests", "character quest?", "Clear quests", "Clears one quest from a player, or all of them if you leave the quest empty.", { confirm: "This clears the character's quests. With no quest named, it clears all of them." });
cmd("quest-variable", "Quests", "character variable value", "Set quest variable", "Sets a quest variable on a player.");

cmd("npc-kill", "NPCs", "actor", "Kill", "Kills a creature. Its loot is open to everybody and nobody gets experience.", { confirm: "This kills the creature." });
cmd("npc-heal", "NPCs", "actor", "Heal", "Heals a creature to full.");
cmd("npc-reset", "NPCs", "actor", "Reset", "Sends a creature home at full health with its threat cleared.");
cmd("npc-respawn", "NPCs", "actor", "Respawn", "Makes a dead creature again now, or replaces a living one.", { confirm: "This replaces the creature at its spawn point." });
cmd("npc-despawn", "NPCs", "actor", "Despawn", "Takes a creature out of the world until its respawn timer.", { confirm: "This takes the creature out of the world until its respawn timer." });
cmd("npc-move", "NPCs", "actor x y z?", "Move", "Stands a creature at a point. Its leash is centred there.");

const CONFIRM = Object.fromEntries(COMMANDS.filter((c) => c.confirm).map((c) => [c.name, c.confirm]));
