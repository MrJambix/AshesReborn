// The live map: every player online as a dot that moves, the city nodes with their levels, and on request the creatures, resource nodes,
// mounts and loot of the area in view. It reads /api/v1/gm/map once a second and /api/v1/gm/nodes every 15 s, only while it is open.
"use strict";

const KIND = { player: 1, npc: 2, resource: 3, mount: 4, loot: 5 };
const LAYERS = [
  { key: "terrain", title: "Game map", on: true },
  { key: "players", title: "Players", kind: KIND.player, on: true },
  { key: "nodes", title: "Nodes", on: true },
  { key: "npcs", title: "Creatures", kind: KIND.npc, on: false, heavy: true },
  { key: "resources", title: "Resources", kind: KIND.resource, on: false, heavy: true },
  { key: "mounts", title: "Mounts", kind: KIND.mount, on: false, heavy: true },
  { key: "loot", title: "Loot", kind: KIND.loot, on: false, heavy: true },
];
// the envelope of the design data's locations (move_checker.h): what Fit shows
const WORLD = { minX: -2008064, maxX: 432254, minY: -912785, maxY: 1381376 };
const HEAVY_VIEW_CM = 800000;   // creatures and the rest are asked for only when the view is under 8 km across
const NODE_COLORS = ["#6f6a5e", "#9cc2a0", "#7fb3e0", "#d8b874", "#e8875f"];
const DOT_COLORS = { 2: "#ef978c", 3: "#7ccf8a", 4: "#c9a77a", 5: "#f0d070" };
const STATE_COMBAT = 1, STATE_DEAD = 2, STATE_MOUNTED = 4;

const map = {
  on: false, timer: 0, nodeTimer: 0, frame: 0, ctx: null,
  cx: 0, cy: 0, scale: 0,                     // the centre in turned world space (cm) and pixels per cm
  turn: Number(localStorage.getItem("gm-map-turn") || 0),
  layers: Object.fromEntries(LAYERS.map((l) => [l.key, JSON.parse(localStorage.getItem("gm-map-" + l.key) ?? String(l.on))])),
  dots: new Map(),                            // id -> { e, from:[x,y], to:[x,y], at }
  trails: new Map(),                          // player id -> [[x, y], ...]
  nodes: [],
  selected: null,                             // { type: "dot", id } | { type: "node", name } | { type: "point", x, y }
  follow: 0,
  hover: null,
  drag: null,
  live: null,
  heavyNote: "",
  mini: null,                                 // the game's own map: { manifest, overview, tiles: Map "x_y" -> ImageBitmap | "loading" | "failed" } or { missing }
  miniQueue: [], miniLoading: 0,
};

// ---- world <-> screen. Turn 0 has world +X up and +Y right; each turn is a quarter more, clockwise.

function turned(wx, wy) {
  let a = wy, b = -wx;
  for (let i = 0; i < map.turn; i++) [a, b] = [-b, a];
  return [a, b];
}
function unturned(a, b) {
  for (let i = 0; i < map.turn; i++) [a, b] = [b, -a];
  return [-b, a];
}
function toScreen(wx, wy) {
  const [a, b] = turned(wx, wy);
  const c = map.ctx.canvas;
  return [(a - map.cx) * map.scale + c.clientWidth / 2, (b - map.cy) * map.scale + c.clientHeight / 2];
}
function toWorld(sx, sy) {
  const c = map.ctx.canvas;
  return unturned((sx - c.clientWidth / 2) / map.scale + map.cx, (sy - c.clientHeight / 2) / map.scale + map.cy);
}

function fit() {
  const c = map.ctx.canvas;
  const corners = [[WORLD.minX, WORLD.minY], [WORLD.minX, WORLD.maxY], [WORLD.maxX, WORLD.minY], [WORLD.maxX, WORLD.maxY]].map(([x, y]) => turned(x, y));
  const as = corners.map((p) => p[0]), bs = corners.map((p) => p[1]);
  map.cx = (Math.min(...as) + Math.max(...as)) / 2;
  map.cy = (Math.min(...bs) + Math.max(...bs)) / 2;
  map.scale = Math.min(c.clientWidth / (Math.max(...as) - Math.min(...as)), c.clientHeight / (Math.max(...bs) - Math.min(...bs))) * 0.95;
}

function centreOn(wx, wy, scale) {
  [map.cx, map.cy] = turned(wx, wy);
  if (scale) map.scale = scale;
}

