// The GM web tools: a client of /api/v1/auth/login, /api/v1/gm and /api/v1/gm/*.
// Everything from the server goes on the page as text, never as markup.
"use strict";

const $ = (id) => document.getElementById(id);
const TOKEN_KEY = "gm-token";

const state = {
  token: sessionStorage.getItem(TOKEN_KEY) || "",
  view: "overview",
  gm: null,        // the account's GM session; null: no character in the world
  gmNote: "",      // why there is none
  online: null,    // the world server answered the last status read
  players: [],
  places: [],
  catalog: [],
  selected: null,  // the command spec in the form
  lastResult: "",  // what the server said to the last command
  inspected: null, // the actor id in the inspect panel
  timer: 0,
};

const KINDS = {
  ENTITY_KIND_PLAYER: "Player", ENTITY_KIND_NPC: "Creature", ENTITY_KIND_RESOURCE_NODE: "Resource",
  ENTITY_KIND_MOUNT: "Mount", ENTITY_KIND_LOOT_PILE: "Loot",
};

// ---- calling the server

class ApiError extends Error {
  constructor(status, message) { super(message); this.status = status; }
}

async function api(path, options = {}) {
  const headers = { Authorization: "Bearer " + state.token };
  if (options.body) headers["Content-Type"] = "application/json";
  for (let attempt = 0; ; attempt++) {
    let reply;
    try {
      reply = await fetch(path, { ...options, headers, signal: AbortSignal.timeout(8000) });
    } catch {
      throw new ApiError(0, "The server did not answer. Check the world before trying again.");
    }
    // The server allows five GM requests a second; a refused request did nothing, so it can go again.
    if (reply.status === 429 && attempt < 3) { await new Promise((r) => setTimeout(r, 300)); continue; }
    let data = {};
    try { data = await reply.json(); } catch { /* no JSON: the status says enough */ }
    if (reply.status === 401) { signOut(data.message || "Your sign-in expired. Sign in again."); }
    if (!reply.ok) throw new ApiError(reply.status, data.message || "The server refused the request.");
    return data;
  }
}

// ---- small helpers

function el(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [key, value] of Object.entries(props)) {
    if (key === "onclick") node.addEventListener("click", value);
    else if (key === "className") node.className = value;
    else node[key] = value;
  }
  node.append(...children);
  return node;
}
const cm = (v) => Math.round(Number(v) || 0).toLocaleString();
const num = (v, digits = 0) => (Number(v) || 0).toLocaleString(undefined, { maximumFractionDigits: digits });
const quote = (v) => '"' + String(v).replace(/\\/g, "\\\\").replace(/"/g, '\\"') + '"';
const verbOf = (line) => {
  const words = line.trim().split(/\s+/);
  return (words[0].toLowerCase() === ".gm" ? words[1] || "" : words[0]).toLowerCase();
};
const inWorld = () => state.gm !== null;
const gmOn = () => inWorld() && state.gm.enabled;

function logEntry(command, message, ok) {
  const log = $("log");
  const entry = el("div", { className: "log-entry" }, el("time", {}, new Date().toLocaleTimeString()));
  if (command) entry.append(el("span", { className: "muted" }, command + "\n"));
  entry.append(el("span", { className: ok ? "good" : "bad" }, ok ? "" : "Refused: ", ), el("span", {}, message));
  log.prepend(entry);
  while (log.childElementCount > 200) log.lastElementChild.remove();
}

function confirmRun(text, line) {
  const dialog = $("confirm");
  $("confirm-text").textContent = text;
  $("confirm-command").textContent = line;
  dialog.returnValue = "no";
  dialog.showModal();
  return new Promise((resolve) => dialog.addEventListener("close", () => resolve(dialog.returnValue === "yes"), { once: true }));
}

// run sends one command. A request that timed out may still have been applied, so nothing here sends it twice.
async function run(line) {
  const command = line.trim();
  if (!command) return false;
  const warning = CONFIRM[verbOf(command)];
  if (warning && !(await confirmRun(warning, command))) return false;
  try {
    const result = await api("/api/v1/gm", { method: "POST", body: JSON.stringify({ command }) });
    logEntry(command, result.message, true);
    state.lastResult = result.message;
    applyGm(result);
    return true;
  } catch (error) {
    logEntry(command, error.message, false);
    state.lastResult = "Refused: " + error.message;
    return false;
  }
}

// ---- sign in

function showSignIn(message) {
  $("app").hidden = true;
  $("signin").hidden = false;
  $("signin-error").hidden = !message;
  $("signin-error").textContent = message || "";
  $("password").value = "";
  $($("username").value ? "password" : "username").focus();
}

function signOut(message) {
  clearTimeout(state.timer);
  state.timer = 0;
  state.token = "";
  state.gm = null;
  sessionStorage.removeItem(TOKEN_KEY);
  showSignIn(message);
}

async function signIn(event) {
  event.preventDefault();
  const button = event.target.querySelector("button");
  button.disabled = true;
  $("signin-error").hidden = true;
  try {
    const reply = await fetch("/api/v1/auth/login", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username: $("username").value.trim(), password: $("password").value }),
    });
    const data = await reply.json().catch(() => ({}));
    if (!reply.ok || !data.access_token) { showSignIn(data.message || "Signing in failed."); return; }
    state.token = data.access_token;
    sessionStorage.setItem(TOKEN_KEY, state.token);
    $("password").value = "";
    await enter();
  } catch {
    showSignIn("The server did not answer.");
  } finally {
    button.disabled = false;
  }
}

