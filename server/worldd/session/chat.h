// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace aoc::worldd::chat {

// EChatCategory values from layouts/aoc.usmap. Proximity ranges are server policy,
// in Unreal centimetres; they have not been measured against a retail capture.
inline constexpr uint8_t kSay = 9, kShout = 10, kEmote = 11, kYell = 12;
inline constexpr uint8_t kTargeted = 43, kTell = 45, kGlobal = 50, kReply = 145;
inline constexpr size_t kMaxInbox = 64;

struct Message {
    std::string text, from;
    uint8_t category = kSay;
};

struct Peer {
    std::string name, last_tell;
    double x = 0, y = 0, z = 0;
    std::deque<Message> inbox;
};

inline bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    auto lower = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
    for (size_t i = 0; i < a.size(); ++i) if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

// The client limit is 256 UTF-16 code units (DefaultAoCRecordConstants.ini).
// Validate UTF-8 before it is passed to the FString encoder. Keep player text on
// one line; ordinary item-link markup remains text and grants no GM decoration.
inline bool valid_text(const std::string& text) {
    if (text.empty() || text.size() > 1024) return false;
    size_t units = 0;
    bool visible = false;
    for (size_t i = 0; i < text.size();) {
        const unsigned char lead = text[i++];
        uint32_t cp = lead;
        int extra = 0;
        if (lead >= 0xc2 && lead <= 0xdf) { cp &= 0x1f; extra = 1; }
        else if (lead >= 0xe0 && lead <= 0xef) { cp &= 0x0f; extra = 2; }
        else if (lead >= 0xf0 && lead <= 0xf4) { cp &= 7; extra = 3; }
        else if (lead >= 0x80) return false;
        if (i + extra > text.size()) return false;
        for (int j = 0; j < extra; ++j) {
            const unsigned char c = text[i++];
            if ((c & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3f);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || cp < 0x20 || cp == 0x7f ||
            cp == 0x85 || cp == 0x2028 || cp == 0x2029) return false;
        units += cp > 0xffff ? 2 : 1;
        if (units > 256) return false;
        visible = visible || (cp != ' ' && cp != 0xa0);
    }
    return visible;
}

// Accessed on worldd's endpoint thread. Weak peers prevent a disconnected session
// from remaining a recipient, including a failed join or a destroyed test session.
class Room {
public:
    std::shared_ptr<Peer> connect(const std::string& name) {
        auto peer = std::make_shared<Peer>();
        peer->name = name;
        live_peers();
        peers_.push_back(peer);
        return peer;
    }

    /// Whom a character ignores, for as long as worldd runs: chat from them does not reach them.  Saved nowhere yet.  A name is kept as the character online spells it (else as typed).
    std::string ignore(const Peer& who, const std::string& name) {
        if (!valid_text(name) || same_name(who.name, name)) return "You cannot ignore that name.";
        auto& list = ignored_[lowered(who.name)];
        if (list.size() >= kMaxIgnored && !list.count(lowered(name))) return "Your ignore list is full.";
        std::string shown = name;
        for (const auto& peer : live_peers()) {
            if (same_name(peer->name, name)) shown = peer->name;
        }
        list[lowered(name)] = shown;
        return {};
    }
    bool unignore(const Peer& who, const std::string& name) { return ignored_[lowered(who.name)].erase(lowered(name)) > 0; }
    /// The names `who` ignores, as shown (in the order of their lower-case spelling).
    std::vector<std::string> ignored_by(const Peer& who) {
        std::vector<std::string> out;
        for (const auto& [key, shown] : ignored_[lowered(who.name)]) out.push_back(shown);
        return out;
    }
    bool ignores(const std::string& who, const std::string& name) const {
        const auto it = ignored_.find(lowered(who));
        return it != ignored_.end() && it->second.count(lowered(name)) > 0;
    }

    // Empty means accepted. A refusal is shown only to the sender.
    std::string send(Peer& sender, const std::string& text, uint8_t category, std::string target) {
        if (!valid_text(text)) return "Chat messages must contain 1 to 256 characters on one line.";
        if (sender.inbox.size() >= kMaxInbox) return "Chat is busy. Please try again.";
        auto peers = live_peers();
        const bool whisper = category == kTell || category == kTargeted || category == kReply;
        if (whisper) {
            if (target.empty() && category == kReply) target = sender.last_tell;
            std::shared_ptr<Peer> recipient;
            for (const auto& peer : peers) if (same_name(peer->name, target)) { recipient = peer; break; }
            if (!recipient) return "That player is not online.";
            if (ignores(recipient->name, sender.name)) return recipient->name + " is not accepting your messages.";
            if (recipient->inbox.size() >= kMaxInbox || sender.inbox.size() >= kMaxInbox) return "Chat is busy. Please try again.";
            recipient->inbox.push_back({text, sender.name, kTell});
            if (recipient.get() != &sender) sender.inbox.push_back({text, recipient->name, kTell});
            recipient->last_tell = sender.name;
            sender.last_tell = recipient->name;
            return {};
        }
        double range = 0;
        switch (category) {
            case kSay: case kEmote: range = 10000; break;
            case kShout: case kYell: range = 30000; break;
            case kGlobal: case 51: case 52: case 54: case 55: break; // OOC, auction, recruiting, trade
            default: return "This chat channel is not available yet. Use /say or /global.";
        }
        for (const auto& peer : peers) {
            if (range > 0 && std::hypot(std::hypot(peer->x - sender.x, peer->y - sender.y), peer->z - sender.z) > range) continue;
            if (ignores(peer->name, sender.name)) continue;
            if (peer->inbox.size() < kMaxInbox) peer->inbox.push_back({text, sender.name, category});
        }
        return {};
    }

private:
    static constexpr size_t kMaxIgnored = 200;
    static std::string lowered(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c); });
        return s;
    }
    std::map<std::string, std::map<std::string, std::string>> ignored_;   // who (lower case) -> name (lower case) -> name as shown
    std::vector<std::shared_ptr<Peer>> live_peers() {
        std::vector<std::shared_ptr<Peer>> live;
        for (auto it = peers_.begin(); it != peers_.end();) {
            if (auto peer = it->lock()) { live.push_back(std::move(peer)); ++it; }
            else it = peers_.erase(it);
        }
        return live;
    }
    std::vector<std::weak_ptr<Peer>> peers_;
};

} // namespace aoc::worldd::chat