// the world box in view, for the reads of the heavy layers
function viewBox() {
  const c = map.ctx.canvas;
  const pts = [[0, 0], [c.clientWidth, 0], [0, c.clientHeight], [c.clientWidth, c.clientHeight]].map(([x, y]) => toWorld(x, y));
  const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]);
  return { minX: Math.floor(Math.min(...xs)), minY: Math.floor(Math.min(...ys)), maxX: Math.ceil(Math.max(...xs)), maxY: Math.ceil(Math.max(...ys)) };
}

// ---- the game's own map (mapc minimap): an overview of the whole of Verra, and tiles of 2.5 km for a closer view

// a picture from the API, with the login: the map is the game's art, kept behind the GM sign-in like everything else
async function fetchPicture(name) {
  const reply = await fetch("/api/v1/gm/minimap/" + name, { headers: { Authorization: "Bearer " + state.token }, signal: AbortSignal.timeout(15000) });
  if (!reply.ok) throw new ApiError(reply.status, "no picture");
  return createImageBitmap(await reply.blob());
}

async function loadMinimap() {
  if (map.mini) return;
  map.mini = { tiles: new Map() };
  try {
    map.mini.manifest = await api("/api/v1/gm/minimap/minimap.json");
    map.mini.overview = await fetchPicture("overview.png");
  } catch {
    map.mini = { missing: true, tiles: new Map() };
  }
  renderMapStatus();
}

function queueTile(x, y) {
  const key = `${x}_${y}`;
  map.mini.tiles.set(key, "loading");
  map.miniQueue.push([x, y]);
  pumpTiles();
}

function pumpTiles() {
  while (map.miniLoading < 6 && map.miniQueue.length) {
    const [x, y] = map.miniQueue.shift();
    map.miniLoading++;
    fetchPicture(`s${map.mini.manifest.tile_cm}_x${x}_y${y}.png`)
      .then((img) => map.mini.tiles.set(`${x}_${y}`, img))
      .catch(() => map.mini.tiles.set(`${x}_${y}`, "failed"))
      .finally(() => { map.miniLoading--; pumpTiles(); });
  }
}

// draws a picture that covers world X from x0 to x1 and Y from y0 to y1, its top at x1 and its left at y0 (the tiles' way), whatever the turn
function placePicture(img, x0, x1, y0, y1, ratio) {
  const ctx = map.ctx;
  const tl = toScreen(x1, y0), tr = toScreen(x1, y1), bl = toScreen(x0, y0);
  ctx.setTransform(ratio * (tr[0] - tl[0]) / img.width, ratio * (tr[1] - tl[1]) / img.width, ratio * (bl[0] - tl[0]) / img.height, ratio * (bl[1] - tl[1]) / img.height,
    ratio * tl[0], ratio * tl[1]);
  ctx.drawImage(img, 0, 0);
}

function drawMinimap(ratio) {
  const mm = map.mini;
  if (!map.layers.terrain || !mm || !mm.manifest) return;
  const m = mm.manifest, s = m.tile_cm, ctx = map.ctx;
  ctx.save();
  ctx.globalAlpha = 0.92;
  if (mm.overview) placePicture(mm.overview, m.min_x * s, (m.max_x + 1) * s, m.min_y * s, (m.max_y + 1) * s, ratio);
  if (s * map.scale > m.overview_px * 1.25) {   // closer than the overview is sharp: the tiles in view, fetched as they are needed
    const box = viewBox();
    for (let x = Math.max(m.min_x, Math.floor(box.minX / s)); x <= Math.min(m.max_x, Math.floor(box.maxX / s)); x++) {
      for (let y = Math.max(m.min_y, Math.floor(box.minY / s)); y <= Math.min(m.max_y, Math.floor(box.maxY / s)); y++) {
        const tile = mm.tiles.get(`${x}_${y}`);
        if (tile === undefined) queueTile(x, y);
        else if (typeof tile === "object") placePicture(tile, x * s, (x + 1) * s, y * s, (y + 1) * s, ratio);
      }
    }
  }
  ctx.restore();
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
}

// ---- reading the server

