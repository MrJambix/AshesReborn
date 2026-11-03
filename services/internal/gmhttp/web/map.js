// The live map, drawn as the game draws its own: the game's map pictures (the painted Region layer, the Local layer when close, each town's backdrop
// at its level), the game's icons and legend, and on them every player online as a marker that moves. It reads /api/v1/gm/map once a second,
// /api/v1/gm/nodes every 15 s and /api/v1/gm/mapdata (the game's map records) once, only while it is open.
"use strict";

const KIND = { player: 1, npc: 2, resource: 3, mount: 4, loot: 5 };
const LIVE_LAYERS = [
  { key: "terrain", title: "Game map", on: true },
  { key: "players", title: "Players", kind: KIND.player, on: true },
  { key: "npcs", title: "Creatures", kind: KIND.npc, on: false, heavy: true },
  { key: "resources", title: "Resources", kind: KIND.resource, on: false, heavy: true },
  { key: "mounts", title: "Mounts", kind: KIND.mount, on: false, heavy: true },
  { key: "loot", title: "Loot", kind: KIND.loot, on: false, heavy: true },
];
// the envelope of the design data's locations (move_checker.h): what Fit shows
const WORLD = { minX: -2008064, maxX: 432254, minY: -912785, maxY: 1381376 };
const HEAVY_VIEW_CM = 800000;   // creatures and the rest are asked for only when the view is under 8 km across
const REGION_SCALE = 0.0006;    // px per cm from which the Region tiles are sharper than the overview
const LOCAL_SCALE = 0.0025;     // px per cm from which the game switches to its Local layer (and the towns' backdrops)
// the smallest scale (px per cm) at which each kind of mark shows, as the game's map hides the small ones from afar
const SHOW_FROM = { node: 0, poi_major: 0.0003, dungeon: 0.0003, landmark: 0.0003, poi_minor: 0.0008, shrine: 0.0008, location: 0.0012, special: 0.004, quest: 0.004, vendor: 0.004 };
const KIND_TITLE = { poi_major: "Major point of interest", poi_minor: "Minor point of interest", dungeon: "Dungeon", landmark: "Landmark", location: "Named place",
  special: "Special place", shrine: "Respawn shrine", quest: "Quest giver", vendor: "Vendor" };
const DOT_COLORS = { 2: "#ef978c", 3: "#7ccf8a", 4: "#c9a77a", 5: "#f0d070" };
const STATE_COMBAT = 1, STATE_DEAD = 2, STATE_MOUNTED = 4;
const ICON_PX = 26;

const map = {
  on: false, timer: 0, nodeTimer: 0, frame: 0, frameNo: 0, ctx: null,
  cx: 0, cy: 0, scale: 0,                     // the centre in turned world space (cm) and pixels per cm
  turn: Number(localStorage.getItem("gm-map-turn") || 0),
  layers: Object.fromEntries(LIVE_LAYERS.map((l) => [l.key, JSON.parse(localStorage.getItem("gm-map-" + l.key) ?? String(l.on))])),
  legendOff: new Set(JSON.parse(localStorage.getItem("gm-map-legend-off") || "[]")),   // the legend's kinds turned off
  dots: new Map(),                            // id -> { e, from:[x,y], to:[x,y], at }
  trails: new Map(),                          // player id -> [[x, y], ...]
  nodes: [],                                  // the live nodes (positions and levels)
  data: null,                                 // the game's map records: { nodes, points, legend }
  art: new Map(),                             // node name -> its art (icons by level, backdrop, yaw)
  selected: null,                             // { type: "dot", id } | { type: "node", name } | { type: "point", index } | { type: "ground", x, y }
  follow: 0,
  drag: null,
  live: null,
  heavyNote: "",
  mini: null,                                 // mapc minimap's manifest, with the tile sets: { manifest, region:Set, local:Set, icons:Set } or { missing }
  pics: new Map(),                            // file -> { img, used } | "loading" | "failed"
  picQueue: [], picLoading: 0,
};

