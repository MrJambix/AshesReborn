// Progression: one player's Adventuring, the 22 professions and the weapon lines, read from ".GM progress" and changed by the XP and level commands.
"use strict";

const PROG_TABS = [
  { key: "adventuring", title: "Adventuring" },
  { key: "Gathering", title: "Gathering" },
  { key: "Processing", title: "Processing" },
  { key: "Crafting", title: "Crafting" },
  { key: "weapons", title: "Weapons" },
];
const prog = { data: null, tab: "adventuring", selected: {}, everyone: false, all: false, asked: 0 };

const spaced = (name) => name.replace(/^Weapon_/, "").replace(/_/g, " ").replace(/([a-z])([A-Z])/g, "$1 $2");

// parseProgress reads worldd's "kind name key=value ..." lines.
function parseProgress(text) {
  const out = { name: "", adventuring: null, copper: 0, professions: [], weapons: [] };
  for (const line of text.split("\n")) {
    const words = line.trim().split(/\s+/).filter(Boolean);
    if (!words.length) continue;
    const kind = words.shift();
    const name = words.length && !words[0].includes("=") ? words.shift() : "";
    const facts = { name };
    for (const w of words) {
      const at = w.indexOf("=");
      if (at > 0) facts[w.slice(0, at)] = /^-?\d+$/.test(w.slice(at + 1)) ? Number(w.slice(at + 1)) : w.slice(at + 1);
    }
    if (kind === "character") out.name = name;
    else if (kind === "adventuring") out.adventuring = facts;
    else if (kind === "purse") out.copper = facts.copper || 0;
    else if (kind === "profession") out.professions.push(facts);
    else if (kind === "weapon") out.weapons.push(facts);
  }
  return out;
}

const progPlayer = () => valueOf($("prog-player")) || "self";

async function loadProgress() {
  const ask = ++prog.asked;
  const note = $("prog-note");
  if (!gmOn()) {
    note.hidden = false;
    note.textContent = inWorld() ? "Turn GM mode on to read and change a player's progression." : "Enter the world with your GM account to read a player's progression.";
    renderProgress();
    return;
  }
  try {
    const data = parseProgress(await runQuiet(`progress ${argument(progPlayer())}`));
    if (ask !== prog.asked) return;
    prog.data = data;
    note.hidden = true;
  } catch (error) {
    if (ask !== prog.asked) return;
    prog.data = null;
    note.hidden = false;
    note.textContent = error.message;
  }
  renderProgress();
}

function openedProgress() {
  if (!prog.data || prog.data.name.toLowerCase() !== progPlayer().toLowerCase()) loadProgress();
  else renderProgress();
}

function bar(p) {
  const span = (p.next || 0) - (p.from || 0);
  const pct = span > 0 ? Math.max(0, Math.min(100, ((p.xp - p.from) / span) * 100)) : 100;
  const box = el("div", { className: "bar", title: `${num(pct)}% to the next level` });
  const fill = el("i");
  fill.style.width = pct + "%";
  box.append(fill);
  return box;
}

function renderProgress() {
  const d = prog.data;
  $("prog-tabs").replaceChildren(...PROG_TABS.map((t) => {
    const count = d && t.key !== "adventuring" && t.key !== "weapons" ? d.professions.filter((p) => p.category === t.key).length : 0;
    const b = el("button", { type: "button", onclick: () => { prog.tab = t.key; renderProgress(); } }, t.title + (count ? ` (${count})` : ""));
    b.setAttribute("aria-pressed", String(prog.tab === t.key));
    return b;
  }));
  const table = $("prog-table");
  if (!d) { table.replaceChildren(); renderProgCard(); return; }
  if (prog.tab === "adventuring") {
    const a = d.adventuring || {};
    const list = el("dl", { className: "summary" });
    const fact = (term, ...value) => list.append(el("dt", {}, term), el("dd", {}, ...value));
    fact("Character", d.name);
    fact("Level", el("b", {}, String(a.level ?? "–")), a.cap ? ` of ${a.cap}` : "");
    fact("XP", num(a.xp), a.next ? el("span", { className: "muted" }, ` (level ${a.level + 1} at ${num(a.next)})`) : el("span", { className: "muted" }, " (the cap)"), bar(a));
    fact("Experience debt", num(a.debt));
    fact("Purse", `${num(Math.floor(d.copper / 10000))} gold ${num(Math.floor(d.copper / 100) % 100)} silver ${num(d.copper % 100)} copper`);
    table.replaceChildren(list);
  } else {
    const weapons = prog.tab === "weapons";
    const rows = weapons ? d.weapons : d.professions.filter((p) => p.category === prog.tab);
    if (!prog.selected[prog.tab] && rows.length) prog.selected[prog.tab] = rows[0].name;
    const head = weapons ? ["Weapon line", "Level", "XP", ""] : ["Profession", "Certification", "Level", "XP", ""];
    const body = rows.map((p) => {
      const tr = el("tr", { className: "pick-row" + (prog.selected[prog.tab] === p.name ? " selected" : ""), tabIndex: 0 },
        el("td", {}, spaced(p.name)),
        ...(weapons ? [] : [el("td", {}, p.tier)]),
        el("td", {}, String(p.level), el("span", { className: "muted small" }, ` / ${p.cap}`)),
        el("td", { className: "num" }, num(p.xp)),
        el("td", {}, bar(p)));
      const pick = () => { prog.selected[prog.tab] = p.name; renderProgress(); };
      tr.addEventListener("click", pick);
      tr.addEventListener("keydown", (e) => { if (e.key === "Enter") pick(); });
      return tr;
    });
    table.replaceChildren(el("table", {}, el("thead", {}, el("tr", {}, ...head.map((h) => el("th", {}, h)))), el("tbody", {}, ...body)));
  }
  renderProgCard();
}