// enter opens the tools once the account's grant is confirmed. Only this check turns a non-GM away: any other trouble
// (the world server restarting) leaves the tools open and showing it.
async function enter() {
  try {
    await api("/api/v1/gm/status");
  } catch (error) {
    if (error.status === 401) return;
    if (error.status === 403) { signOut(error.message); return; }
  }
  $("signin").hidden = true;
  $("app").hidden = false;
  show(state.view);
  poll();
  loadPlaces();
}

// ---- what the server says

function applyGm(gm) {
  state.gm = gm;
  state.gmNote = "";
  if (gm.commands && gm.commands.length && !state.catalog.length) { state.catalog = gm.commands; renderCatalog(); } // a command newer than this page still gets a plain form
  renderSession();
}

async function refreshGm() {
  try {
    applyGm(await api("/api/v1/gm"));
  } catch (error) {
    state.gm = null;
    state.gmNote = error.message;
    renderSession();
  }
}

async function refreshStatus() {
  try {
    const status = await api("/api/v1/gm/status");
    state.online = true;
    renderStats(status);
  } catch (error) {
    if (error.status === 403) { signOut(error.message); return; }
    if (error.status === 0 || error.status >= 500) state.online = false;
  }
  const server = $("server");
  server.textContent = state.online === false ? "World offline" : "World online";
  server.className = "pill " + (state.online === false ? "bad" : "good");
}

async function refreshPlayers() {
  try {
    state.players = (await api("/api/v1/gm/players")).players || [];
    renderPlayers();
  } catch { /* the header says the world is offline */ }
}

async function loadPlaces() {
  try {
    state.places = (await api("/api/v1/gm/places")).places || [];
    renderPlaces();
  } catch { /* tried again when the view opens */ }
}

// Polling: a hidden tab asks nothing; the launcher's panel shares the same budget.
async function poll() {
  clearTimeout(state.timer);
  if (!document.hidden && state.token) {
    await refreshGm();
    if (!state.token) return;
    await refreshStatus();
    if (!state.token) return;
    await refreshPlayers();
    if (!state.token) return;
  }
  state.timer = setTimeout(poll, 5000);
}

// ---- rendering

function renderSession() {
  const toggle = $("gm-toggle");
  const session = $("session");
  toggle.hidden = !inWorld();
  if (inWorld()) {
    session.textContent = state.gm.character + " · GM mode " + (state.gm.enabled ? "on" : "off");
    session.className = "session " + (state.gm.enabled ? "good" : "muted");
    toggle.textContent = state.gm.enabled ? "Turn GM mode off" : "Turn GM mode on";
  } else {
    session.textContent = "Not in the world";
    session.className = "session muted";
  }
  const help = !inWorld()
    ? (state.gmNote || "Enter the world with your GM account first.") + " You can still look around here, but commands need your character in the world."
    : state.gm.enabled ? "" : "GM mode is off, so worldd will refuse commands. Turn it on above.";
  for (const id of ["session-help", "commands-help"]) { $(id).hidden = !help; $(id).textContent = help; }
  if (state.wasOn !== gmOn()) { state.wasOn = gmOn(); renderPlayers(); renderPlaces(); updateGive(); updateMoney(); }
  renderInspectButtons();
}

function renderStats(s) {
  const stats = s.stats || {};
  const tile = (value, label) => el("div", { className: "stat" }, el("b", {}, value), el("span", {}, label));
  $("stats").replaceChildren(
    tile(num(stats.players), "Players"),
    tile(num(stats.npcs), "NPCs"),
    tile(num(stats.resource_nodes), "Resource nodes"),
    tile(num(stats.mounts), "Mounts"),
    tile(num(stats.tick_ms, 2) + " ms", "Network loop pass"),
    tile(num(stats.publish_max_ms, 2) + " ms", "Worst publish, last minute or two"),
    tile(num((Number(s.world_time) || 0) / 3600, 1) + " h", "World clock"),
    tile(num(s.subscribers), "Open world streams"),
  );
}

