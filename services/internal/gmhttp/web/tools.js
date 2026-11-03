// Pickers, the prebuilt command forms, Give items and Give money.
"use strict";

const pretty = (name) => name.replace(/^Verra_/, "").replace(/_/g, " ");
const valueOf = (input) => input.dataset.value || input.value.trim();
const argument = (value) => (/^[\w.+-]+$/.test(value) ? value : quote(value));
const COPPER_CAP = 10000000; // worldd's purse: 1,000 gold
const EVERYONE_TITLE = "Everyone online";
const playerText = (input) => (valueOf(input) === "*" ? EVERYONE_TITLE.toLowerCase() : input.value.trim());

// ---- searching for what a field names

function entryRow(e) {
  const sub = [e.label ? pretty(e.name) : "", e.level ? "Level " + e.level : "", e.stack > 1 ? "Stack " + e.stack : ""].filter(Boolean).join(" · ");
  return { value: e.value, title: e.label || pretty(e.name), sub, stack: e.stack };
}

const sources = {
  player: async (q, everyone = false) => {
    const rows = [{ value: "self", title: "self", sub: "Your own character" }];
    if (everyone) rows.push({ value: "*", title: EVERYONE_TITLE, sub: "Every player in the world, one after another" });
    for (const p of state.players) rows.push({ value: p.name, title: p.name, sub: "Online" });
    return rows.filter((r) => r.title.toLowerCase().includes(q.toLowerCase()));
  },
  place: async (q) => state.places
    .filter((p) => (p.name + " " + p.display_name).toLowerCase().includes(q.toLowerCase()))
    .slice(0, 50)
    .map((p) => ({ value: p.name, title: p.display_name || p.name, sub: p.display_name ? p.name : "" })),
  actor: async (q) => {
    if (q.length < 2) return [];
    const reply = await api("/api/v1/gm/search?q=" + encodeURIComponent(q));
    return (reply.results || [])
      .filter((r) => r.entity && r.entity.kind === "ENTITY_KIND_NPC")
      .map((r) => ({ value: r.entity.id, title: r.name, sub: `Level ${r.entity.level} · ${stateText(r.entity.state || 0)} · ${r.entity.id}` }));
  },
  catalog: async (q, kind) => {
    const reply = await api(`/api/v1/gm/catalog?kind=${kind}&q=${encodeURIComponent(q)}`);
    return (reply.results || []).map(entryRow);
  },
};

// picker gives an input a list that follows what is typed. A picked row keeps its value apart from what the field shows;
// typing again drops it, and whatever is typed is sent as it stands, so a name the list does not hold can still be tried.
function picker(input, search) {
  const list = el("ul", { className: "pick-list", hidden: true });
  input.setAttribute("autocomplete", "off");
  input.setAttribute("spellcheck", "false");
  const wrap = el("div", { className: "pick" });
  queueMicrotask(() => { input.replaceWith(wrap); wrap.append(input, list); });
  let asked = 0, timer = 0, active = -1;
  const choose = (row) => {
    input.value = row.title;
    input.dataset.value = row.value;
    list.hidden = true;
    input.dispatchEvent(new CustomEvent("picked", { detail: row, bubbles: true }));
  };
  const mark = () => [...list.children].forEach((li, i) => {
    li.classList.toggle("active", i === active);
    if (i === active) li.scrollIntoView({ block: "nearest" });
  });
  const refresh = async () => {
    const ask = ++asked;
    let rows;
    try {
      rows = await search(input.value.trim());
    } catch (error) {
      rows = error.status === 401 ? [] : [{ note: error.message }];
    }
    if (ask !== asked || document.activeElement !== input) return;
    active = -1;
    list.replaceChildren(...rows.map((row) => {
      if (row.note) return el("li", { className: "muted" }, row.note);
      const li = el("li", {}, el("span", {}, row.title));
      if (row.sub) li.append(el("span", { className: "muted small" }, row.sub));
      li.row = row;
      li.addEventListener("mousedown", (event) => { event.preventDefault(); choose(row); });
      return li;
    }));
    list.hidden = rows.length === 0;
  };
  input.addEventListener("input", () => {
    delete input.dataset.value;
    clearTimeout(timer);
    timer = setTimeout(refresh, 200);
  });
  input.addEventListener("focus", refresh);
  input.addEventListener("blur", () => { asked++; list.hidden = true; });
  input.addEventListener("keydown", (event) => {
    const rows = [...list.children].filter((li) => li.row);
    if (event.key === "Escape") list.hidden = true;
    if (list.hidden || !rows.length) return;
    if (event.key === "ArrowDown" || event.key === "ArrowUp") {
      event.preventDefault();
      active = (active + (event.key === "ArrowDown" ? 1 : rows.length - 1) + (active < 0 && event.key === "ArrowUp" ? 1 : 0)) % rows.length;
      mark();
    } else if (event.key === "Enter" && active >= 0) {
      event.preventDefault();
      choose(rows[active].row);
    }
  });
}