async function readMap() {
  const kinds = LAYERS.filter((l) => l.kind && map.layers[l.key]);
  const light = kinds.filter((l) => !l.heavy).map((l) => l.kind);
  const box = viewBox();
  const wide = Math.max(box.maxX - box.minX, box.maxY - box.minY);
  const heavy = kinds.filter((l) => l.heavy).map((l) => l.kind);
  map.heavyNote = heavy.length && wide > HEAVY_VIEW_CM ? "Zoom in under 8 km to see " + kinds.filter((l) => l.heavy).map((l) => l.title.toLowerCase()).join(", ") + "." : "";
  const asks = [];
  if (light.length) asks.push(api("/api/v1/gm/map?kinds=" + light.join(",")));
  if (heavy.length && !map.heavyNote) asks.push(api(`/api/v1/gm/map?kinds=${heavy.join(",")}&box=${box.minX},${box.minY},${box.maxX},${box.maxY}`));
  if (!asks.length) { map.live = null; renderMapStatus(); return; }
  try {
    const frames = await Promise.all(asks);
    const seen = new Set();
    const now = performance.now();
    for (const f of frames) {
      for (const e of f.entities || []) {
        seen.add(e.id);
        const dot = map.dots.get(e.id);
        const at = dot ? position(dot, now) : [e.x, e.y];
        map.dots.set(e.id, { e: { ...e, n: e.n || (dot && dot.e.n) || "" }, from: at, to: [e.x, e.y], at: now });
        if (e.k === KIND.player) {
          const trail = map.trails.get(e.id) || [];
          const last = trail[trail.length - 1];
          if (!last || Math.hypot(last[0] - e.x, last[1] - e.y) > 100) trail.push([e.x, e.y]);
          if (trail.length > 60) trail.shift();
          map.trails.set(e.id, trail);
        }
      }
    }
    for (const id of map.dots.keys()) if (!seen.has(id)) { map.dots.delete(id); map.trails.delete(id); }
    map.live = frames[0];
  } catch (error) {
    map.live = { live: false, problem: error.message, entities: [] };
  }
  renderMapStatus();
  renderMapLists();
  if (map.selected && map.selected.type === "dot") renderMapPanel();
}

async function readNodes() {
  try {
    map.nodes = (await api("/api/v1/gm/nodes")).nodes || [];
  } catch { /* the next read tries again */ }
  renderMapLists();
  if (map.selected && map.selected.type === "node") renderMapPanel();
}

function position(dot, now) {
  const t = Math.min(1, (now - dot.at) / 1000);
  return [dot.from[0] + (dot.to[0] - dot.from[0]) * t, dot.from[1] + (dot.to[1] - dot.from[1]) * t];
}

// ---- drawing

function gridStep() {
  for (const step of [10000, 50000, 100000, 500000, 1000000]) if (step * map.scale >= 70) return step;
  return 2000000;
}