// ---- world <-> screen. Turn 0 has world +X up and +Y right (the game's map); each turn is a quarter more, clockwise.

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

// the world box in view
function viewBox(margin = 0) {
  const c = map.ctx.canvas;
  const pts = [[-margin, -margin], [c.clientWidth + margin, -margin], [-margin, c.clientHeight + margin], [c.clientWidth + margin, c.clientHeight + margin]].map(([x, y]) => toWorld(x, y));
  const xs = pts.map((p) => p[0]), ys = pts.map((p) => p[1]);
  return { minX: Math.floor(Math.min(...xs)), minY: Math.floor(Math.min(...ys)), maxX: Math.ceil(Math.max(...xs)), maxY: Math.ceil(Math.max(...ys)) };
}

// ---- the game's pictures (mapc minimap), fetched with the login and kept while they are in use

async function fetchPicture(name) {
  const reply = await fetch("/api/v1/gm/minimap/" + name, { headers: { Authorization: "Bearer " + state.token }, signal: AbortSignal.timeout(20000) });
  if (!reply.ok) throw new ApiError(reply.status, "no picture");
  return createImageBitmap(await reply.blob());
}

async function loadMinimap() {
  if (map.mini) return;
  map.mini = { region: new Set(), local: new Set(), icons: new Set() };
  try {
    const m = await api("/api/v1/gm/minimap/minimap.json");
    if (m.version !== 2) throw new Error("an older map export");
    map.mini.manifest = m;
    for (const key of ["region", "local"]) for (const [x, y] of m.layers[key].tiles) map.mini[key].add(`${x}_${y}`);
    map.mini.icons = new Set(m.icons);
    map.mini.backdrops = new Set(m.backdrops);
  } catch {
    map.mini = { missing: true, region: new Set(), local: new Set(), icons: new Set() };
  }
  renderMapStatus();
  renderLegend();
}

async function loadMapData() {
  if (map.data) return;
  try {
    map.data = await api("/api/v1/gm/mapdata");
    map.art = new Map(map.data.nodes.map((n) => [n.name, n]));
  } catch {
    map.data = null;   // the next opening tries again
  }
  renderLegend();
  renderMapLists();
}

// picture returns the picture when it is here, and asks for it when it is not. The newest asks go first: the view the GM is looking at.
function picture(name) {
  const p = map.pics.get(name);
  if (p && typeof p === "object") { p.used = map.frameNo; return p.img; }
  if (p === undefined) {
    map.pics.set(name, "loading");
    map.picQueue.push({ name, asked: map.frameNo });
    pumpPictures();
  }
  return null;
}

const icon = (name) => (name && map.mini && map.mini.icons.has(name) ? picture(`i_${name}.png`) : null);

function pumpPictures() {
  while (map.picLoading < 8 && map.picQueue.length) {
    const { name, asked } = map.picQueue.pop();
    if (asked < map.frameNo - 20 && !name.startsWith("i_")) { map.pics.delete(name); continue; }   // scrolled away before its turn
    map.picLoading++;
    fetchPicture(name)
      .then((img) => { map.pics.set(name, { img, used: map.frameNo }); if (name.startsWith("i_")) renderLegendIcons(); })
      .catch(() => map.pics.set(name, "failed"))
      .finally(() => { map.picLoading--; forget(); pumpPictures(); });
  }
}

// keeps at most 500 tiles and backdrops: the ones used longest ago go (the icons stay)
function forget() {
  const held = [...map.pics].filter(([n, p]) => typeof p === "object" && !n.startsWith("i_"));
  if (held.length <= 500) return;
  held.sort((a, b) => a[1].used - b[1].used);
  for (const [n, p] of held.slice(0, held.length - 400)) { p.img.close?.(); map.pics.delete(n); }
}

