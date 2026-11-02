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
    const auto target = [&] { return iequal(arg(1), "self") || arg(1) == "-" ? invoker.name() : arg(1); };
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
        else if (verb == "money") accept(give_money(target(), integer(2), by));
        else if (verb == "item") {
            const auto amount = integer(3);
            if (amount < 1 || amount > 100000) throw std::invalid_argument("count must be 1 to 100000");   // what give_item takes
            accept(give_item(target(), arg(2), static_cast<int32_t>(amount), by, arg(4)));
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
    out.character = invoker.name(); out.level = invoker.gm_level();
    out.enabled = invoker.gm_enabled(); out.panel_revision = invoker.gm_panel_revision();
    if (out.message.empty()) out.message = "No entries.";
    spdlog::info("[gm] {}: {}: {}", by, verb, out.status == GmStatus::Ok ? "completed" : "refused");
    return out;
}