function draw() {
  if (!map.on) return;
  map.frame = requestAnimationFrame(draw);
  const ctx = map.ctx, c = ctx.canvas;
  const ratio = window.devicePixelRatio || 1;
  if (c.width !== Math.round(c.clientWidth * ratio) || c.height !== Math.round(c.clientHeight * ratio)) {
    c.width = Math.round(c.clientWidth * ratio);
    c.height = Math.round(c.clientHeight * ratio);
    if (!map.scale) fit();
  }
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  const W = c.clientWidth, H = c.clientHeight, now = performance.now();
  if (map.follow) {
    const dot = map.dots.get(map.follow);
    if (dot) [map.cx, map.cy] = turned(...position(dot, now));
  }
  ctx.fillStyle = "#0b0c0f";
  ctx.fillRect(0, 0, W, H);
  drawMinimap(ratio);
  const onMap = map.layers.terrain && map.mini && map.mini.manifest;

  // the grid, in km
  const step = gridStep();
  const box = viewBox();
  ctx.lineWidth = 1;
  ctx.font = "11px Segoe UI, system-ui, sans-serif";
  for (const axis of ["x", "y"]) {
    const lo = axis === "x" ? box.minX : box.minY, hi = axis === "x" ? box.maxX : box.maxY;
    for (let v = Math.ceil(lo / step) * step; v <= hi; v += step) {
      const [x1, y1] = axis === "x" ? toScreen(v, box.minY) : toScreen(box.minX, v);
      const [x2, y2] = axis === "x" ? toScreen(v, box.maxY) : toScreen(box.maxX, v);
      ctx.strokeStyle = onMap ? (v % (step * 5) === 0 ? "rgba(0, 0, 0, .35)" : "rgba(0, 0, 0, .15)") : v === 0 ? "#3a3426" : v % (step * 5) === 0 ? "#24211a" : "#16150f";
      ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x2, y2); ctx.stroke();
    }
  }
  // the world's edge (the game's map shows its own coasts)
  ctx.strokeStyle = onMap ? "rgba(0, 0, 0, 0)" : "#393123";
  ctx.setLineDash([4, 4]);
  ctx.beginPath();
  [[WORLD.minX, WORLD.minY], [WORLD.minX, WORLD.maxY], [WORLD.maxX, WORLD.maxY], [WORLD.maxX, WORLD.minY]].forEach(([x, y], i) => {
    const [sx, sy] = toScreen(x, y);
    if (i) ctx.lineTo(sx, sy); else ctx.moveTo(sx, sy);
  });
  ctx.closePath(); ctx.stroke();
  ctx.setLineDash([]);

  // the small dots: creatures, resources, mounts, loot
  for (const dot of map.dots.values()) {
    const e = dot.e;
    if (e.k === KIND.player) continue;
    const [sx, sy] = toScreen(...position(dot, now));
    if (sx < -5 || sy < -5 || sx > W + 5 || sy > H + 5) continue;
    ctx.fillStyle = e.s & STATE_DEAD ? "#555" : DOT_COLORS[e.k] || "#999";
    const r = map.scale > 0.02 ? 3 : 2;
    if (e.k === KIND.resource) ctx.fillRect(sx - r, sy - r, r * 2, r * 2);
    else { ctx.beginPath(); ctx.arc(sx, sy, r, 0, Math.PI * 2); ctx.fill(); }
    if (e.k === KIND.npc && e.s & STATE_COMBAT) { ctx.strokeStyle = "#ef978c"; ctx.beginPath(); ctx.arc(sx, sy, r + 3, 0, Math.PI * 2); ctx.stroke(); }
  }

  // the city nodes
  if (map.layers.nodes) {
    for (const n of map.nodes) {
      const [sx, sy] = toScreen(n.x, n.y);
      if (sx < -80 || sy < -20 || sx > W + 80 || sy > H + 20) continue;
      const level = Math.max(0, n.level);
      const r = 4 + level * 1.6;
      ctx.fillStyle = NODE_COLORS[level] || NODE_COLORS[0];
      ctx.strokeStyle = "#090a0c";
      ctx.lineWidth = 2;
      ctx.beginPath(); ctx.moveTo(sx, sy - r); ctx.lineTo(sx + r, sy); ctx.lineTo(sx, sy + r); ctx.lineTo(sx - r, sy); ctx.closePath();
      ctx.fill(); ctx.stroke();
      if (level >= 3 || (level >= 1 && map.scale > 0.0006) || map.scale > 0.002) {   // the whole world shows only Villages and Towns by name
        ctx.fillStyle = level >= 3 ? "#f3eddf" : "#b8b3a6";
        ctx.font = (level >= 3 ? "600 12px" : "11px") + " Segoe UI, system-ui, sans-serif";
        ctx.fillText(n.display_name || n.name, sx + r + 4, sy + 4);
      }
      if (isSelected("node", n.name)) ring(sx, sy, r + 5);
    }
  }

  // the players, last so they are on top: a trail, the dot, the way they face, the name
  if (map.layers.players) {
    for (const dot of map.dots.values()) {
      const e = dot.e;
      if (e.k !== KIND.player) continue;
      const trail = map.trails.get(e.id) || [];
      if (trail.length > 1) {
        ctx.strokeStyle = "rgba(216, 184, 116, .35)";
        ctx.lineWidth = 2;
        ctx.beginPath();
        trail.forEach(([x, y], i) => { const [sx, sy] = toScreen(x, y); if (i) ctx.lineTo(sx, sy); else ctx.moveTo(sx, sy); });
        const [px, py] = toScreen(...position(dot, now));
        ctx.lineTo(px, py);
        ctx.stroke();
      }
      const [wx, wy] = position(dot, now);
      const [sx, sy] = toScreen(wx, wy);
      const me = inWorld() && e.n && e.n.toLowerCase() === state.gm.character.toLowerCase();
      const yaw = (e.yaw / 256) * Math.PI * 2;
      const [hx, hy] = toScreen(wx + Math.cos(yaw) * 1, wy + Math.sin(yaw) * 1);
      const len = Math.hypot(hx - sx, hy - sy) || 1;
      ctx.strokeStyle = "#f3eddf";
      ctx.lineWidth = 2;
      ctx.beginPath(); ctx.moveTo(sx, sy); ctx.lineTo(sx + ((hx - sx) / len) * 11, sy + ((hy - sy) / len) * 11); ctx.stroke();
      ctx.fillStyle = e.s & STATE_DEAD ? "#777" : me ? "#d8b874" : "#5fb0ff";
      ctx.strokeStyle = e.s & STATE_COMBAT ? "#ef978c" : "#090a0c";
      ctx.lineWidth = e.s & STATE_COMBAT ? 3 : 2;
      ctx.beginPath(); ctx.arc(sx, sy, 6, 0, Math.PI * 2); ctx.fill(); ctx.stroke();
      ctx.font = "600 12px Segoe UI, system-ui, sans-serif";
      ctx.fillStyle = "#090a0c";
      const label = (e.n || "player") + (e.s & STATE_MOUNTED ? " (mounted)" : "") + (e.s & STATE_DEAD ? " (dead)" : "");
      ctx.fillText(label, sx + 10, sy - 7);
      ctx.fillStyle = me ? "#d8b874" : "#f3eddf";
      ctx.fillText(label, sx + 9, sy - 8);
      if (isSelected("dot", e.id)) ring(sx, sy, 11);
    }
  }
  if (map.selected && map.selected.type === "point") {
    const [sx, sy] = toScreen(map.selected.x, map.selected.y);
    ctx.strokeStyle = "#d8b874"; ctx.lineWidth = 2;
    ctx.beginPath(); ctx.moveTo(sx - 7, sy); ctx.lineTo(sx + 7, sy); ctx.moveTo(sx, sy - 7); ctx.lineTo(sx, sy + 7); ctx.stroke();
  }
  // the scale bar
  const metres = step / 100;
  ctx.fillStyle = "#999790";
  ctx.font = "11px Segoe UI, system-ui, sans-serif";
  ctx.fillRect(12, H - 14, step * map.scale, 2);
  ctx.fillText(metres >= 1000 ? `${metres / 1000} km` : `${metres} m`, 12, H - 20);
}

