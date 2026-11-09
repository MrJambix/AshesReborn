// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
// GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
// ============================================================================
//  session/player_emotes.cpp
//
//  The emote commands: /sit, /wave and the rest of the game's emote list (step 5 of the social track).
//
//  The client sends ServerPlayEmote(EmoteName) with the typed word and no slash.  The word is looked up in the game's list (content/emote_commands.h) without regard to case:
//    - a word that is not on the list is refused, the refusal is logged and the client is sent nothing;
//    - /sit is the Rest (General_Rest, a toggle).  The first one sits the character down through the ability runtime (session/abilities.h): the ability item with the Rest montage and, two
//      seconds on, the Resting effect.  A second /sit, while the Rest runs, ends it and plays the montage's end section (SitEnd).  In combat, on a mount or dead the character does not sit: the
//      refusal is logged.  A Rest that runs can always be ended;
//    - every other word plays its ability through the generic ability runtime (session/ability_pipeline.h), which applies the record's cooldown and cost (Emote_Wave: a one-second
//      cooldown and one charge, no Mana or Stamina).  An ability the content tables do not hold is logged and nothing is played.
//  A refusal sends the client nothing: it starts no instance of its own for an emote, so there is none for a ClientEndAbility to end (TriggerInfo::by_server).
//  Calls over the budget (kEmoteBurst, then one every kEmoteInterval) are dropped and counted.
//
//  The other players see the emotes: the pawn they are shown (session/player_presence.cpp) carries the owner's ReplicatedActiveAbilities, so an emote's ability item reaches every view that
//  holds the pawn as any ability's does, and its ReplicatedRecordsToLoad names the emote abilities the owner's component loads, which the other clients need to play them.
// ============================================================================
#include "session/player_session.h"

#include "bootstrap/constants.h"
#include "content/emote_commands.h"
#include "protocol/repl/rpc_fields.h"

#include <spdlog/spdlog.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace aoc { namespace worldd {

namespace repl = ::aoc::protocol::repl;

namespace {

/// The word as the log shows it: printable ASCII only (anything else is a question mark) and at most 40 characters, so that a hostile client's word is not copied whole into the log.
std::string shown(const std::string& word) {
    constexpr size_t kMaxShown = 40;
    std::string out;
    for (size_t i = 0; i < word.size() && i < kMaxShown; ++i) {
        const unsigned char c = static_cast<unsigned char>(word[i]);
        out += (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
    }
    if (word.size() > kMaxShown) out += "...";
    return out;
}

}  // namespace

void PlayerSession::emote_refusal(const std::string& word, const char* why) {
    ++emotes_refused_;
    spdlog::log(level_for(emote_refusals_logged_), "[emote] {}: \"{}\" is refused: {}", name(), shown(word), why);
}

void PlayerSession::on_play_emote(Connection& conn, const repl::ClientRpc& rpc, Clock::time_point now) {
    if (!within_budget(emote_budget_, "ServerPlayEmote", now)) {
        ++emotes_dropped_;
        return;
    }
    if (!rpc.call.decoded) {
        ++emotes_refused_;
        if (++emote_unreadable_logged_ <= kLoggedPerKind) {
            spdlog::warn("[emote] {}: a ServerPlayEmote does not read ({} parameter bits): ignored", name(), rpc.call.param_bits);
        }
        return;
    }
    const repl::Value* value = repl::rpcf::value_of(*rpc.call.rpc, rpc.call.params, "EmoteName");
    const std::string* text = value ? value->get_if<std::string>() : nullptr;
    const std::string word = text ? *text : std::string();
    const content::EmoteCommand* emote = content::find_emote_command(word);
    if (!emote) {
        ++emotes_refused_;
        spdlog::log(level_for(emote_refusals_logged_), "[emote] {}: \"{}\" is not an emote: nothing is played", name(), shown(word));
        return;
    }
    if (dead_) return emote_refusal(word, "the character is dead");
    if (emote->ability == content::kGuidRest) return emote_sit(conn, word, now);
    emote_play(conn, emote->ability, std::string(emote->name), word, now);
}

void PlayerSession::emote_sit(Connection& conn, const std::string& word, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime) return emote_refusal(word, "abilities are not running for this session");
    cancel_gather(&conn, "a sit was asked for");   // (the same channel cancel an ordinary ability trigger does)
    const double sec = ability_seconds(now);
    if (runtime->resting()) {   // a second /sit: the character stands up
        runtime->end_rest(sec);
        ++emotes_played_;
        spdlog::log(level_for(emote_logged_), "[emote] {}: \"{}\" ends the Rest", name(), shown(word));
        take_ability_events();
        flush_abilities(conn);
        return;
    }
    TriggerInfo ask;
    ask.by_server = true;
    const Refusal ran = runtime->trigger(content::kGuidRest, next_emote_instance(), sec, ask);
    take_ability_events();
    if (ran != Refusal::None) {
        emote_refusal(word, refusal_name(ran));
    } else {
        ++emotes_played_;
        spdlog::log(level_for(emote_logged_), "[emote] {}: \"{}\" sits the character down", name(), shown(word));
    }
    flush_abilities(conn);
}

void PlayerSession::emote_play(Connection& conn, int64_t ability, const std::string& ability_name, const std::string& word, Clock::time_point now) {
    AbilityRuntime* runtime = ability_runtime(now);
    if (!runtime || !pipeline_) return emote_refusal(word, "the generic ability runtime is not running for this session");
    if (!pipeline_->handles(ability)) {
        ++emotes_refused_;
        spdlog::log(level_for(emote_refusals_logged_), "[emote] {}: \"{}\" is the game's emote {}, but the content tables hold no ability for it: nothing is played", name(), shown(word),
                    ability_name);
        return;
    }
    const abilities::AbilityDef* def = world_ && world_->ability_catalog() ? world_->ability_catalog()->ability(ability) : nullptr;
    if (mounted() && def && !def->row.usable_while_mounted) return emote_refusal(word, "the character is on a mount");
    TriggerInfo ask;
    ask.by_server = true;
    const Refusal ran = pipeline_->trigger(ability, next_emote_instance(), ability_seconds(now), ask);
    if (ran == Refusal::None) {
        ++emotes_played_;
        spdlog::log(level_for(emote_logged_), "[emote] {}: \"{}\" plays {}", name(), shown(word), ability_name);
    } else {
        ++emotes_refused_;   // (the pipeline's refusal event says why: take_ability_events logs it)
    }
    take_ability_events();
    flush_abilities(conn);
}

/// The client call of the emote feature: ServerPlayEmote, a call of the PlayerController (session/rpc_table.h).
void PlayerSession::register_emotes_rpcs(RpcTable& t) {
    t.add(RpcScope::PlayerController, "ServerPlayEmote", [](PlayerSession& s, const RpcContext& c) { s.on_play_emote(c.conn, c.rpc, c.now); });
}

}}  // namespace aoc::worldd