// draws a picture whose top-left, top-right and bottom-left corners stand at these world points, whatever the turn
function placeQuad(img, tl, tr, bl, ratio) {
  const ctx = map.ctx;
  const [a, b, c] = [toScreen(...tl), toScreen(...tr), toScreen(...bl)];
  ctx.setTransform(ratio * (b[0] - a[0]) / img.width, ratio * (b[1] - a[1]) / img.width, ratio * (c[0] - a[0]) / img.height, ratio * (c[1] - a[1]) / img.height,
    ratio * a[0], ratio * a[1]);
  ctx.drawImage(img, 0, 0);
}

// a layer's tiles in view; tile (x, y) is centred on (x, y) times its size, its picture with +X up and +Y right (mapc minimap)
function drawTiles(layer, have, ratio) {
  const s = layer.tile_cm, box = viewBox();
  const x0 = Math.floor(box.minX / s + 0.5), x1 = Math.floor(box.maxX / s + 0.5), y0 = Math.floor(box.minY / s + 0.5), y1 = Math.floor(box.maxY / s + 0.5);
  for (let x = x0; x <= x1; x++) {
    for (let y = y0; y <= y1; y++) {
      if (!have.has(`${x}_${y}`)) continue;
      const img = picture(`${layer.prefix}_x${x}_y${y}.jpg`);
      if (img) placeQuad(img, [(x + 0.5) * s, (y - 0.5) * s], [(x + 0.5) * s, (y + 0.5) * s], [(x - 0.5) * s, (y - 0.5) * s], ratio);
    }
  }
}

// a node's town as it stands: its backdrop at its level, centred on the node and turned by its yaw (mapc minimap)
function drawBackdrops(ratio) {
  const m = map.mini.manifest, half = m.backdrop_cm / 2, box = viewBox(200);
  for (const n of map.nodes) {
    const art = map.art.get(n.name);
    if (!art || !art.backdrop) continue;
    if (art.x + half < box.minX || art.x - half > box.maxX || art.y + half < box.minY || art.y - half > box.maxY) continue;
    const file = `${art.backdrop}_L${Math.min(4, Math.max(0, n.level))}`;
    if (!map.mini.backdrops.has(file)) continue;
    const img = picture(`n_${file}.jpg`);
    if (!img) continue;
    const t = (art.yaw * Math.PI) / 180, c = Math.cos(t), s = Math.sin(t);
    const at = (lx, ly) => [art.x + lx * c - ly * s, art.y + lx * s + ly * c];
    placeQuad(img, at(half, -half), at(half, half), at(-half, -half), ratio);
  }
}

function drawGameMap(ratio) {
  const mm = map.mini;
  if (!map.layers.terrain || !mm || !mm.manifest) return;
  const m = mm.manifest, ov = m.overview, ctx = map.ctx;
  ctx.save();
  const overview = picture(ov.file);
  if (overview) placeQuad(overview, [ov.max_x, ov.min_y], [ov.max_x, ov.max_y], [ov.min_x, ov.min_y], ratio);
  if (map.scale > REGION_SCALE) drawTiles(m.layers.region, mm.region, ratio);
  if (map.scale > LOCAL_SCALE) { drawTiles(m.layers.local, mm.local, ratio); drawBackdrops(ratio); }
  ctx.restore();
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
}

// ---- reading the server