function ring(sx, sy, r) {
  const ctx = map.ctx;
  ctx.strokeStyle = "#d8b874";
  ctx.lineWidth = 2;
  ctx.beginPath(); ctx.arc(sx, sy, r, 0, Math.PI * 2); ctx.stroke();
}

const isSelected = (type, key) => map.selected && map.selected.type === type && (type === "node" ? map.selected.name === key : map.selected.id === key);

// ---- finding what is under the pointer

function hitTest(sx, sy) {
  const now = performance.now();
  let best = null, bestD = 12;
  const consider = (d, hit) => { if (d < bestD) { bestD = d; best = hit; } };
  for (const dot of map.dots.values()) {
    if (dot.e.k === KIND.player && !map.layers.players) continue;
    const [x, y] = toScreen(...position(dot, now));
    consider(Math.hypot(x - sx, y - sy) - (dot.e.k === KIND.player ? 4 : 0), { type: "dot", id: dot.e.id });
  }
  if (map.layers.nodes) {
    for (const n of map.nodes) {
      const [x, y] = toScreen(n.x, n.y);
      consider(Math.hypot(x - sx, y - sy) - 2, { type: "node", name: n.name });
    }
  }
  return best;
}

function describe(hit) {
  if (!hit) return "";
  if (hit.type === "node") {
    const n = map.nodes.find((x) => x.name === hit.name);
    return n ? `${n.display_name || n.name} · ${n.level_name || "level unknown"}` : "";
  }
  const dot = map.dots.get(hit.id);
  if (!dot) return "";
  const e = dot.e;
  const kind = { 1: "Player", 2: "Creature", 3: "Resource node", 4: "Mount", 5: "Loot" }[e.k];
  return [e.n || kind, e.l ? "level " + e.l : "", e.hp !== 255 && e.k === KIND.npc ? e.hp + "%" : "", e.s & STATE_DEAD ? "dead" : e.s & STATE_COMBAT ? "in combat" : ""].filter(Boolean).join(" · ");
}

// ---- the panel of what is selected

function select(hit) {
  map.selected = hit;
  if (hit && hit.type === "dot" && map.follow && map.follow !== hit.id) map.follow = 0;
  renderMapPanel();
  if (hit && hit.type === "dot") {
    const dot = map.dots.get(hit.id);
    if (dot && dot.e.k !== KIND.player) loadDetail(hit.id);
  }
}

async function loadDetail(id) {
  try {
    const reply = await api("/api/v1/gm/inspect?id=" + encodeURIComponent(id));
    if (map.selected && map.selected.id === id) { map.selected.detail = reply; renderMapPanel(); }
  } catch { /* the panel shows what the stream says */ }
}

const metresText = (x, y) => `${num(x / 100)}, ${num(y / 100)} m`;