function presetButtons(input, values, format = String) {
  return el("div", { className: "presets" }, ...values.map((v) => el("button", {
    type: "button",
    onclick: () => { input.value = v; input.dispatchEvent(new Event("input", { bubbles: true })); },
  }, format(v))));
}

// ---- the prebuilt command forms

function commandFields(spec) {
  return spec.args.split(/\s+/).filter(Boolean).map((raw) => {
    const optional = raw.endsWith("?");
    const arg = optional ? raw.slice(0, -1) : raw;
    const everyone = arg === "character" && EVERYONE.has(spec.name);
    return { arg, optional, everyone, ...((spec.fields && spec.fields[arg]) || fields[arg] || { type: "text", label: arg }) };
  });
}

function allCommands() {
  const known = new Set(COMMANDS.map((c) => c.name));
  const extra = state.catalog.filter((c) => !known.has(c.name))
    .map((c) => ({ name: c.name, category: c.category, args: c.arguments, title: c.name, help: "A command this page has no form for yet." }));
  return COMMANDS.concat(extra);
}

function renderCatalog() {
  const groups = new Map();
  for (const spec of allCommands()) {
    if (!groups.has(spec.category)) groups.set(spec.category, []);
    groups.get(spec.category).push(spec);
  }
  $("catalog").replaceChildren();
  for (const [category, specs] of groups) {
    const group = el("div", { className: "group" });
    for (const spec of specs) {
      const button = el("button", { type: "button", onclick: () => selectCommand(spec) }, spec.title);
      button.dataset.command = spec.name;
      button.setAttribute("aria-pressed", "false");
      group.append(button);
    }
    $("catalog").append(el("h3", {}, category), group);
  }
}

function fieldInput(field, id, values) {
  let input;
  if (field.type === "rarity") {
    input = el("select", { id }, el("option", { value: "" }, "The item's own"), ...RARITIES.map((r) => el("option", { value: r }, r)));
  } else if (field.type === "select") {
    input = el("select", { id }, ...field.options.map((o) => el("option", { value: o.value }, o.title)));
  } else if (field.type === "number") {
    input = el("input", { id, type: "number", step: field.step || "any" });
    if (field.min !== undefined) input.min = field.min;
    if (field.max !== undefined) input.max = field.max;
    if (field.hint) input.placeholder = field.hint;
  } else {
    input = el("input", { id });
    if (field.type === "player") {
      input.value = "self";
      picker(input, (q) => sources.player(q, field.everyone));
    } else if (field.type === "place") {
      picker(input, sources.place);
    } else if (field.type === "actor") {
      input.placeholder = "Name or actor id";
      picker(input, sources.actor);
    } else if (field.type === "catalog") {
      input.placeholder = field.hint || "Search";
      picker(input, (q) => sources.catalog(q, field.kind));
    }
  }
  if (values[field.arg] !== undefined) input.value = values[field.arg];
  input.dataset.arg = field.arg;
  input.dataset.optional = String(field.optional);
  return input;
}

function selectCommand(spec, values = {}) {
  if (spec.view) { openView(spec.view, values.character); return; }
  state.selected = spec;
  state.fields = commandFields(spec);
  for (const button of $("catalog").querySelectorAll("button")) button.setAttribute("aria-pressed", String(button.dataset.command === spec.name));
  $("command-title").textContent = spec.title;
  $("command-help").textContent = spec.help;
  const box = $("command-fields");
  box.replaceChildren();
  for (const field of state.fields) {
    const id = "arg-" + field.arg;
    const input = fieldInput(field, id, values);
    const label = el("label", { htmlFor: id }, field.label + (field.optional ? " (optional)" : ""));
    const holder = el("div", { className: "field" }, label, input);
    if (field.presets) holder.append(presetButtons(input, field.presets));
    box.append(holder);
    if (field.type === "number" && field.hint) holder.append(el("span", { className: "muted small" }, field.hint));
  }
  buildLine();
  queueMicrotask(() => {
    const first = [...box.querySelectorAll("input")].find((input) => !input.value);
    if (first) first.focus();
  });
}