async function readMap() {
  const kinds = LIVE_LAYERS.filter((l) => l.kind && map.layers[l.key]);
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

// ---- what shows

const legendOn = (kind) => !map.legendOff.has(kind);
const pointShows = (p) => legendOn(p.k) && map.scale >= (SHOW_FROM[p.k] ?? 0.004);
const nodeLevel = (n) => Math.min(4, Math.max(0, n.level));
// the nodes the game's map shows: Verra's own (the design data's test and playground nodes stay in the list beside the map only)
const shownNodes = () => (map.data ? map.nodes.filter((n) => map.art.has(n.name)) : map.nodes);

function nodeIcon(n) {
  const art = map.art.get(n.name);
  return art && art.icons.length ? art.icons[Math.min(nodeLevel(n), art.icons.length - 1)] : `TUI_Icon_Node_LVL${nodeLevel(n)}`;
}

// ---- drawing

function gridStep() {
  for (const step of [10000, 50000, 100000, 500000, 1000000]) if (step * map.scale >= 70) return step;
  return 2000000;
}

// the labels of the marks, drawn after the marks, the most important first; one that would cover another is left out, as on the game's map
let labels = [];
const queueLabel = (prio, text, sx, sy, style) => labels.push({ prio, text, sx, sy, style });

function drawLabels() {
  const ctx = map.ctx, taken = [];
  labels.sort((a, b) => b.prio - a.prio);
  for (const l of labels) {
    ctx.font = (l.style.font || "600 12px") + " Segoe UI, system-ui, sans-serif";
    const w = ctx.measureText(l.text).width + 6, h = 15;
    const x0 = l.style.align === "center" ? l.sx - w / 2 : l.sx - 3, y0 = l.sy - 12;
    if (taken.some((r) => x0 < r[2] && x0 + w > r[0] && y0 < r[3] && y0 + h > r[1])) continue;
    taken.push([x0, y0, x0 + w, y0 + h]);
    label(l.text, l.sx, l.sy, l.style);
  }
  labels = [];
}

function label(text, sx, sy, { font = "600 12px", color = "#f3eddf", align = "left" } = {}) {
  const ctx = map.ctx;
  ctx.font = font + " Segoe UI, system-ui, sans-serif";
  ctx.textAlign = align;
  ctx.lineWidth = 3;
  ctx.strokeStyle = "rgba(9, 10, 12, .85)";
  ctx.strokeText(text, sx, sy);
  ctx.fillStyle = color;
  ctx.fillText(text, sx, sy);
  ctx.textAlign = "left";
}

function drawIcon(img, sx, sy, size, angle = 0) {
  const ctx = map.ctx;
  const w = size, h = (size * img.height) / img.width;
  if (!angle) { ctx.drawImage(img, sx - w / 2, sy - h / 2, w, h); return; }
  ctx.save();
  ctx.translate(sx, sy);
  ctx.rotate(angle);
  ctx.drawImage(img, -w / 2, -h / 2, w, h);
  ctx.restore();
}

function draw() {
  if (!map.on) return;
  map.frame = requestAnimationFrame(draw);
  map.frameNo++;
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
  drawGameMap(ratio);
  const onMap = map.layers.terrain && map.mini && map.mini.manifest;

  // without the game's map: a grid in km and the world's edge
  if (!onMap) {
    const step = gridStep();
    const box = viewBox();
    ctx.lineWidth = 1;
    for (const axis of ["x", "y"]) {
      const lo = axis === "x" ? box.minX : box.minY, hi = axis === "x" ? box.maxX : box.maxY;
      for (let v = Math.ceil(lo / step) * step; v <= hi; v += step) {
        const [x1, y1] = axis === "x" ? toScreen(v, box.minY) : toScreen(box.minX, v);
        const [x2, y2] = axis === "x" ? toScreen(v, box.maxY) : toScreen(box.maxX, v);
        ctx.strokeStyle = v === 0 ? "#3a3426" : v % (step * 5) === 0 ? "#24211a" : "#16150f";
        ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x2, y2); ctx.stroke();
      }
    }
    ctx.strokeStyle = "#393123";
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    [[WORLD.minX, WORLD.minY], [WORLD.minX, WORLD.maxY], [WORLD.maxX, WORLD.maxY], [WORLD.maxX, WORLD.minY]].forEach(([x, y], i) => {
      const [sx, sy] = toScreen(x, y);
      if (i) ctx.lineTo(sx, sy); else ctx.moveTo(sx, sy);
    });
    ctx.closePath(); ctx.stroke();
    ctx.setLineDash([]);
  }

  // the names of places, under everything else, as the game writes them on its map
  const points = map.data ? map.data.points : [];
  if (legendOn("location")) {
    for (const p of points) {
      if ((p.k !== "location" && p.k !== "special") || !pointShows(p)) continue;
      const [sx, sy] = toScreen(p.x, p.y);
      if (sx < -150 || sy < -20 || sx > W + 150 || sy > H + 20) continue;
      queueLabel(0, p.title, sx, sy, { font: "italic 500 12px", color: p.k === "special" ? "#e8d9b0" : "#efe6cf", align: "center" });
    }
  }

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
    if (e.k === KIND.npc && e.s & STATE_COMBAT) { ctx.strokeStyle = "#ef978c"; ctx.lineWidth = 1; ctx.beginPath(); ctx.arc(sx, sy, r + 3, 0, Math.PI * 2); ctx.stroke(); }
  }

  // the game's marks: points of interest, dungeons, shrines, quest givers, vendors
  points.forEach((p, index) => {
    if (p.k === "location" || p.k === "special" || !pointShows(p)) return;
    const [sx, sy] = toScreen(p.x, p.y);
    if (sx < -40 || sy < -40 || sx > W + 40 || sy > H + 40) return;
    const img = icon(p.icon);
    const size = Math.round((p.size || 32) * (ICON_PX / 32));
    if (img) drawIcon(img, sx, sy, size);
    else { ctx.fillStyle = "#d8b874"; ctx.beginPath(); ctx.arc(sx, sy, 4, 0, Math.PI * 2); ctx.fill(); }
    const major = p.k === "poi_major" || p.k === "dungeon" || p.k === "landmark";
    if (major ? map.scale > 0.0006 : map.scale > 0.006) queueLabel(major ? 2 : 1, p.title, sx, sy + size / 2 + 11, { font: "600 11px", align: "center" });
    if (isSelected("point", index)) ring(sx, sy, size / 2 + 4);
  });

  // the city nodes, with the game's icon of their level
  if (legendOn("node")) {
    for (const n of shownNodes()) {
      const [sx, sy] = toScreen(n.x, n.y);
      if (sx < -80 || sy < -30 || sx > W + 80 || sy > H + 30) continue;
      const img = icon(nodeIcon(n));
      const size = 30 + nodeLevel(n) * 2;
      if (img) drawIcon(img, sx, sy, size);
      else { ctx.fillStyle = "#d8b874"; ctx.beginPath(); ctx.arc(sx, sy, 6, 0, Math.PI * 2); ctx.fill(); }
      if (n.level >= 3 || (n.level >= 1 && map.scale > 0.0005) || map.scale > 0.0012) {
        queueLabel(3 + nodeLevel(n), n.display_name || n.name, sx, sy + size / 2 + 12, { font: n.level >= 3 ? "700 13px" : "600 12px", color: "#f6e7bf", align: "center" });
      }
      if (isSelected("node", n.name)) ring(sx, sy, size / 2 + 4);
    }
  }
  drawLabels();

  // the players, last so they are on top: a trail, the game's marker turned the way they face, the name
  if (map.layers.players && legendOn("player")) {
    for (const dot of map.dots.values()) {
      const e = dot.e;
      if (e.k !== KIND.player) continue;
      const trail = map.trails.get(e.id) || [];
      if (trail.length > 1) {
        ctx.strokeStyle = "rgba(216, 184, 116, .45)";
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
      const [hx, hy] = toScreen(wx + Math.cos(yaw), wy + Math.sin(yaw));
      const angle = Math.atan2(hy - sy, hx - sx) + Math.PI / 2;   // the markers point up
      const img = icon(me ? "TUI_Icon_PlayerFacing" : "TUI_Icon_PartyFacing");
      if (e.s & (STATE_COMBAT | STATE_DEAD)) {
        ctx.strokeStyle = e.s & STATE_DEAD ? "#999" : "#ef978c";
        ctx.lineWidth = 2;
        ctx.beginPath(); ctx.arc(sx, sy, 13, 0, Math.PI * 2); ctx.stroke();
      }
      if (img) drawIcon(img, sx, sy, 26, angle);
      else {
        ctx.fillStyle = me ? "#d8b874" : "#5fb0ff";
        ctx.strokeStyle = "#090a0c"; ctx.lineWidth = 2;
        ctx.beginPath(); ctx.arc(sx, sy, 6, 0, Math.PI * 2); ctx.fill(); ctx.stroke();
      }
      const text = (e.n || "player") + (e.s & STATE_MOUNTED ? " (mounted)" : "") + (e.s & STATE_DEAD ? " (dead)" : "");
      label(text, sx + 14, sy - 8, { color: me ? "#f0cf7f" : "#e3f0ff" });
      if (isSelected("dot", e.id)) ring(sx, sy, 15);
    }
  }
  if (map.selected && map.selected.type === "ground") {
    const [sx, sy] = toScreen(map.selected.x, map.selected.y);
    ctx.strokeStyle = "#d8b874"; ctx.lineWidth = 2;
    ctx.beginPath(); ctx.moveTo(sx - 7, sy); ctx.lineTo(sx + 7, sy); ctx.moveTo(sx, sy - 7); ctx.lineTo(sx, sy + 7); ctx.stroke();
  }
  // the scale bar
  const step = gridStep();
  const metres = step / 100;
  ctx.fillStyle = "#f3eddf";
  ctx.fillRect(12, H - 14, step * map.scale, 2);
  label(metres >= 1000 ? `${metres / 1000} km` : `${metres} m`, 12, H - 20, { font: "11px" });
}