function renderMapPanel() {
  const panel = $("map-panel");
  if (!panel) return;
  const sel = map.selected;
  if (!sel) { panel.hidden = true; return; }
  panel.hidden = false;
  const facts = [];
  const buttons = [];
  const add = (label, handler, { danger = false, enabled = gmOn() } = {}) =>
    buttons.push(el("button", { type: "button", className: danger ? "danger" : "", disabled: !enabled, onclick: handler }, label));
  const me = selfPlayer();
  let title = "";
  if (sel.type === "point") {
    title = "A point";
    facts.push(["Where", metresText(sel.x, sel.y)]);
    add("Teleport me here", () => run(`teleport-xyz self ${Math.round(sel.x)} ${Math.round(sel.y)}`));
  } else if (sel.type === "node") {
    const n = map.nodes.find((x) => x.name === sel.name);
    if (!n) { panel.hidden = true; return; }
    title = n.display_name || n.name;
    facts.push(["Record", n.name], ["Level", n.level >= 0 ? `${n.level} · ${n.level_name}` : "Node service not answering"], ["Experience", `${num(n.experience)} (${n.percent}% of the level)`]);
    if (Number(n.to_next) > 0) facts.push(["To the next level", num(n.to_next)]);
    if (n.parent) facts.push(["Vassal of", n.parent]);
    facts.push(["Where", metresText(n.x, n.y)]);
    add("Go to", () => run(`teleport self ${quote(n.name)}`));
    const level = el("select", {}, ...LEVELS.map((l) => el("option", { value: l.value, selected: Number(l.value) === n.level }, l.title + " " + (["Wilderness", "Crossroads", "Encampment", "Village", "Town"][Number(l.value)] || ""))));
    const xp = el("input", { type: "number", min: 1, step: 1, value: 1000 });
    panel.replaceChildren(el("h3", {}, title), factList(facts), el("div", { className: "actions" }, ...buttons),
      el("label", {}, "Node level"), el("div", { className: "row tight" }, level, el("button", { type: "button", disabled: !gmOn(), onclick: async () => { if (await run(`node-level ${quote(n.name)} ${level.value}`)) setTimeout(readNodes, 1500); } }, "Set")),
      el("label", {}, "Add node XP"), el("div", { className: "row tight" }, xp, el("button", { type: "button", disabled: !gmOn(), onclick: async () => { if (await run(`node-xp ${quote(n.name)} ${Math.round(Number(xp.value))}`)) setTimeout(readNodes, 1500); } }, "Add")));
    return;
  } else {
    const dot = map.dots.get(sel.id);
    if (!dot) { panel.replaceChildren(el("p", { className: "muted" }, "That has left the area or the world.")); return; }
    const e = dot.e;
    const detail = sel.detail || {};
    const actor = detail.actor;
    if (e.k === KIND.player) {
      title = e.n || "Player";
      const name = quote(e.n);
      const isMe = me && e.n && me.name.toLowerCase() === e.n.toLowerCase();
      facts.push(["Level", e.l || "–"], ["Where", metresText(e.x, e.y)], ["State", stateText(e.s)]);
      add(map.follow === e.id ? "Stop following" : "Follow", () => { map.follow = map.follow === e.id ? 0 : e.id; renderMapPanel(); }, { enabled: true });
      add("Go to", () => run(`teleport-xyz self ${e.x} ${e.y} ${e.z}`), { enabled: gmOn() && !isMe });
      add("Bring here", () => { const at = selfPlayer().position; run(`teleport-xyz ${name} ${at.x} ${at.y} ${at.z}`); }, { enabled: gmOn() && !!me && !isMe });
      add("Heal", () => run(`heal ${name}`), { enabled: gmOn() && !(e.s & STATE_DEAD) });
      add("Give items", () => openView("give-items", e.n), { enabled: true });
      add("Give money", () => openView("give-money", e.n), { enabled: true });
      add("Progression", () => openView("progression", e.n), { enabled: true });
    } else {
      title = (actor && actor.name) || { 2: "Creature", 3: "Resource node", 4: "Mount", 5: "Loot" }[e.k];
      facts.push(["Id", e.id], ["Level", e.l || "–"], ["Where", metresText(e.x, e.y)], ["State", stateText(e.s)]);
      if (e.k === KIND.npc) facts.push(["Health", e.hp === 255 ? "–" : e.hp + "%"]);
      if (actor && actor.archetype) facts.push(["Archetype", actor.archetype]);
      add("Go to", () => run(`teleport-xyz self ${e.x} ${e.y} ${e.z}`));
      if (e.k === KIND.npc) {
        add("Heal", () => run("npc-heal " + e.id));
        add("Reset", () => run("npc-reset " + e.id));
        add("Bring here", () => { const at = selfPlayer().position; run(`npc-move ${e.id} ${at.x} ${at.y} ${at.z}`); }, { enabled: gmOn() && !!me });
        add("Respawn", () => run("npc-respawn " + e.id), { danger: true });
        add("Kill", () => run("npc-kill " + e.id), { danger: true });
        add("Despawn", () => run("npc-despawn " + e.id), { danger: true });
      }
    }
  }
  panel.replaceChildren(el("h3", {}, title), factList(facts), el("div", { className: "actions" }, ...buttons));
}