// formProblem says what is wrong with the fields, for the fields worldd would otherwise refuse with only its usage line.
function formProblem() {
  for (const field of state.fields || []) {
    const input = $("command-fields").querySelector(`[data-arg="${field.arg}"]`);
    const value = valueOf(input);
    if (!value) {
      if (!field.optional) return `Fill in ${field.label}.`;
      continue;
    }
    if (field.type !== "number") continue;
    const number = Number(value);
    if (!Number.isFinite(number)) return `${field.label} must be a number.`;
    if (field.min !== undefined && number < field.min) return `${field.label} must be at least ${num(field.min, 2)}.`;
    if (field.max !== undefined && number > field.max) return `${field.label} must be at most ${num(field.max, 2)}.`;
    if (field.step === 1 && !Number.isInteger(number)) return `${field.label} must be a whole number.`;
  }
  return "";
}

function buildLine() {
  if (!state.selected) return;
  const parts = [".GM", state.selected.name];
  for (const input of $("command-fields").querySelectorAll("[data-arg]")) {
    const value = valueOf(input);
    if (value) parts.push(argument(value));
  }
  state.builtLine = parts.join(" ");
  $("command-line").value = state.builtLine;
}

async function submitCommand(event) {
  event.preventDefault();
  const line = $("command-line").value.trim();
  // A line typed by hand is the GM's own: only what the form built is held to the form's rules.
  const problem = line === state.builtLine ? formProblem() : "";
  $("command-problem").hidden = !problem;
  $("command-problem").textContent = problem;
  if (!problem) await run(line);
}

// ---- Give items

const ITEM_FILTERS = [
  ["All", ""], ["Weapons", "Gear_Weapon"], ["Armor", "Gear_Armor"], ["Jewelry", "Gear_Accessory_"], ["Rings", "Gear_Accessory_Ring"],
  ["Necklaces", "Gear_Accessory_Necklace"], ["Earrings", "Gear_Accessory_Earring"], ["Raw gems", "Resource_Raw_Gem"], ["Cut gems", "Resource_Processed_Gem"],
  ["Resources", "Resource_"],
  ["Food & potions", "Consumable_"], ["Recipes", "Consumable_Recipe"], ["Mounts", "Mount_"], ["Bags", "Bag_"],
  ["Cosmetics", "Cosmetic_"], ["GM scrolls", "GM_Scroll"],
];
const give = { prefix: "", item: null, asked: 0, timer: 0, mode: "" };

async function searchItems() {
  const ask = ++give.asked;
  const note = $("give-note");
  try {
    const params = new URLSearchParams({ kind: "item", q: $("give-query").value.trim(), prefix: give.prefix });
    const reply = await api("/api/v1/gm/catalog?" + params);
    if (ask !== give.asked) return;
    const results = reply.results || [];
    note.hidden = !reply.truncated && results.length > 0;
    note.textContent = results.length ? "Showing the first 50. Add a word to narrow it." : "Nothing by that name.";
    $("give-results").replaceChildren(...results.map(itemRow));
  } catch (error) {
    if (ask !== give.asked) return;
    note.hidden = false;
    note.textContent = error.message;
    $("give-results").replaceChildren();
  }
}

function itemRow(e) {
  const row = el("tr", { className: "pick-row", tabIndex: 0 },
    el("td", {}, el("div", {}, pretty(e.name)), el("div", { className: "muted small" }, e.name)),
    el("td", {}, e.level ? String(e.level) : "–"),
    el("td", {}, e.stack ? String(e.stack) : "–"));
  const select = () => {
    for (const other of $("give-results").children) other.classList.remove("selected");
    row.classList.add("selected");
    give.item = e;
    $("give-selected").textContent = pretty(e.name);
    $("give-selected").classList.remove("muted");
    $("give-count-presets").replaceWith(countPresets(e.stack));
    updateGive();
  };
  row.addEventListener("click", select);
  row.addEventListener("keydown", (event) => { if (event.key === "Enter") { event.preventDefault(); select(); } });
  return row;
}