function ring(sx, sy, r) {
  const ctx = map.ctx;
  ctx.strokeStyle = "#d8b874";
  ctx.lineWidth = 2;
  ctx.beginPath(); ctx.arc(sx, sy, r, 0, Math.PI * 2); ctx.stroke();
}

const isSelected = (type, key) => map.selected && map.selected.type === type &&
  (type === "node" ? map.selected.name === key : type === "point" ? map.selected.index === key : map.selected.id === key);

// ---- finding what is under the pointer

function hitTest(sx, sy) {
  const now = performance.now();
  let best = null, bestD = 14;
  const consider = (d, hit) => { if (d < bestD) { bestD = d; best = hit; } };
  for (const dot of map.dots.values()) {
    if (dot.e.k === KIND.player && (!map.layers.players || !legendOn("player"))) continue;
    const [x, y] = toScreen(...position(dot, now));
    consider(Math.hypot(x - sx, y - sy) - (dot.e.k === KIND.player ? 6 : 0), { type: "dot", id: dot.e.id });
  }
  if (legendOn("node")) {
    for (const n of shownNodes()) {
      const [x, y] = toScreen(n.x, n.y);
      consider(Math.hypot(x - sx, y - sy) - 8, { type: "node", name: n.name });
    }
  }
  if (map.data) {
    map.data.points.forEach((p, index) => {
      if (!pointShows(p)) return;
      const [x, y] = toScreen(p.x, p.y);
      consider(Math.hypot(x - sx, y - sy) - (p.k === "location" || p.k === "special" ? 0 : 6), { type: "point", index });
    });
  }
  return best;
}