function selfPlayer() {
  return inWorld() ? state.players.find((p) => p.name.toLowerCase() === state.gm.character.toLowerCase()) : null;
}

function renderPlayers() {
  $("player-count").textContent = "(" + state.players.length + ")";
  const filter = $("player-filter").value.trim().toLowerCase();
  const shown = state.players.filter((p) => p.name.toLowerCase().includes(filter));
  $("players-empty").hidden = shown.length > 0;
  $("players-empty").textContent = state.players.length ? "No online player by that name." : "Nobody is in the world.";
  const me = selfPlayer();
  $("players").replaceChildren(...shown.map((p) => {
    const pos = p.position || {};
    const flags = [p.dead && "Dead", p.mounted && "Mounted"].filter(Boolean).join(", ") || "Alive";
    const name = quote(p.name);
    const isMe = me && me.name === p.name;
    const actions = el("div", { className: "actions" });
    const add = (label, handler, enabled = gmOn()) => actions.append(el("button", { onclick: handler, disabled: !enabled, type: "button" }, label));
    add("Go to", () => run(`teleport-xyz self ${pos.x} ${pos.y} ${pos.z}`), gmOn() && !isMe);
    add("Bring here", () => { const at = selfPlayer().position; run(`teleport-xyz ${name} ${at.x} ${at.y} ${at.z}`); }, gmOn() && !!me && !isMe);
    add("Give items", () => openGive("give-items", p.name), true);
    add("Give money", () => openGive("give-money", p.name), true);
    return el("tr", {},
      el("td", {}, p.name),
      el("td", {}, `${cm(pos.x)}, ${cm(pos.y)}, ${cm(pos.z)}`),
      el("td", {}, flags),
      el("td", {}, "×" + num(p.move_speed_multiplier || 1, 2)),
      el("td", {}, actions));
  }));
}

function renderPlaces() {
  const filter = $("place-filter").value.trim().toLowerCase();
  const rows = state.places.filter((p) => !filter || (p.name + " " + p.display_name).toLowerCase().includes(filter));
  $("places").replaceChildren(...rows.map((p) => {
    const pos = p.position || {};
    return el("tr", {},
      el("td", {}, p.display_name || p.name),
      el("td", { className: "muted" }, p.name),
      el("td", {}, `${cm(pos.x)}, ${cm(pos.y)}, ${cm(pos.z)}`),
      el("td", {}, el("button", { type: "button", disabled: !gmOn(), onclick: () => run("teleport self " + quote(p.name)) }, "Go to")));
  }));
}

// ---- find and inspect

async function find(event) {
  event.preventDefault();
  const note = $("find-note");
  note.hidden = true;
  try {
    const reply = await api("/api/v1/gm/search?q=" + encodeURIComponent($("find-query").value.trim()));
    const results = reply.results || [];
    if (reply.truncated || !results.length) {
      note.hidden = false;
      note.textContent = results.length ? "Showing the first 50 matches. Narrow the search for the rest." : "Nothing by that name.";
    }
    $("find-results").replaceChildren(...results.map(resultRow));
  } catch (error) {
    note.hidden = false;
    note.textContent = error.message;
  }
}

function stateText(bits) {
  return [bits & 2 && "Dead", bits & 1 && "In combat", bits & 4 && "Mounted"].filter(Boolean).join(", ") || "–";
}

function resultRow(result) {
  const entity = result.entity || {};
  const row = el("tr", { className: "pick", tabIndex: 0 },
    el("td", {}, result.name || entity.name || "(no name)"),
    el("td", {}, KINDS[entity.kind] || "?"),
    el("td", {}, entity.level ? String(entity.level) : "–"),
    el("td", {}, entity.health_pct === 255 || entity.health_pct === undefined ? "–" : entity.health_pct + "%"),
    el("td", {}, stateText(entity.state || 0)));
  const open = () => {
    for (const other of $("find-results").children) other.classList.remove("selected");
    row.classList.add("selected");
    inspect(entity.id);
  };
  row.addEventListener("click", open);
  row.addEventListener("keydown", (event) => { if (event.key === "Enter") open(); });
  return row;
}

let inspectButtons = [];

async function inspect(id) {
  state.inspected = id;
  const panel = $("inspect");
  panel.hidden = false;
  try {
    renderInspect(await api("/api/v1/gm/inspect?id=" + encodeURIComponent(id)));
  } catch (error) {
    inspectButtons = [];
    panel.replaceChildren(el("p", { className: "bad" }, error.message));
  }
}

