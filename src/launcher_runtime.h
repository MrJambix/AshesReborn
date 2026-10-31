// SPDX-License-Identifier: AGPL-3.0-only
// Player setup for the directly launched executable. Development launch scripts
// keep managing their own tether unless managed_tether is enabled in server.json.
#pragma once
#include "services/tether/tether_service.h"
#include <windows.h>
#include <wincrypt.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>

namespace aoc::launcher {

inline std::string trust_player_certificate(const std::filesystem::path& file) {
    PCCERT_CONTEXT cert = nullptr;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, file.c_str(),
            CERT_QUERY_CONTENT_FLAG_CERT, CERT_QUERY_FORMAT_FLAG_ALL, 0,
            nullptr, nullptr, nullptr, nullptr, nullptr,
            reinterpret_cast<const void**>(&cert)))
        return "The server certificate is missing or damaged. Download the launcher again.";

    HCERTSTORE roots = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
        CERT_SYSTEM_STORE_CURRENT_USER, L"ROOT");
    if (!roots) {
        CertFreeCertificateContext(cert);
        return "Windows could not open your certificate store. Please try again.";
    }
    PCCERT_CONTEXT found = CertFindCertificateInStore(roots, X509_ASN_ENCODING, 0,
        CERT_FIND_EXISTING, cert, nullptr);
    bool trusted = found != nullptr;
    if (found) CertFreeCertificateContext(found);
    if (!trusted) {
        const int answer = MessageBoxW(nullptr,
            L"Reborn Launcher needs to trust its server certificate to connect. "
            L"It will be added to the trusted certificates for your Windows account. "
            L"Windows may ask you to confirm this. Continue?",
            L"Reborn Launcher - first connection", MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2);
        if (answer == IDYES)
            trusted = CertAddCertificateContextToStore(roots, cert, CERT_STORE_ADD_USE_EXISTING, nullptr) == TRUE;
    }
    CertCloseStore(roots, 0);
    CertFreeCertificateContext(cert);
    return trusted ? "" : "The server certificate was not trusted. Open the launcher again to complete setup.";
}

class PlayerRuntime {
    TetherServiceImpl tether_;
    HANDLE single_instance_ = nullptr;
    std::string token_file_;
public:
    ~PlayerRuntime() {
        tether_.stop();
        if (!token_file_.empty()) {
            std::error_code ec;
            std::filesystem::remove(token_file_, ec);
        }
        if (single_instance_) CloseHandle(single_instance_);
    }

    std::string start(const nlohmann::json& config, const std::string& dir) {
        const auto managed = config.find("managed_tether");
        if (managed == config.end() || !managed->is_boolean() || !managed->get<bool>()) return "";
        single_instance_ = CreateMutexW(nullptr, FALSE, L"Local\\RebornLauncher.Player");
        if (!single_instance_) return "Windows could not start the launcher. Please try again.";
        if (GetLastError() == ERROR_ALREADY_EXISTS)
            return "Reborn Launcher is already open. Use its existing window.";

        const std::string error = trust_player_certificate(std::filesystem::path(dir) / L"ca.crt");
        if (!error.empty()) return error;
        token_file_ = aoc::ics_token::path_in(dir);
        std::error_code ec;
        std::filesystem::remove(token_file_, ec);
        if (ec) return "The previous sign-in could not be cleared. Please close the game and try again.";

        const auto endpoint = config.find("ics_host");
        const auto port = config.find("ics_port");
        const auto local = config.find("tether_port");
        if (endpoint == config.end() || !endpoint->is_string() || endpoint->get<std::string>().empty() ||
            port == config.end() || !port->is_number_integer() ||
            local == config.end() || !local->is_number_integer())
            return "The launcher settings are damaged. Download the launcher again.";
        const auto gateway_port = port->get<long long>(), tether_port = local->get<long long>();
        if (gateway_port < 1 || gateway_port > 65535 || tether_port < 1 || tether_port > 65535)
            return "The launcher settings are damaged. Download the launcher again.";
        tether_.set_ics_endpoint(endpoint->get<std::string>(), static_cast<uint16_t>(gateway_port));
        tether_.set_token_file(token_file_);
        if (!tether_.start(static_cast<uint16_t>(tether_port), true))
            return "The game connection helper could not start. Close older launchers and try again.";
        return "";
    }
};
} // namespace aoc::launcher