function describe(hit) {
  if (!hit) return "";
  if (hit.type === "node") {
    const n = map.nodes.find((x) => x.name === hit.name);
    return n ? `${n.display_name || n.name} · ${n.level_name || "level unknown"}` : "";
  }
  if (hit.type === "point") {
    const p = map.data.points[hit.index];
    return p ? `${p.title} · ${KIND_TITLE[p.k] || p.k}` : "";
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
const goTo = (x, y, z) => run(z ? `teleport-xyz self ${Math.round(x)} ${Math.round(y)} ${Math.round(z)}` : `teleport-xyz self ${Math.round(x)} ${Math.round(y)}`);

function renderMapPanel() {
  const panel = $("map-panel");
  if (!panel) return;
  const sel = map.selected;
  if (!sel) { panel.hidden = true; return; }
  panel.hidden = false;
  const facts = [];
  const buttons = [];
  const add = (text, handler, { danger = false, enabled = gmOn() } = {}) =>
    buttons.push(el("button", { type: "button", className: danger ? "danger" : "", disabled: !enabled, onclick: handler }, text));
  const me = selfPlayer();
  let title = "";
  if (sel.type === "ground") {
    title = "A point";
    facts.push(["Where", metresText(sel.x, sel.y)]);
    add("Teleport me here", () => goTo(sel.x, sel.y));
  } else if (sel.type === "point") {
    const p = map.data && map.data.points[sel.index];
    if (!p) { panel.hidden = true; return; }
    title = p.title;
    facts.push(["Kind", KIND_TITLE[p.k] || p.k]);
    if (p.note && p.note !== KIND_TITLE[p.k]) facts.push([p.k === "quest" ? "Quest" : p.k === "vendor" ? "Shop" : "About", p.note]);
    facts.push(["Record", p.name], ["Where", metresText(p.x, p.y)]);
    if (p.box && p.box[2] > p.box[0]) facts.push(["Size", `${num((p.box[2] - p.box[0]) / 100)} x ${num((p.box[3] - p.box[1]) / 100)} m`]);
    add("Go to", () => goTo(p.x, p.y, p.z));
    add("Zoom in", () => { centreOn(p.x, p.y, Math.max(map.scale, 0.004)); }, { enabled: true });
  } else if (sel.type === "node") {
    const n = map.nodes.find((x) => x.name === sel.name);
    if (!n) { panel.hidden = true; return; }
    const art = map.art.get(n.name);
    title = n.display_name || n.name;
    facts.push(["Record", n.name], ["Level", n.level >= 0 ? `${n.level} · ${n.level_name}` : "Node service not answering"], ["Experience", `${num(n.experience)} (${n.percent}% of the level)`]);
    if (Number(n.to_next) > 0) facts.push(["To the next level", num(n.to_next)]);
    if (art && art.type) facts.push(["Type", art.type]);
    if (n.parent) facts.push(["Vassal of", n.parent]);
    facts.push(["Where", metresText(n.x, n.y)]);
    add("Go to", () => run(`teleport self ${quote(n.name)}`));
    add("Zoom to the town", () => centreOn(n.x, n.y, Math.max(map.scale, 0.006)), { enabled: true });
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

// ---- the legend: the game's own, each line a switch

function legendIcon(name) {
  const c = el("canvas", { className: "legend-icon", width: 40, height: 40 });
  c.dataset.icon = name || "";
  return c;
}

function renderLegendIcons() {
  for (const c of document.querySelectorAll("#map-legend canvas.legend-icon")) {
    if (c.dataset.drawn) continue;
    const img = icon(c.dataset.icon);
    if (!img) continue;
    const g = c.getContext("2d");
    const s = Math.min(40 / img.width, 40 / img.height);
    g.drawImage(img, (40 - img.width * s) / 2, (40 - img.height * s) / 2, img.width * s, img.height * s);
    c.dataset.drawn = "1";
  }
}

function renderLegend() {
  const box = $("map-legend");
  if (!box) return;
  if (!map.data) { box.replaceChildren(el("p", { className: "muted small" }, map.on ? "Reading the game's map records…" : "")); return; }
  const counts = {};
  for (const p of map.data.points) counts[p.k] = (counts[p.k] || 0) + 1;
  counts.node = map.nodes.length || map.data.nodes.length;
  counts.player = 1;
  const groups = [];
  for (const g of map.data.legend) {
    const lines = g.entries.filter((e) => e.kind && counts[e.kind]);
    const seen = new Set();
    const rows = lines.filter((e) => !seen.has(e.kind) && seen.add(e.kind)).map((e) => {
      const b = el("button", { type: "button", className: "legend-line", title: e.kind === "player" ? "" : `${num(counts[e.kind])} on the map` },
        legendIcon(e.icon), el("span", {}, e.title), e.kind === "player" ? "" : el("span", { className: "muted small" }, num(counts[e.kind])));
      b.setAttribute("aria-pressed", String(legendOn(e.kind)));
      b.addEventListener("click", () => {
        if (map.legendOff.has(e.kind)) map.legendOff.delete(e.kind); else map.legendOff.add(e.kind);
        localStorage.setItem("gm-map-legend-off", JSON.stringify([...map.legendOff]));
        b.setAttribute("aria-pressed", String(legendOn(e.kind)));
      });
      return b;
    });
    const missing = g.entries.filter((e) => !e.kind || !counts[e.kind]).map((e) => e.title);
    if (!rows.length && !missing.length) continue;
    groups.push(el("div", { className: "legend-group" }, el("h4", {}, g.title), ...rows,
      missing.length ? el("p", { className: "muted small" }, "Not on this map yet: " + missing.join(", ") + ".") : ""));
  }
  box.replaceChildren(...groups);
  renderLegendIcons();
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

function listItem(text, note, selected, go) {
  const li = el("li", { tabIndex: 0, className: selected ? "selected" : "" }, el("span", {}, text), el("span", { className: "muted small" }, note));
  li.addEventListener("click", go);
  li.addEventListener("keydown", (ev) => { if (ev.key === "Enter") go(); });
  return li;
}

function renderMapLists() {
  const filter = $("map-filter").value.trim().toLowerCase();
  const players = [...map.dots.values()].filter((d) => d.e.k === KIND.player && (d.e.n || "").toLowerCase().includes(filter))
    .sort((a, b) => (a.e.n || "").localeCompare(b.e.n || ""));
  $("map-players").replaceChildren(...(players.length ? players.map((d) =>
    listItem(d.e.n || "player", [d.e.l ? "lvl " + d.e.l : "", stateText(d.e.s) !== "–" ? stateText(d.e.s) : ""].filter(Boolean).join(" · "), isSelected("dot", d.e.id),
      () => { centreOn(d.to[0], d.to[1], Math.max(map.scale, 0.004)); select({ type: "dot", id: d.e.id }); renderMapLists(); }))
    : [el("li", { className: "muted" }, map.layers.players ? "Nobody online" : "The Players layer is off")]));
  const nodes = [...map.nodes].filter((n) => (n.display_name + " " + n.name).toLowerCase().includes(filter)).sort((a, b) => b.level - a.level || a.display_name.localeCompare(b.display_name));
  $("map-nodes").replaceChildren(...nodes.slice(0, 120).map((n) =>
    listItem(n.display_name || n.name, n.level >= 0 ? n.level_name : "", isSelected("node", n.name),
      () => { centreOn(n.x, n.y, Math.max(map.scale, 0.002)); select({ type: "node", name: n.name }); renderMapLists(); })));
  const places = $("map-places");
  if (!filter) { places.replaceChildren(el("li", { className: "muted" }, map.data ? `${num(map.data.points.length)} places: type above to find one` : "Reading the game's map records…")); return; }
  const found = map.data ? map.data.points.map((p, index) => ({ p, index })).filter(({ p }) => (p.title + " " + p.name).toLowerCase().includes(filter)) : [];
  places.replaceChildren(...(found.length ? found.slice(0, 80).map(({ p, index }) =>
    listItem(p.title, KIND_TITLE[p.k] || p.k, isSelected("point", index), () => {
      centreOn(p.x, p.y, Math.max(map.scale, SHOW_FROM[p.k] ? SHOW_FROM[p.k] * 2.5 : 0.004, p.k === "quest" || p.k === "vendor" ? 0.008 : 0));
      select({ type: "point", index }); renderMapLists();
    })) : [el("li", { className: "muted" }, "No place by that name")]));
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
  loadMapData();
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
  $("map-layers").append(...LIVE_LAYERS.map((l) => {
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
    if (ev.shiftKey && gmOn()) { goTo(x, y); return; }
    select({ type: "ground", x, y });
  });
  document.addEventListener("visibilitychange", () => { if (!document.hidden && map.on) { tickMap(); tickNodes(); } });
  renderMapStatus();
  renderLegend();
}

document.addEventListener("DOMContentLoaded", setupMap);
