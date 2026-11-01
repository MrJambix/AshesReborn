// SPDX-License-Identifier: AGPL-3.0-only
// Uses the fake client/connection Harness from test_worldd_session.cpp.

static repl::RpcParams chat_params(const schema::RpcSchema& rpc, const std::string& text, uint8_t category,
                                   const std::string& target = "") {
    repl::RpcParams params;
    for (const auto& command : rpc.commands) {
        repl::Value v;
        switch (command.type) {
            case schema::PropType::Bool: v = repl::Value::boolean(false); break;
            case schema::PropType::Str: v = repl::Value::str(""); break;
            case schema::PropType::Name: v = repl::Value(repl::NameValue{}); break;
            case schema::PropType::Text: v = repl::Value(repl::TextValue{}); break;
            case schema::PropType::Enum: v = repl::Value::u64(0); break;
            case schema::PropType::Float: v = repl::Value::f32(0); break;
            case schema::PropType::NetSerializeStruct: v = repl::Value(repl::ItemInstance{}); break;
            default: std::exit(2);
        }
        params.values[command.handle] = std::move(v);
    }
    repl::set_rpc_param(rpc, params, "ChatData.Quote", repl::Value::str(text));
    repl::set_rpc_param(rpc, params, "ChatData.ChatCategory", repl::Value::u64(category));
    repl::set_rpc_param(rpc, params, "ChatData.TargetName", repl::Value::str(target));
    // Attempts to spoof a GM and another sender must not survive the server.
    repl::set_rpc_param(rpc, params, "ChatData.SubjectName", repl::Value::str("FakeGM"));
    repl::set_rpc_param(rpc, params, "ChatData.bSentFromGM", repl::Value::boolean(true));
    repl::set_rpc_param(rpc, params, "ChatData.EmblemDecorators", repl::Value::str("GM_ICON"));
    repl::set_rpc_param(rpc, params, "ChatData.LocationX", repl::Value::f32(1e9f));
    return params;
}

static std::vector<repl::ElementState> received_chat(const Harness& h, size_t first) {
    std::vector<repl::ElementState> messages;
    const auto& cls = class_named("AoCPlayerControllerBP_C");
    for (const auto* delivered : h.on_channel(h.s->channel_of(bs::Role::PlayerController), first)) {
        const auto blocks = repl::read_content_blocks(delivered->content.data(), delivered->content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& block : *blocks) {
            if (block.header.has_rep_layout) continue;
            size_t pos = block.payload_pos;
            const auto calls = repl::read_rpc_fields(cls, delivered->content.data(), pos + block.payload_bits, pos);
            if (!calls) continue;
            for (const auto& call : *calls) {
                if (std::string_view(call.rpc->name) != "ClientReceiveChatMessages" || !call.decoded) continue;
                const auto it = call.params.values.find(1);
                const auto* list = it == call.params.values.end() ? nullptr : it->second.get_if<repl::ArrayValue>();
                if (list) for (const auto& message : list->elements) messages.push_back(message);
                CHECK(delivered->reliable, "chat is delivered reliably");
            }
        }
    }
    return messages;
}

