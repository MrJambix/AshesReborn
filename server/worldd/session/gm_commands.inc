// Included inside aoc::worldd by gm_travel.cpp. All calls run on the endpoint thread.
const std::vector<GmCommandSpec>& gm_command_specs() {
    static const std::vector<GmCommandSpec> specs = {
        {"on", "Session", ""}, {"off", "Session", ""}, {"ui", "Session", ""}, {"help", "Session", ""},
        {"players", "Travel", ""}, {"places", "Travel", ""},
        {"teleport", "Travel", "character place"}, {"teleport-xyz", "Travel", "character x y z?"},
        {"speed", "Travel", "character multiplier"},
        {"money", "Inventory", "character copper"}, {"item", "Inventory", "character item count rarity?"},
        {"finish-jobs", "Crafting", "character station?"},
        {"teach-recipe", "Crafting", "character recipe"}, {"forget-recipe", "Crafting", "character recipe"}, {"recipes", "Crafting", "character"},
        {"place-station", "Crafting", "node plot station"}, {"remove-station", "Crafting", "node plot"},
        {"progress", "Character", "character"}, {"heal", "Character", "character"},
        {"xp", "Progression", "character xp"}, {"level", "Progression", "character level"}, {"clear-debt", "Progression", "character"},
        {"profession-xp", "Progression", "character profession xp"}, {"profession-level", "Progression", "character profession level"},
        {"profession-tier", "Progression", "character profession tier"},
        {"reset-tutorials", "Progression", "character"},
        {"weapon-level", "Progression", "character line level"}, {"weapon-xp", "Progression", "character line xp"},
        {"node-level", "Nodes", "node level"}, {"node-xp", "Nodes", "node xp"}, {"node-info", "Nodes", "node"},
        {"clock", "World", ""}, {"advance-clock", "World", "seconds"},
        {"quest-list", "Quests", "character"}, {"quest-give", "Quests", "character quest"},
        {"quest-finish", "Quests", "character quest"}, {"quest-fail", "Quests", "character quest"},
        {"quest-abandon", "Quests", "character quest"}, {"quest-clear", "Quests", "character quest?"},
        {"quest-variable", "Quests", "character variable value"},
        {"npc-kill", "NPCs", "actor"}, {"npc-heal", "NPCs", "actor"}, {"npc-reset", "NPCs", "actor"},
        {"npc-respawn", "NPCs", "actor"}, {"npc-despawn", "NPCs", "actor"}, {"npc-move", "NPCs", "actor x y z?"}
    };
    return specs;
}

GmCommandResult GmTravel::account_command(int64_t account_id, const std::string& text) {
    GmCommandResult out;
    PlayerHandle owner;
    for (const auto& h : directory_()) {
        if (h && h.player->account_id() == account_id) {
            if (owner) { out.status = GmStatus::FailedPrecondition; out.message = "More than one character is online for this account."; return out; }
            owner = h;
        }
    }
    if (!owner) { out.status = GmStatus::NotFound; out.message = "Enter the world with your GM account first."; return out; }
    if (!text.empty()) return command(*owner.player, *owner.connection, text);
    if (owner.player->gm_level() != 3) { out.status = GmStatus::PermissionDenied; out.message = "This account is not a GM."; return out; }
    out.character = owner.player->name(); out.level = owner.player->gm_level();
    out.enabled = owner.player->gm_enabled(); out.panel_revision = owner.player->gm_panel_revision();
    return out;
}