function renderInspect(reply) {
  const entity = reply.entity || {};
  const actor = reply.actor;
  const player = reply.player;
  const id = entity.id;
  const facts = [["Id", id], ["Kind", KINDS[entity.kind] || "?"], ["Position", `${cm(entity.x_cm)}, ${cm(entity.y_cm)}, ${cm(entity.z_cm)}`]];
  if (player) {
    facts.push(["Level", player.level], ["Character", player.character_id], ["State", stateText(entity.state || 0)], ["Speed", "×" + num(player.move_speed_multiplier || 1, 2)]);
  }
  if (actor) {
    facts.push(["Level", actor.level], ["Archetype", actor.archetype], ["Made by", actor.source]);
    if (entity.kind === "ENTITY_KIND_NPC") {
      facts.push(
        ["Health", actor.combatant ? `${num(actor.health)} / ${num(actor.max_health)}` : "Full (not fought yet)"],
        ["AI", actor.ai_state],
        ["Target", actor.target && actor.target !== "0" ? actor.target : "–"],
        ["Threat", (actor.threat || []).map((t) => `${t.id}: ${num(t.threat)}`).join(", ") || "–"],
        ["From home", cm(actor.distance_from_home_cm) + " cm"],
        ["Spawn", `${cm(actor.spawn_x_cm)}, ${cm(actor.spawn_y_cm)}, ${cm(actor.spawn_z_cm)}`],
        ["Respawn", `${num(actor.respawn_seconds)} s, ${actor.respawn_rule || "–"}`]);
      if (actor.corpse) facts.push(["Corpse", `${actor.corpse_loot_stacks} loot stacks, closes in ${num(actor.corpse_closes_in_seconds)} s`]);
      if (Number(actor.respawn_in_seconds) > 0) facts.push(["Back in", num(actor.respawn_in_seconds) + " s"]);
    }
  }
  const list = el("dl");
  for (const [term, value] of facts) list.append(el("dt", {}, term), el("dd", {}, String(value ?? "–")));

  inspectButtons = [];
  const actions = el("div", { className: "actions" });
  const add = (label, makeLine, danger) => {
    const button = el("button", {
      type: "button",
      className: danger ? "danger" : "",
      onclick: async () => { if (await run(makeLine())) inspect(id); },
    }, label);
    inspectButtons.push(button);
    actions.append(button);
  };
  add("Go to", () => `teleport-xyz self ${entity.x_cm} ${entity.y_cm} ${entity.z_cm}`);
  if (entity.kind === "ENTITY_KIND_NPC") {
    add("Heal", () => "npc-heal " + id);
    add("Reset", () => "npc-reset " + id);
    add("Bring here", () => { const at = selfPlayer().position; return `npc-move ${id} ${at.x} ${at.y} ${at.z}`; });
    add("Respawn", () => "npc-respawn " + id, true);
    add("Kill", () => "npc-kill " + id, true);
    add("Despawn", () => "npc-despawn " + id, true);
  }
  $("inspect").replaceChildren(el("h3", {}, (actor && actor.name) || (player && player.name) || entity.name || "(no name)"), list, actions);
  renderInspectButtons();
}

function renderInspectButtons() {
  for (const button of inspectButtons) button.disabled = !gmOn() || (button.textContent === "Bring here" && !selfPlayer());
}

// ---- navigation

function show(view) {
  state.view = view;
  for (const section of document.querySelectorAll(".view")) section.hidden = section.id !== "view-" + view;
  for (const button of document.querySelectorAll(".nav button")) {
    if (button.dataset.view === view) button.setAttribute("aria-current", "page");
    else button.removeAttribute("aria-current");
  }
  if (view === "places") { if (state.places.length) renderPlaces(); else loadPlaces(); }
  if (view === "give-items" || view === "give-money") openedGive(view);
}

document.addEventListener("DOMContentLoaded", () => {
  $("signin-form").addEventListener("submit", signIn);
  $("signout").addEventListener("click", () => signOut());
  $("gm-toggle").addEventListener("click", () => run(gmOn() ? "off" : "on"));
  $("find-form").addEventListener("submit", find);
  setupTools();
  $("player-filter").addEventListener("input", renderPlayers);
  $("place-filter").addEventListener("input", renderPlaces);
  for (const button of document.querySelectorAll(".nav button")) button.addEventListener("click", () => show(button.dataset.view));
  document.addEventListener("visibilitychange", () => { if (!document.hidden && state.token) poll(); });
  if (state.token) enter();
  else showSignIn();
});