static void test_chat() {
    std::printf("player chat through client RPCs\n");
    Harness a, b, outsider;
    CHECK(a.join() && b.join(second_record(), a.fx.world.get()) && outsider.join(), "two chat peers share a world; another world stays separate");
    if (!a.s || !b.s || !outsider.s) return;
    a.settle(); b.settle(); outsider.settle();
    const auto& cls = class_named("AoCPlayerControllerBP_C");
    const auto* rpc = cls.rpc("SendChatMessage");
    CHECK(rpc != nullptr, "shipping client chat RPC exists");
    if (!rpc) return;
    auto send = [&](const std::string& text, uint8_t category, const std::string& target = "") {
        a.t += seconds(4); // replenish the real session's budget
        a.client_sends({a.client.actor(a.s->channel_of(bs::Role::PlayerController),
                         client_rpc(cls, "SendChatMessage", chat_params(*rpc, text, category, target)), true)});
        b.s->tick(b.conn, b.t); // exercise the timed tick used by the live endpoint
        a.settle(); b.settle(); outsider.settle();
    };
    size_t ai = a.got.size(), bi = b.got.size(), oi = outsider.got.size();
    send("Hello Verra", chat::kSay);
    auto am = received_chat(a, ai), bm = received_chat(b, bi);
    CHECK(am.size() == 1 && bm.size() == 1, "say reaches the sender and a nearby player");
    CHECK(received_chat(outsider, oi).empty(), "chat never crosses world instances");
    if (bm.size() == 1) {
        const auto& v = bm[0].values;
        CHECK(v.at(1) == repl::Value::str("Hello Verra") && v.at(2) == repl::Value::str("cyb") &&
              v.at(3) == repl::Value::u64(chat::kSay), "wire message retains text and channel with the authenticated character name");
        CHECK(v.at(4) == repl::Value::str("") && v.at(5) == repl::Value::boolean(false) &&
              v.at(6) == repl::Value(repl::ItemInstance{}), "client cannot forge GM status, decorations or associated items");
    }
    bi = b.got.size(); ai = a.got.size();
    send("Private hello", chat::kTell, "DEE");
    CHECK(received_chat(b, bi).size() == 1 && received_chat(a, ai).size() == 1, "tell finds an online name without case sensitivity and echoes to the sender");
    bi = b.got.size(); ai = a.got.size();
    send("Missing recipient", chat::kTell, "offline");
    am = received_chat(a, ai);
    CHECK(received_chat(b, bi).empty() && am.size() == 1 && am[0].values.at(3) == repl::Value::u64(4) &&
          am[0].values.at(2) == repl::Value::str(""), "offline whispers produce a private system status without leaking to other players");
    bi = b.got.size();
    send("guild secret", 31);
    CHECK(received_chat(b, bi).empty(), "unimplemented guild channel is never broadcast");
    bi = b.got.size();
    send(std::string(257, 'x'), chat::kGlobal);
    CHECK(received_chat(b, bi).empty(), "oversize input is not delivered");
    bi = b.got.size();
    send("\xc3\xa9 hello \xf0\x9f\x8c\x8d", chat::kGlobal);
    bm = received_chat(b, bi);
    CHECK(bm.size() == 1 && bm[0].values.at(1) == repl::Value::str("\xc3\xa9 hello \xf0\x9f\x8c\x8d"), "Unicode survives the client's FString round trip");
    bi = b.got.size();
    a.t += seconds(4);
    const auto many = client_rpc(cls, "SendChatMessage", chat_params(*rpc, "budget", chat::kGlobal));
    a.client_repeats(a.s->channel_of(bs::Role::PlayerController), many, 12);
    a.settle(); b.settle();
    CHECK(received_chat(b, bi).size() == 4, "chat burst is bounded to four messages before tokens replenish");

    chat::Room room;
    auto near = room.connect("near"), far = room.connect("far"), third = room.connect("third");
    far->x = 10001;
    room.send(*near, "range", chat::kSay, "");
    CHECK(near->inbox.size() == 1 && far->inbox.empty(), "say enforces the 100 metre server range");
    room.send(*near, "global", chat::kGlobal, "");
    CHECK(far->inbox.size() == 1, "global reaches players outside say range");
    third->inbox.clear(); far->inbox.clear();
    room.send(*near, "secret", chat::kTell, "far");
    CHECK(third->inbox.empty() && far->inbox.size() == 1, "whispers exclude a third player in the same world");
    room.send(*far, "reply", chat::kReply, "");
    CHECK(near->inbox.back().text == "reply", "reply resolves the last whisper counterpart");
    far.reset();
    CHECK(!room.send(*near, "gone", chat::kTell, "far").empty(), "destroyed peers are no longer online");
    for (int i = 0; i < 200; ++i) {
        near->inbox.clear(); // the sender reads normally while the third player stalls
        room.send(*near, "bounded", chat::kGlobal, "");
    }
    CHECK(third->inbox.size() == chat::kMaxInbox, "slow recipients have a bounded inbox");
    CHECK(!chat::valid_text("   ") && !chat::valid_text("line\nbreak") && !chat::valid_text(std::string("nul\0x", 5)) &&
          !chat::valid_text("\xc0\x80") && !chat::valid_text("\xed\xa0\x80"), "blank, multiline, NUL and invalid UTF-8 input is rejected");
    CHECK(chat::valid_text(std::string(256, 'x')), "256 ASCII characters are allowed");
    std::string unicode;
    for (int i = 0; i < 128; ++i) unicode += "\xf0\x9f\x8c\x8d";
    CHECK(chat::valid_text(unicode) && !chat::valid_text(unicode + "x"), "supplementary Unicode counts as two UTF-16 units");
    b.s->finish("chat test logout");
    ai = a.got.size();
    send("logged out", chat::kTell, "dee");
    am = received_chat(a, ai);
    CHECK(am.size() == 1 && am[0].values.at(3) == repl::Value::u64(4), "logout immediately removes the recipient from chat");
}
