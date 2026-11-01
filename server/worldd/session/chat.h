// SPDX-License-Identifier: AGPL-3.0-only
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
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
            if (peer->inbox.size() < kMaxInbox) peer->inbox.push_back({text, sender.name, category});
        }
        return {};
    }

private:
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