GmCommandResult GmTravel::command(PlayerSession& invoker, Connection& conn, const std::string& text) {
    GmCommandResult out;
    const auto fail = [&](GmStatus status, const std::string& message) { out.status = status; out.message = message; return out; };
    if (invoker.gm_level() != 3) return fail(GmStatus::PermissionDenied, "This account is not a GM.");
    if (text.size() > 1024 || text.find_first_of("\r\n\t") != std::string::npos) return fail(GmStatus::InvalidArgument, "Invalid GM command.");
    std::istringstream input(text);
    std::vector<std::string> words;
    for (;;) {
        input >> std::ws;
        if (input.eof()) break;
        std::string word;
        if (!(input >> std::quoted(word))) return fail(GmStatus::InvalidArgument, "Invalid quoted argument. Use .GM help.");
        words.push_back(std::move(word));
    }
    if (words.empty()) return fail(GmStatus::InvalidArgument, "Use .GM help.");
    if (iequal(words[0], ".gm")) words.erase(words.begin());
    if (words.empty()) words.push_back("ui");
    std::string verb = words.front();
    for (char& c : verb) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const GmCommandSpec* spec = nullptr;
    for (const auto& s : gm_command_specs()) if (s.name == verb) { spec = &s; break; }
    if (!spec) return fail(GmStatus::InvalidArgument, "Unknown GM command. Use .GM help.");
    std::istringstream params(spec->arguments);
    size_t required = 0, maximum = 0;
    for (std::string p; params >> p;) { ++maximum; if (p.back() != '?') ++required; }
    const size_t count = words.size() - 1;
    if (count < required || count > maximum) return fail(GmStatus::InvalidArgument, "Usage: .GM " + spec->name + " " + spec->arguments);
    if (verb != "on" && verb != "off" && verb != "help" && !invoker.gm_enabled())
        return fail(GmStatus::PermissionDenied, "GM mode is off. Use .GM On first.");
    if (verb != "off" && verb != "help" && !invoker.gm_rpc_admitted_for_panel(Clock::now()))
        return fail(GmStatus::FailedPrecondition, "GM commands are busy. Try again shortly.");
    const auto arg = [&](size_t n) { return n < words.size() ? words[n] : std::string(); };
    // the character a command is for: "self" (or "-") is the GM's own; "*" is every player online, for the commands that give or change one player's things
    std::string who = iequal(arg(1), "self") || arg(1) == "-" ? invoker.name() : arg(1);
    const auto target = [&] { return who; };
    const auto integer = [&](size_t n) -> int64_t {
        const std::string s = arg(n); size_t used = 0;
        const auto value = std::stoll(s, &used);
        if (used != s.size()) throw std::invalid_argument("integer");
        return value;
    };
    const auto number = [&](size_t n) {
        const std::string s = arg(n); size_t used = 0; const double value = std::stod(s, &used);
        if (used != s.size() || !std::isfinite(value)) throw std::invalid_argument("number");
        return value;
    };
    const auto accept = [&](const auto& r) { out.status = r.status; out.message = r.message.empty() ? "Command completed." : r.message; };
    const std::string by = "GM account " + std::to_string(invoker.account_id()) + " (" + invoker.name() + ")";
    const auto online = [&]() -> PlayerHandle {
        const PlayerHandle h = find_player(target());
        if (!h) fail(GmStatus::NotFound, "no player named \"" + log_text(target()) + "\" is in the world");
        return h;
    };
    // a profession by its name ("Mining", "lumber milling", "LumberMilling"), or "*" for all 22 (0)
    const auto profession_of = [&](const std::string& text, int64_t& guid) {
        const professions::Book* book = world_.professions();
        if (!book) { fail(GmStatus::FailedPrecondition, "this world has no professions (its content.db lacks them)"); return false; }
        if (text == "*" || iequal(text, "all")) { guid = 0; return true; }
        const auto squeeze = [](const std::string& t) {
            std::string o;
            for (const char c : t) if (c != ' ' && c != '_' && c != '-') o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return o;
        };
        for (const professions::Profession& p : book->all()) {
            if (squeeze(p.name) == squeeze(text)) { guid = p.guid; return true; }
        }
        std::string names;
        for (const professions::Profession& p : book->all()) names += " " + p.name;
        fail(GmStatus::NotFound, "no profession is called \"" + log_text(text) + "\" (* is all of them); the professions are:" + names);
        return false;
    };
    const auto progress_text = [&](const PlayerSession::GmProgressResult& r) {
        std::string t = who + "'s " + r.what;
        if (r.changed > 1) t += " (" + std::to_string(r.changed) + " moved; the first)";
        t += ": level " + std::to_string(r.level_before);
        if (!r.tier_before.empty()) t += " (" + r.tier_before + ")";
        t += " -> " + std::to_string(r.level_after);
        if (!r.tier_after.empty()) t += " (" + r.tier_after + ")";
        t += ", " + std::to_string(r.xp_before) + " -> " + std::to_string(r.xp_after) + " XP";
        if (r.what == "Adventuring" && (r.debt_before != 0 || r.debt_after != 0)) t += ", debt " + std::to_string(r.debt_before) + " -> " + std::to_string(r.debt_after);
        return t + ".";
    };
    const auto dispatch = [&]() -> GmCommandResult {
    try {
        if (verb == "on" || verb == "off" || verb == "ui") {
            invoker.set_gm_enabled(verb != "off");
            invoker.gm_show_tag(conn, verb != "off", Clock::now());
            if (verb == "off" && invoker.gm_speed() != 1.0) invoker.gm_set_speed(conn, 1.0, Clock::now(), by);
            out.message = verb == "off" ? "GM mode disabled." : "GM owner mode enabled. The in-game panel is available in Reborn Launcher; use .GM help for chat commands.";
        } else if (verb == "help") {
            for (const auto& s : gm_command_specs()) out.message += ".GM " + s.name + (s.arguments.empty() ? "" : " " + s.arguments) + "\n";
        } else if (verb == "players") {
            for (const auto& p : list_players()) out.message += p.name + " " + coords(p.position.x, p.position.y) + "\n";
        } else if (verb == "places") {
            for (const auto& p : list_places()) out.message += p.name + "\n";
        } else if (verb == "teleport" || verb == "teleport-xyz") {
            GmTeleportRequest r; r.character = target();
            if (verb == "teleport") r.place = arg(2);
            else { r.x = number(2); r.y = number(3); r.has_z = count == 4; if (r.has_z) r.z = number(4); }
            accept(teleport(r, by));
        } else if (verb == "speed") accept(set_speed(target(), number(2), by));
        else if (verb == "money") {
            const auto r = give_money(target(), integer(2), by);
            accept(r);
            if (r.status == GmStatus::Ok) out.message = "Gave " + std::to_string(r.added) + " copper to " + r.character + " (purse " + std::to_string(r.balance) + " copper).";
        } else if (verb == "item") {
            const auto amount = integer(3);
            if (amount < 1 || amount > 100000) throw std::invalid_argument("count must be 1 to 100000");   // what give_item takes
            // the fourth word is a rarity ("Epic") or an exact quality ("q65" or "65", 1 to 200)
            std::string rarity = arg(4);
            int32_t quality = 0;
            const std::string digits = !rarity.empty() && (rarity[0] == 'q' || rarity[0] == 'Q') ? rarity.substr(1) : rarity;
            if (!digits.empty() && digits.size() <= 3 && std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
                quality = static_cast<int32_t>(std::stoi(digits));
                if (quality < 1) throw std::invalid_argument("quality 1 to 200");
                rarity.clear();
            }
            const auto r = give_item(target(), arg(2), static_cast<int32_t>(amount), by, rarity, quality);
            accept(r);
            if (r.status == GmStatus::Ok)
                out.message = "Gave " + std::to_string(r.count) + " " + r.item + " to " + r.character + " as " + r.rarity + " (quality " + std::to_string(r.quality) + ")" +
                              (r.container.empty() ? "" : ", into " + r.container) + ".";
        }
        else if (verb == "xp" || verb == "level" || verb == "clear-debt") {
            const PlayerHandle h = online();
            if (!h) return out;
            const auto change = verb == "xp" ? PlayerSession::GmAdventuring::AddXp : verb == "level" ? PlayerSession::GmAdventuring::SetLevel : PlayerSession::GmAdventuring::ClearDebt;
            const auto r = h.player->gm_adventuring(*h.connection, change, verb == "clear-debt" ? 0 : integer(2), now_(), by);
            if (!r.ok) return fail(GmStatus::FailedPrecondition, r.refusal);
            out.message = progress_text(r);
        } else if (verb == "profession-xp" || verb == "profession-level" || verb == "profession-tier") {
            const PlayerHandle h = online();
            if (!h) return out;
            int64_t guid = 0;
            if (!profession_of(arg(2), guid)) return out;
            int64_t amount = 0;
            if (verb == "profession-tier") {   // a number 0 to 4 or the certification's name
                const professions::Book& book = *world_.professions();
                amount = -1;
                for (int t = 0; t < professions::kTiers; ++t) {
                    if (iequal(book.tier(t).name, arg(3)) || arg(3) == std::to_string(t)) amount = t;
                }
                if (amount < 0) {
                    std::string names;
                    for (int t = 0; t < professions::kTiers; ++t) names += " " + std::to_string(t) + " " + book.tier(t).name;
                    return fail(GmStatus::InvalidArgument, "no certification is called \"" + log_text(arg(3)) + "\"; they are:" + names);
                }
            } else amount = integer(3);
            const auto change = verb == "profession-xp" ? PlayerSession::GmProfession::AddXp : verb == "profession-level" ? PlayerSession::GmProfession::SetLevel : PlayerSession::GmProfession::SetTier;
            const auto r = h.player->gm_profession(*h.connection, guid, change, amount, now_(), by);
            if (!r.ok) return fail(GmStatus::FailedPrecondition, r.refusal);
            out.message = progress_text(r);
        } else if (verb == "heal") {
            const PlayerHandle h = online();
            if (!h) return out;
            const auto r = h.player->gm_heal(*h.connection, now_(), by);
            if (!r.ok) return fail(GmStatus::FailedPrecondition, r.refusal);
            char buf[160];
            std::snprintf(buf, sizeof buf, ": Health %.0f -> %.0f, Mana %.0f -> %.0f, Stamina full.", r.health_before, r.health, r.mana_before, r.mana);
            out.message = "Healed " + h.player->name() + buf;
        } else if (verb == "progress") {
            // one fact a line, "kind name key=value ...", so that the web page can read it as well as a GM
            const PlayerHandle h = online();
            if (!h) return out;
            const PlayerSession& p = *h.player;
            const leveling::LevelCurve* curve = world_.level_curve();
            const leveling::Progress& a = p.progress();
            std::string m = "character " + p.name() + "\n";
            m += "adventuring level=" + std::to_string(a.level) + " xp=" + std::to_string(a.xp) + " debt=" + std::to_string(a.debt);
            if (curve) {
                m += " cap=" + std::to_string(curve->cap()) + " from=" + std::to_string(curve->xp_for(a.level));
                if (a.level < curve->cap()) m += " next=" + std::to_string(curve->xp_for(a.level + 1));
            }
            m += "\npurse copper=" + std::to_string(p.wallet().copper()) + "\n";
            const professions::Professions& pr = p.professions();
            if (pr.valid()) {
                const professions::Book& book = *pr.book();
                for (const professions::Profession& pf : book.all()) {
                    const professions::State s = pr.state(pf.guid);
                    const professions::Tier& t = book.tier(s.tier);
                    m += "profession " + pf.name + " category=" + pf.category + " tier=" + t.name + " tierno=" + std::to_string(s.tier) + " level=" + std::to_string(s.level) +
                         " cap=" + std::to_string(t.max_level) + " xp=" + std::to_string(s.xp) + " from=" + std::to_string(pf.curve.xp_for(s.level)) +
                         " next=" + std::to_string(pf.curve.xp_for(s.level + 1)) + "\n";
                }
            }
            const weaponxp::State& w = p.weapon_lines();
            if (w.valid()) {
                const weaponxp::Lines& lines = *w.lines();
                for (const weaponxp::Line& l : lines.all()) {
                    const int level = w.level(l.leveling_type);
                    m += "weapon " + l.name + " level=" + std::to_string(level) + " cap=" + std::to_string(lines.max_level()) + " max=" + std::to_string(lines.max_xp()) + " xp=" + std::to_string(w.xp(l.leveling_type)) +
                         " from=" + std::to_string(lines.xp_for(level)) + " next=" + std::to_string(lines.xp_for(std::min(level + 1, lines.max_level()))) + "\n";
                }
            }
            out.message = m;
        } else if (verb == "finish-jobs") accept(finish_jobs(target(), arg(2), by));
        else if (verb == "teach-recipe") accept(teach_recipe(target(), arg(2), by));
        else if (verb == "forget-recipe") accept(forget_recipe(target(), arg(2), by));
        else if (verb == "recipes") {
            const auto r = list_taught(target()); accept(r);
            if (r.status == GmStatus::Ok) { out.message.clear(); for (const auto& p : r.recipes) out.message += p.first + "\n"; }
        } else if (verb == "place-station") accept(place_station(arg(1), arg(2), arg(3), by));
        else if (verb == "remove-station") accept(remove_station(arg(1), arg(2), by));
        else if (verb == "reset-tutorials") accept(reset_tutorials(target(), by));
        else if (verb == "weapon-level" || verb == "weapon-xp") accept(set_weapon_xp(target(), arg(2), verb == "weapon-level", integer(3), by));
        else if (verb.rfind("node-", 0) == 0 || verb == "clock" || verb == "advance-clock") {
            auto* control = world_.node_control();
            if (!control) return fail(GmStatus::FailedPrecondition, "The node service is unavailable.");
            if (verb == "node-level") {
                const auto level = integer(2);
                if (level < 0 || level > 4) throw std::invalid_argument("level 0 to 4");
                control->set_level(arg(1), static_cast<int>(level), false, by);
            } else if (verb == "node-xp") control->add_experience(arg(1), integer(2), by);
            else if (verb == "node-info") control->node_data(arg(1), by);
            else if (verb == "clock") control->clock(by);
            else {
                const auto seconds = integer(1);
                if (seconds <= 0) throw std::invalid_argument("positive seconds");
                control->advance_clock(seconds, by);
            }
            out.message = "Request queued. Node changes will appear in game; diagnostic results are recorded in the server log.";
        } else if (verb.rfind("quest-", 0) == 0) {
            const auto h = find_player(target());
            if (!h) return fail(GmStatus::NotFound, "That character is not online.");
            auto& p = *h.player;
            const auto* book = world_.quests();
            if (!book || !p.quests_on_) return fail(GmStatus::FailedPrecondition, "Quests are unavailable for that character.");
            if (!p.room_for(4)) return fail(GmStatus::FailedPrecondition, "That character's reply queue is busy.");
            if (verb == "quest-list") {
                for (const auto& q : p.quest_log_.instances()) {
                    const auto* def = book->quest(q.record);
                    out.message += (def ? def->name : std::to_string(q.record)) + "\n";
                }
            } else if (verb == "quest-variable") {
                if (arg(2).empty()) throw std::invalid_argument("variable");
                svc::QuestCommand cmd; cmd.kind = "set-var"; cmd.name = arg(2); cmd.value = integer(3);
                p.apply_quest_command(*h.connection, cmd, Clock::now()); out.message = "Quest variable updated.";
            } else {
                const auto* def = book->find(arg(2));
                if ((!arg(2).empty() || verb != "quest-clear") && !def) return fail(GmStatus::NotFound, "Unknown quest record.");
                const std::string kind = verb.substr(6);
                if ((kind == "finish" || kind == "fail" || kind == "abandon") && !p.quest_log_.find(def->guid))
                    return fail(GmStatus::FailedPrecondition, "That quest is not active for this character.");
                if (kind == "give" && p.quest_log_.find(def->guid)) return fail(GmStatus::FailedPrecondition, "That quest is already active.");
                svc::QuestCommand cmd; cmd.kind = kind; cmd.quest = arg(2);
                if (kind == "finish" || kind == "fail" || kind == "abandon") {
                    if (!p.end_quest(*h.connection, def->guid, kind.c_str(), Clock::now()))
                        return fail(GmStatus::FailedPrecondition, "The quest command was refused for the character's current state.");
                } else p.apply_quest_command(*h.connection, cmd, Clock::now());
                if (kind == "give" && !p.quest_log_.find(def->guid)) return fail(GmStatus::FailedPrecondition, "The quest could not be started. Check the character's quest limit and state.");
                out.message = "Quest command applied to " + p.name() + ".";
            }
        }
        else if (verb.rfind("npc-", 0) == 0) {
            const auto id = integer(1);
            if (id <= 0) throw std::invalid_argument("actor");
            if (verb == "npc-kill") accept(kill_npc(id, by));
            else if (verb == "npc-heal") accept(heal_npc(id, by));
            else if (verb == "npc-reset") accept(reset_npc(id, by));
            else if (verb == "npc-respawn") accept(respawn_npc(id, by));
            else if (verb == "npc-despawn") accept(despawn_npc(id, by));
            else accept(move_npc(id, number(2), number(3), count == 4 ? number(4) : 0, count == 4, by));
        }
    } catch (const std::exception&) { return fail(GmStatus::InvalidArgument, "Invalid argument. Usage: .GM " + spec->name + " " + spec->arguments); }
    return out;
    };
    static const std::vector<std::string> kEveryone = {"money", "item", "xp", "level", "clear-debt", "heal", "profession-xp", "profession-level", "profession-tier",
                                                       "weapon-level", "weapon-xp", "teach-recipe"};
    if (arg(1) == "*" && std::find(kEveryone.begin(), kEveryone.end(), verb) != kEveryone.end()) {
        // every player online, one after another; the answer has a line each
        std::vector<std::string> names;
        for (const auto& h : directory_()) if (h) names.push_back(h.player->name());
        if (names.empty()) return fail(GmStatus::NotFound, "Nobody is in the world.");
        size_t done = 0;
        GmStatus last = GmStatus::Ok;
        std::string lines;
        for (const std::string& name : names) {
            who = name;
            out = GmCommandResult();
            const GmCommandResult r = dispatch();
            if (r.status == GmStatus::Ok) ++done;
            else last = r.status;
            lines += name + ": " + (r.status == GmStatus::Ok ? "" : "refused: ") + (r.message.empty() ? "Command completed." : r.message) + "\n";
        }
        out = GmCommandResult();
        out.status = done > 0 ? GmStatus::Ok : last;
        out.message = std::to_string(done) + " of " + std::to_string(names.size()) + " players done:\n" + lines;
    } else {
        out = dispatch();
    }
    out.character = invoker.name(); out.level = invoker.gm_level();
    out.enabled = invoker.gm_enabled(); out.panel_revision = invoker.gm_panel_revision();
    if (out.message.empty()) out.message = "No entries.";
    spdlog::info("[gm] {}: {}: {}", by, verb, out.status == GmStatus::Ok ? "completed" : "refused");
    return out;
}