function factList(facts) {
  const list = el("dl");
  for (const [term, value] of facts) list.append(el("dt", {}, term), el("dd", {}, String(value ?? "–")));
  return list;
}

// ---- the status line and the lists beside the map

function renderMapStatus() {
  const pill = $("map-status");
  const f = map.live;
  if (!f) { pill.textContent = "Not live"; pill.className = "pill"; }
  else if (f.live) { pill.textContent = "Live"; pill.className = "pill good"; }
  else { pill.textContent = f.problem || "Waiting for the world stream"; pill.className = "pill bad"; }
  const counts = {};
  for (const d of map.dots.values()) counts[d.e.k] = (counts[d.e.k] || 0) + 1;
  const parts = [counts[1] ? `${counts[1]} player${counts[1] === 1 ? "" : "s"}` : "no players", counts[2] && `${num(counts[2])} creatures`, counts[3] && `${num(counts[3])} resources`,
    counts[4] && `${num(counts[4])} mounts`, counts[5] && `${num(counts[5])} loot`, map.nodes.length && `${map.nodes.length} nodes`].filter(Boolean);
  const missing = map.layers.terrain && map.mini && map.mini.missing ? " · The game's map is not on this server (mapc minimap)." : "";
  $("map-counts").textContent = parts.join(" · ") + (map.heavyNote ? " · " + map.heavyNote : "") + missing;
}

function renderMapLists() {
  const filter = $("map-filter").value.trim().toLowerCase();
  const players = [...map.dots.values()].filter((d) => d.e.k === KIND.player && (d.e.n || "").toLowerCase().includes(filter))
    .sort((a, b) => (a.e.n || "").localeCompare(b.e.n || ""));
  $("map-players").replaceChildren(...(players.length ? players.map((d) => {
    const li = el("li", { tabIndex: 0, className: isSelected("dot", d.e.id) ? "selected" : "" }, el("span", {}, d.e.n || "player"),
      el("span", { className: "muted small" }, [d.e.l ? "lvl " + d.e.l : "", stateText(d.e.s) !== "–" ? stateText(d.e.s) : ""].filter(Boolean).join(" · ")));
    const go = () => { centreOn(d.to[0], d.to[1], Math.max(map.scale, 0.004)); select({ type: "dot", id: d.e.id }); renderMapLists(); };
    li.addEventListener("click", go);
    li.addEventListener("keydown", (ev) => { if (ev.key === "Enter") go(); });
    return li;
  }) : [el("li", { className: "muted" }, map.layers.players ? "Nobody online" : "The Players layer is off")]));
  const nodes = [...map.nodes].filter((n) => (n.display_name + " " + n.name).toLowerCase().includes(filter)).sort((a, b) => b.level - a.level || a.display_name.localeCompare(b.display_name));
  $("map-nodes").replaceChildren(...nodes.slice(0, 120).map((n) => {
    const li = el("li", { tabIndex: 0, className: isSelected("node", n.name) ? "selected" : "" }, el("span", {}, n.display_name || n.name),
      el("span", { className: "muted small" }, n.level >= 0 ? n.level_name : ""));
    const go = () => { centreOn(n.x, n.y, Math.max(map.scale, 0.002)); select({ type: "node", name: n.name }); renderMapLists(); };
    li.addEventListener("click", go);
    li.addEventListener("keydown", (ev) => { if (ev.key === "Enter") go(); });
    return li;
  }));
}

// ---- opening and closing

async function tickMap() {
  clearTimeout(map.timer);
  if (!map.on) return;
  if (!document.hidden) await readMap();
  map.timer = setTimeout(tickMap, 1000);
}

async function tickNodes() {
  clearTimeout(map.nodeTimer);
  if (!map.on) return;
  if (!document.hidden) await readNodes();
  map.nodeTimer = setTimeout(tickNodes, 15000);
}

function startMap() {
  if (map.on || !state.token) return;
  map.on = true;
  cancelAnimationFrame(map.frame);
  map.frame = requestAnimationFrame(draw);
  loadMinimap();
  tickMap();
  tickNodes();
}

