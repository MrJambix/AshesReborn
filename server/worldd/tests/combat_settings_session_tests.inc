// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Cyb and contributors.

static void test_auto_attack_settings_persist() {
    std::printf("auto attack options save and survive login synchronization in a fresh session\n");
    struct Sink : PositionSink {
        std::map<std::string, bool> saved;
        int writes = 0;
        void save(const std::string&, const svc::Position&) override {}
        void save_progress(const std::string&, svc::Progress p) override {
            for (const auto& [key, value] : p.auto_attack_settings) saved[key] = value;
            ++writes;
        }
    } sink;
    const auto& cls = class_named("AoCPlayerControllerBP_C");
    auto send_options = [&](Harness& h, const std::map<std::string, bool>& options) {
        repl::RpcParams params;
        const schema::RpcSchema& rpc = *cls.rpc("ServerUpdateAccountCombatSettings");
        for (size_t i = 0; i < rpc.commands.size(); ++i) {   // the client sends the struct whole
            using schema::PropType;
            switch (rpc.commands[i].type) {
                case PropType::Bool: params.values[static_cast<uint16_t>(i + 1)] = repl::Value::boolean(false); break;
                case PropType::Float: params.values[static_cast<uint16_t>(i + 1)] = repl::Value::f32(0.0f); break;
                case PropType::Double: params.values[static_cast<uint16_t>(i + 1)] = repl::Value::f64(0.0); break;
                case PropType::Int8: case PropType::Int16: case PropType::Int32: case PropType::Int64: params.values[static_cast<uint16_t>(i + 1)] = repl::Value::i64(0); break;
                case PropType::UInt8: case PropType::UInt16: case PropType::UInt32: case PropType::UInt64: case PropType::Enum: params.values[static_cast<uint16_t>(i + 1)] = repl::Value::u64(0); break;
                default: break;   // (a kind this struct does not hold: the write below says so)
            }
        }
        for (const auto path : kAutoAttackPaths) {
            const std::string key(path.substr(path.rfind('.') + 1));
            const auto it = options.find(key);
            CHECK(repl::set_rpc_param(*cls.rpc("ServerUpdateAccountCombatSettings"), params,
                  path.substr(std::string_view("CharacterInGameSettings.").size()), repl::Value::boolean(it != options.end() && it->second)), "the auto attack option has a typed RPC parameter");
        }
        h.client_sends({h.client.actor(h.s->channel_of(bs::Role::PlayerController), client_rpc(cls, "ServerUpdateAccountCombatSettings", params), true)});
        h.settle();
    };
    std::map<std::string, bool> enabled, disabled;
    for (size_t i = 0; i < kAutoAttackPaths.size(); ++i) {
        const auto path = kAutoAttackPaths[i];
        const std::string key(path.substr(path.rfind('.') + 1));
        enabled[key] = i % 2 == 0;
        disabled[key] = false;
    }
    Harness first;
    first.fx.world->set_position_sink(&sink);
    CHECK(first.join(), "the first session joins");
    first.settle();
    send_options(first, disabled); // the automatic login request
    send_options(first, enabled);  // the player applies the options
    CHECK(sink.saved == enabled, "all five auto attack choices are handed to durable storage immediately");

    svc::Character record = cyb_record();
    record.auto_attack_settings = sink.saved;
    Sink after_login;
    after_login.saved = record.auto_attack_settings;   // the store holds them: a save names only what changed
    Harness fresh;
    fresh.fx.world->set_position_sink(&after_login);
    CHECK(fresh.join(record), "a fresh session joins with the saved account settings");
    fresh.settle();
    const size_t before = fresh.got.size();
    send_options(fresh, disabled); // a relaunched client initially sends its old defaults
    CHECK(after_login.writes == 0, "login defaults do not overwrite the saved options");
    bool restored = false;
    for (const auto* packet : fresh.on_channel(fresh.s->channel_of(bs::Role::PlayerController), before)) {
        const auto blocks = repl::read_content_blocks(packet->content.data(), packet->content.bit_pos(), 0);
        if (!blocks) continue;
        for (const auto& block : *blocks) {
            size_t pos = block.payload_pos;
            const auto state = repl::read_rep_block(cls, packet->content.data(), block.payload_pos + block.payload_bits, pos);
            if (!state) continue;
            bool all = packet->reliable;
            for (const auto path : kAutoAttackPaths) {
                const auto* value = state->get(cls, path);
                const auto* flag = value ? value->get_if<bool>() : nullptr;
                const std::string key(path.substr(path.rfind('.') + 1));
                all = all && flag && *flag == enabled[key];
            }
            restored = restored || all;
        }
    }
    CHECK(restored, "a reliable property block restores all five saved values, including false");
    send_options(fresh, disabled);
    CHECK(after_login.saved == disabled, "the player's later change can disable and save every option");
    record.auto_attack_settings = after_login.saved;
    Harness relaunched;
    CHECK(relaunched.join(record), "another new session reads the disabled choices");
    relaunched.settle();
    send_options(relaunched, enabled);
    const auto& pc = relaunched.s->bootstrap().player_controller();
    bool all_disabled = true;
    for (const auto path : kAutoAttackPaths) {
        const auto* value = pc.obj.state.get(*pc.obj.schema, path);
        const auto* flag = value ? value->get_if<bool>() : nullptr;
        all_disabled = all_disabled && flag && !*flag;
    }
    CHECK(all_disabled, "explicitly disabled options also survive a new login");
}
AOC_REGISTER_TEST(test_auto_attack_settings_persist);
