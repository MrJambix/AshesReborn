// SPDX-License-Identifier: AGPL-3.0-only
// Exercise the real player tether over UDP, including the game's token handoff.
#include "services/tether/tether_service.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

static void require(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
    std::printf("ok: %s\n", description);
}

int main() {
    namespace wire = tether::wire;
    namespace fs = std::filesystem;
    const fs::path folder = fs::current_path() / "logs" / ("test-player-" + std::to_string(GetCurrentProcessId()));
    const std::string token_file = (folder / "ics_auth_token.txt").string();
    SOCKET client = INVALID_SOCKET;
    int result = 0;
    try {
        require(aoc::ics_token::write(token_file, "test-only-first-token"), "test sign-in is written");
        TetherServiceImpl tether;
        tether.set_ics_endpoint("gateway.example.com", 8443);
        tether.set_token_file(token_file);
        require(tether.start(0, true), "player tether starts on loopback");
        TetherServiceImpl competing;
        require(!competing.start(tether.port(), true), "another launcher cannot share the player's port");

        client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        require(client != INVALID_SOCKET, "game test socket opens");
        DWORD timeout = 2000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(tether.port());
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        auto send = [&](wire::MsgId kind, const std::vector<uint8_t>& body, int sequence) {
            wire::Header header{};
            header.magic_number = (wire::APP_NUMBER << 16) | (2 << 8) | static_cast<uint8_t>(kind);
            header.session_id = 123;
            header.window_size = 64;
            header.sequence_number = sequence;
            header.data_length = static_cast<uint16_t>(body.size());
            auto bytes = wire::encode(header);
            bytes.insert(bytes.end(), body.begin(), body.end());
            require(sendto(client, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                           reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == static_cast<int>(bytes.size()),
                    "game packet sent");
        };
        auto receive = [&] {
            std::vector<uint8_t> bytes(65535);
            const int n = recv(client, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
            require(n >= static_cast<int>(wire::HEADER_SIZE), "tether answers before the deadline");
            bytes.resize(n);
            return bytes;
        };
        send(wire::MsgId::connect, {}, 0);
        auto connected = receive();
        wire::Header header{};
        require(wire::decode(connected.data(), connected.size(), header) && header.msg_id() == wire::MsgId::connect,
                "game handshake completes");

        auto sign_in = [&](int sequence) {
            wire::Message request;
            request.tags["message_type_name"] = "request_open_session";
            send(wire::MsgId::data, request.encode(), sequence);
            for (int i = 0; i < 4; ++i) {
                const auto packet = receive();
                wire::Header reply{};
                if (wire::decode(packet.data(), packet.size(), reply) && reply.msg_id() == wire::MsgId::data)
                    return wire::Message::decode(packet.data() + wire::HEADER_SIZE, packet.size() - wire::HEADER_SIZE);
            }
            throw std::runtime_error("No sign-in reply received");
        };
        const auto first = sign_in(1);
        require(first.tags.at("auth_token") == "test-only-first-token", "game receives the launcher's current sign-in");
        require(first.tags.at("auth_endpoint") == "https://gateway.example.com:8443", "game receives the configured HTTPS gateway");
        require(aoc::ics_token::write(token_file, "test-only-next-token"), "next sign-in replaces the previous one");
        require(sign_in(2).tags.at("auth_token") == "test-only-next-token", "tether does not cache the previous account's token");
        closesocket(client);
        client = INVALID_SOCKET;
        tether.stop();
        require(!tether.is_running() && competing.start(tether.port(), true), "closing the launcher releases its port");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        result = 1;
    }
    if (client != INVALID_SOCKET) closesocket(client);
    std::error_code ec;
    fs::remove(token_file, ec);
    fs::remove(folder, ec);
    return result;
}