function countPresets(stack) {
  const values = [...new Set([1, 10, 100, stack > 1 ? stack : 1])].sort((a, b) => a - b);
  const box = presetButtons($("give-count"), values);
  box.id = "give-count-presets";
  return box;
}

// ---- the quality of what is given: the item's own, a rarity (the lowest quality of that colour), or an exact quality

const rarityOf = (quality) => RARITY_FIRST.reduce((band, first, i) => (quality >= first ? i : band), 0);

function setQualityMode(mode) {
  give.mode = mode;
  for (const b of $("give-quality-mode").children) b.setAttribute("aria-pressed", String(b.dataset.mode === mode));
  $("give-quality-box").hidden = !mode;
  $("give-quality").disabled = $("give-quality-range").disabled = mode === "rarity";
  if (mode === "rarity") setQuality(RARITY_FIRST[Number($("give-rarity").value)]);
  updateQuality();
}

function setQuality(q) {
  const quality = Math.min(200, Math.max(1, Math.round(Number(q) || 1)));
  $("give-quality").value = quality;
  $("give-quality-range").value = quality;
  $("give-rarity").value = String(rarityOf(quality));
  updateQuality();
}

function updateQuality() {
  const quality = Number($("give-quality").value) || 1;
  const band = rarityOf(quality);
  [...$("give-quality-scale").children].forEach((span, i) => span.classList.toggle("on", i === band));
  const first = RARITY_FIRST[band], last = band === 6 ? 200 : RARITY_FIRST[band + 1] - 1;
  $("give-quality-note").textContent = give.mode === "rarity"
    ? `${RARITIES[band]}: the item gets the lowest quality of that rarity it can have.`
    : `Quality ${quality} is ${RARITIES[band]} (${first}-${last}). Higher in the band rolls higher stats. An item that can never be ${RARITIES[band]} is refused, with the rarities it allows.`;
  updateGive();
}

function qualityWord() {
  if (give.mode === "rarity") return RARITIES[Number($("give-rarity").value)];
  if (give.mode === "quality") return "q" + $("give-quality").value;
  return "";
}

// the Myself / A player / Everyone online buttons under a player field
function whoButtons(box) {
  const input = $(box.dataset.for);
  const choices = [["Myself", "self"], ["A player", ""], [EVERYONE_TITLE, "*"]];
  const mark = () => {
    const v = valueOf(input);
    [...box.children].forEach((b, i) => b.setAttribute("aria-pressed", String(i === 0 ? v === "self" : i === 2 ? v === "*" : v !== "self" && v !== "*")));
  };
  box.append(...choices.map(([label, value]) => el("button", {
    type: "button",
    onclick: () => {
      setPlayer(input, value);
      if (!value) input.focus();
      input.dispatchEvent(new CustomEvent("picked", { bubbles: true }));   // not "input": the picker reads that as typing and drops the value
      mark();
    },
  }, label)));
  input.addEventListener("input", mark);
  input.addEventListener("picked", mark);
  mark();
}

function giveItemProblem() {
  if (!give.item) return "Choose an item.";
  if (!valueOf($("give-player"))) return "Choose a player.";
  const count = Number($("give-count").value);
  if (!Number.isInteger(count) || count < 1 || count > 100000) return "The count must be 1 to 100,000.";
  return "";
}

function updateGive() {
  const problem = giveItemProblem();
  $("give-submit").disabled = !!problem || !gmOn();
  const how = give.mode === "rarity" ? ` (${RARITIES[Number($("give-rarity").value)]})` : give.mode === "quality" ? ` (quality ${$("give-quality").value})` : "";
  $("give-submit").textContent = problem || !give.item ? "Give" : `Give ${num($("give-count").value)}${how} to ${playerText($("give-player"))}`;
  $("give-problem").textContent = gmOn() ? problem : gmOffText();
}

const gmOffText = () => (inWorld() ? "Turn GM mode on to give." : "Enter the world with your GM account to give.");

async function submitGiveItem(event) {
  event.preventDefault();
  if (giveItemProblem() || !gmOn()) return;
  const parts = ["item", argument(valueOf($("give-player"))), give.item.value, $("give-count").value];
  if (qualityWord()) parts.push(qualityWord());
  await run(parts.join(" "));
  $("give-result").textContent = state.lastResult;
}

// ---- Give money