function stopMap() {
  map.on = false;
  clearTimeout(map.timer);
  clearTimeout(map.nodeTimer);
  cancelAnimationFrame(map.frame);
}

function showOnMap(name) {
  show("map");
  const p = state.players.find((x) => x.name === name);
  if (p && p.position) centreOn(p.position.x, p.position.y, Math.max(map.scale, 0.004));
  const dot = [...map.dots.values()].find((d) => d.e.n === name);
  if (dot) select({ type: "dot", id: dot.e.id });
}

function setupMap() {
  const canvas = $("map-canvas");
  map.ctx = canvas.getContext("2d");
  $("map-layers").append(...LAYERS.map((l) => {
    const b = el("button", { type: "button" }, l.title);
    b.setAttribute("aria-pressed", String(map.layers[l.key]));
    b.addEventListener("click", () => {
      map.layers[l.key] = !map.layers[l.key];
      localStorage.setItem("gm-map-" + l.key, String(map.layers[l.key]));
      b.setAttribute("aria-pressed", String(map.layers[l.key]));
      if (l.kind) for (const [id, d] of map.dots) if (d.e.k === l.kind && !map.layers[l.key]) map.dots.delete(id);
      if (l.key === "terrain") { renderMapStatus(); return; }
      readMap();
      renderMapLists();
    });
    return b;
  }));
  $("map-fit").addEventListener("click", () => { map.follow = 0; fit(); });
  $("map-rotate").addEventListener("click", () => {
    const c = canvas;
    const centre = toWorld(c.clientWidth / 2, c.clientHeight / 2);
    map.turn = (map.turn + 1) % 4;
    localStorage.setItem("gm-map-turn", String(map.turn));
    centreOn(centre[0], centre[1]);
  });
  $("map-filter").addEventListener("input", renderMapLists);
  canvas.addEventListener("wheel", (ev) => {
    ev.preventDefault();
    const r = canvas.getBoundingClientRect();
    const sx = ev.clientX - r.left, sy = ev.clientY - r.top;
    const before = toWorld(sx, sy);
    map.scale = Math.min(0.5, Math.max(0.00002, map.scale * Math.exp(-ev.deltaY * 0.0015)));
    const [a, b] = turned(...before);
    map.cx = a - (sx - canvas.clientWidth / 2) / map.scale;
    map.cy = b - (sy - canvas.clientHeight / 2) / map.scale;
  }, { passive: false });
  canvas.addEventListener("pointerdown", (ev) => {
    canvas.setPointerCapture(ev.pointerId);
    map.drag = { x: ev.clientX, y: ev.clientY, cx: map.cx, cy: map.cy, moved: false };
  });
  canvas.addEventListener("pointermove", (ev) => {
    const r = canvas.getBoundingClientRect();
    if (map.drag) {
      const dx = ev.clientX - map.drag.x, dy = ev.clientY - map.drag.y;
      if (Math.abs(dx) + Math.abs(dy) > 4) { map.drag.moved = true; map.follow = 0; }
      if (map.drag.moved) { map.cx = map.drag.cx - dx / map.scale; map.cy = map.drag.cy - dy / map.scale; }
    }
    const hit = map.drag && map.drag.moved ? null : hitTest(ev.clientX - r.left, ev.clientY - r.top);
    const tip = $("map-tip");
    const text = describe(hit);
    tip.hidden = !text;
    if (text) { tip.textContent = text; tip.style.left = ev.clientX - r.left + 14 + "px"; tip.style.top = ev.clientY - r.top + 10 + "px"; }
    canvas.style.cursor = map.drag && map.drag.moved ? "grabbing" : hit ? "pointer" : "grab";
  });
  canvas.addEventListener("pointerleave", () => { $("map-tip").hidden = true; });
  canvas.addEventListener("pointerup", (ev) => {
    const drag = map.drag;
    map.drag = null;
    if (!drag || drag.moved) return;
    const r = canvas.getBoundingClientRect();
    const sx = ev.clientX - r.left, sy = ev.clientY - r.top;
    const hit = hitTest(sx, sy);
    if (hit) { select(hit); renderMapLists(); return; }
    const [x, y] = toWorld(sx, sy);
    if (ev.shiftKey && gmOn()) { run(`teleport-xyz self ${Math.round(x)} ${Math.round(y)}`); return; }
    select({ type: "point", x, y });
  });
  document.addEventListener("visibilitychange", () => { if (!document.hidden && map.on) { tickMap(); tickNodes(); } });
  renderMapStatus();
}

document.addEventListener("DOMContentLoaded", setupMap);