// one action of the card: a field with presets and a button that runs a command built from the field's value
function action(title, { value = "", min, max, presets = [], button, line, select }) {
  const box = el("div", { className: "action" }, el("label", {}, title));
  const input = select ? el("select", {}, ...select.map((o) => el("option", { value: o.value }, o.title))) : el("input", { type: "number", step: 1 });
  if (min !== undefined) input.min = min;
  if (max !== undefined) input.max = max;
  if (value !== "") input.value = value;
  box.append(input);
  if (presets.length) box.append(presetButtons(input, presets.map((p) => p.value ?? p), (v) => (presets.find((p) => p.value === v) || {}).title || num(v)));
  const go = el("button", { type: "button", className: "wide", disabled: !gmOn() }, button(input.value));
  input.addEventListener("input", () => { go.textContent = button(input.value); });
  input.addEventListener("change", () => { go.textContent = button(input.value); });
  go.addEventListener("click", async () => {
    const text = line(input.value);
    if (!text) return;
    if (await run(text)) loadProgress();
  });
  box.append(go);
  return box;
}

const tierName = (n) => TIERS[n].title.replace(/ \(.*/, "");

// upgrades: the certification one step up (Novice -> Apprentice -> Journeyman -> Master -> GrandMaster), or straight to any higher one;
// "at its top level" sets the level to the new certification's cap as well
function upgrades(p, who, target, what) {
  const box = el("div", { className: "action" }, el("label", {}, "Upgrade the certification"));
  const now = Number(p.tierno) || 0;
  if (now >= TIERS.length - 1) {
    box.append(el("p", { className: "muted small" }, `${prog.all ? "This profession is" : spaced(p.name) + " is"} already ${tierName(TIERS.length - 1)}.`));
    return box;
  }
  const upgrade = async (tier, top) => {
    if (!(await run(`profession-tier ${who} ${target} ${tier}`))) return;
    if (top) await run(`profession-level ${who} ${target} ${(tier + 1) * 10}`);
    loadProgress();
  };
  const next = now + 1;
  box.append(
    el("button", { type: "button", className: "wide", disabled: !gmOn(), onclick: () => upgrade(next, false) }, `Upgrade ${what} to ${tierName(next)}`),
    el("button", { type: "button", className: "wide", disabled: !gmOn(), onclick: () => upgrade(next, true) }, `Upgrade to ${tierName(next)} at level ${(next + 1) * 10}`),
    el("div", { className: "presets" }, ...TIERS.slice(next).map((t) =>
      el("button", { type: "button", disabled: !gmOn(), title: `Straight to ${t.title}`, onclick: () => upgrade(Number(t.value), false) }, tierName(Number(t.value))))));
  return box;
}

function toggle(label, key) {
  const box = el("input", { type: "checkbox", checked: prog[key] });
  box.addEventListener("change", () => { prog[key] = box.checked; renderProgCard(); });
  return el("label", { className: "check" }, box, " " + label);
}

function renderProgCard() {
  const card = $("prog-card");
  const d = prog.data;
  if (!d) { card.replaceChildren(el("p", { className: "muted" }, "Choose a player and press Show.")); return; }
  const who = prog.everyone ? '"*"' : argument(d.name);
  const whoText = prog.everyone ? "everyone online" : d.name;
  const parts = [];
  if (prog.tab === "adventuring") {
    const a = d.adventuring || {};
    parts.push(el("h3", {}, "Adventuring"),
      action("Add XP", { value: 10000, min: 1, presets: [1000, 10000, 100000, 1000000], button: (v) => `Add ${num(v)} XP to ${whoText}`, line: (v) => `xp ${who} ${v}` }),
      action("Raise the level", {
        value: Math.min((a.level || 1) + 1, a.cap || 25), min: 1, max: a.cap || 25,
        presets: [5, 10, 15, 20, a.cap || 25], button: (v) => `Raise ${whoText} to level ${v}`, line: (v) => `level ${who} ${v}`,
      }),
      el("p", { className: "muted small" }, "A level only goes up: the ranks the higher levels granted would stay."),
      el("button", { type: "button", className: "wide", disabled: !gmOn() || (!prog.everyone && !a.debt), onclick: async () => { if (await run(`clear-debt ${who}`)) loadProgress(); } },
        prog.everyone ? "Clear everyone's experience debt" : `Clear the experience debt (${num(a.debt)})`));
  } else if (prog.tab === "weapons") {
    const w = d.weapons.find((x) => x.name === prog.selected.weapons);
    if (!w) { card.replaceChildren(el("p", { className: "muted" }, "No weapon lines.")); return; }
    parts.push(el("h3", {}, spaced(w.name), el("span", { className: "muted" }, ` · level ${w.level}`)),
      action("Set the level", { value: w.cap, min: 1, max: w.cap, presets: [5, 10, 15, 20, w.cap], button: (v) => `Set ${spaced(w.name)} to level ${v}`, line: (v) => `weapon-level ${who} ${w.name} ${v}` }));
    if (!prog.everyone) {
      parts.push(action("Add XP", {
        value: 10000, min: 1, presets: [1000, 10000, 100000], button: (v) => `Add ${num(v)} ${spaced(w.name)} XP`,
        line: (v) => `weapon-xp ${who} ${w.name} ${Math.min(w.max || Infinity, w.xp + Math.max(0, Math.round(Number(v) || 0)))}`,
      }));
    }
    parts.push(el("p", { className: "muted small" }, "A level below the ranks bought in the line's tree is refused until they are refunded."));
  } else {
    const p = d.professions.find((x) => x.name === prog.selected[prog.tab]);
    if (!p) { card.replaceChildren(el("p", { className: "muted" }, "No professions.")); return; }
    const target = prog.all ? "*" : p.name;
    const what = prog.all ? "every profession" : spaced(p.name);
    parts.push(el("h3", {}, prog.all ? "All 22 professions" : spaced(p.name), prog.all ? "" : el("span", { className: "muted" }, ` · ${p.tier} ${p.level}`)),
      action("Add XP", { value: 10000, min: 1, presets: [1000, 10000, 100000], button: (v) => `Add ${num(v)} XP to ${what}`, line: (v) => `profession-xp ${who} ${target} ${v}` }),
      action("Set the level (1-50)", {
        value: Math.min(p.level + 1, 50), min: 1, max: 50, presets: [10, 20, 30, 40, 50],
        button: (v) => `Set ${what} to level ${v}`, line: (v) => `profession-level ${who} ${target} ${v}`,
      }),
      el("p", { className: "muted small" }, "A level past the certification's cap raises the certification to fit."),
      upgrades(p, who, target, what),
      action("Certification", { select: TIERS, value: String(p.tierno), button: () => `Set ${what}'s certification`, line: (v) => `profession-tier ${who} ${target} ${v}` }),
      toggle("All 22 professions at once", "all"));
  }
  parts.push(el("h4", {}, "Who"), toggle("Everyone online, not only " + d.name, "everyone"));
  if (!gmOn()) parts.push(el("p", { className: "muted small" }, gmOffText()));
  card.replaceChildren(...parts);
}

function setupProgression() {
  picker($("prog-player"), (q) => sources.player(q, false));
  $("prog-form").addEventListener("submit", (event) => { event.preventDefault(); loadProgress(); });
  $("prog-player").addEventListener("picked", loadProgress);
  $("prog-heal").addEventListener("click", () => run(`heal ${argument(progPlayer())}`));
  renderProgress();
}

document.addEventListener("DOMContentLoaded", setupProgression);