const moneyTotal = () => {
  const part = (id) => Number($(id).value || 0);
  return part("money-gold") * 10000 + part("money-silver") * 100 + part("money-copper");
};

function moneyProblem() {
  const total = moneyTotal();
  if (!valueOf($("money-player"))) return "Choose a player.";
  if (!Number.isInteger(total)) return "Use whole numbers.";
  if (total < 1) return "Enter an amount.";
  if (total > COPPER_CAP) return "A purse holds 1,000 gold at most.";
  return "";
}

function updateMoney() {
  const total = moneyTotal();
  const problem = moneyProblem();
  $("money-total").textContent = Number.isInteger(total) && total > 0 ? `${num(total)} copper` : "";
  $("money-submit").disabled = !!problem || !gmOn();
  $("money-submit").textContent = problem ? "Give" : `Give to ${playerText($("money-player"))}`;
  $("money-problem").textContent = gmOn() ? problem : gmOffText();
}

async function submitMoney(event) {
  event.preventDefault();
  if (moneyProblem() || !gmOn()) return;
  await run(`money ${argument(valueOf($("money-player")))} ${moneyTotal()}`);
  $("money-result").textContent = state.lastResult;
}

function setMoney(gold) {
  $("money-gold").value = gold;
  $("money-silver").value = "";
  $("money-copper").value = "";
  updateMoney();
}

// ---- opening them

function setPlayer(input, name) {
  input.value = name === "*" ? EVERYONE_TITLE : name;
  if (name === "self" || !name) delete input.dataset.value;
  else input.dataset.value = name;
}

const VIEW_PLAYER = { "give-items": "give-player", "give-money": "money-player", progression: "prog-player" };

// openView shows a view, with its player field set to `character` when one is given
function openView(view, character) {
  if (character && VIEW_PLAYER[view]) {
    const input = $(VIEW_PLAYER[view]);
    setPlayer(input, character);
    input.dispatchEvent(new CustomEvent("picked", { bubbles: true }));
  }
  show(view);
  updateGive();
  updateMoney();
}
const openGive = openView;

function openedGive(view) {
  updateGive();
  updateMoney();
  if (view === "give-items" && !$("give-results").childElementCount) searchItems();
}

function setupTools() {
  renderCatalog();
  $("command-form").addEventListener("submit", submitCommand);
  for (const type of ["input", "picked"]) {
    $("command-form").addEventListener(type, (event) => { if (event.target.id !== "command-line") buildLine(); });
    $("give-form").addEventListener(type, updateGive);
    $("money-form").addEventListener(type, updateMoney);
  }
  $("give-form").addEventListener("submit", submitGiveItem);
  $("money-form").addEventListener("submit", submitMoney);
  picker($("give-player"), (q) => sources.player(q, true));
  picker($("money-player"), (q) => sources.player(q, true));
  for (const box of document.querySelectorAll(".who[data-for]")) whoButtons(box);

  $("give-rarity").append(...RARITIES.map((r, i) => el("option", { value: String(i) }, r)));
  $("give-quality-scale").append(...RARITIES.map((r, i) => el("span", { className: "r" + i, title: r }, r.slice(0, 3))));
  for (const b of $("give-quality-mode").children) b.addEventListener("click", () => setQualityMode(b.dataset.mode));
  $("give-rarity").addEventListener("change", () => setQuality(RARITY_FIRST[Number($("give-rarity").value)]));
  $("give-quality").addEventListener("input", () => setQuality($("give-quality").value));
  $("give-quality-range").addEventListener("input", () => setQuality($("give-quality-range").value));

  $("give-query").addEventListener("input", () => { clearTimeout(give.timer); give.timer = setTimeout(searchItems, 200); });
  $("give-search").addEventListener("submit", (event) => { event.preventDefault(); searchItems(); });
  $("give-filters").append(...ITEM_FILTERS.map(([label, prefix]) => {
    const button = el("button", { type: "button" }, label);
    button.setAttribute("aria-pressed", String(prefix === give.prefix));
    button.addEventListener("click", () => {
      give.prefix = prefix;
      for (const other of $("give-filters").children) other.setAttribute("aria-pressed", String(other === button));
      searchItems();
    });
    return button;
  }));
  $("give-count-presets").replaceWith(countPresets(1));

  $("money-presets").append(...[1, 10, 100, 1000].map((gold) => el("button", { type: "button", onclick: () => setMoney(gold) }, num(gold) + " gold")));
}
