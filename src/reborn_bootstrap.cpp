// SPDX-License-Identifier: AGPL-3.0-only
// The downloadable executable: installs its embedded payload for the current
// Windows user and opens the native launcher. No PowerShell or administrator needed.
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::pair<const char*, DWORD> resource(int id) {
    HRSRC entry = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!entry) throw std::runtime_error("The launcher download is incomplete. Please download it again.");
    const DWORD size = SizeofResource(nullptr, entry);
    const char* data = static_cast<const char*>(LockResource(LoadResource(nullptr, entry)));
    if (!data || !size) throw std::runtime_error("The launcher download is damaged. Please download it again.");
    return {data, size};
}

static bool matches(const fs::path& path, const char* data, DWORD size) {
    std::error_code ec;
    if (fs::file_size(path, ec) != size || ec) return false;
    std::ifstream in(path, std::ios::binary);
    std::array<char, 65536> buffer;
    for (DWORD offset = 0; offset < size;) {
        const auto n = std::min<size_t>(buffer.size(), size - offset);
        if (!in.read(buffer.data(), n) || std::memcmp(buffer.data(), data + offset, n)) return false;
        offset += static_cast<DWORD>(n);
    }
    return true;
}

static void extract(const nlohmann::json& manifest, const fs::path& root) {
    fs::create_directories(root);
    for (const auto& file : manifest.at("files")) {
        const fs::path relative = fs::u8path(file.at("path").get<std::string>());
        if (relative.empty() || relative.has_root_path()) throw std::runtime_error("Invalid launcher package path.");
        for (const auto& part : relative)
            if (part == L".." || part == L".") throw std::runtime_error("Invalid launcher package path.");
        const fs::path destination = root / relative;
        // Players' selected game path belongs to them, including after a restart.
        if (relative == fs::path(L"config/paths.json") && fs::exists(destination)) continue;
        const auto [data, size] = resource(file.at("id").get<int>());
        if (matches(destination, data, size)) continue;
        fs::create_directories(destination.parent_path());
        fs::path temporary = destination;
        temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.write(data, size);
            out.close();
            if (!out) throw std::runtime_error("Could not unpack the launcher. Check free disk space and folder permissions.");
        }
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code ec;
            fs::remove(temporary, ec);
            throw std::runtime_error("Could not update the launcher files. Close other launcher windows and try again.");
        }
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command_line, int) {
    bool extract_only = false;
    try {
        int count = 0;
        LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &count);
        std::vector<std::wstring> args;
        if (raw) {
            for (int i = 1; i < count; ++i) args.emplace_back(raw[i]);
            LocalFree(raw);
        }
        extract_only = !args.empty() && args.front() == L"--extract-only";
        if (extract_only && args.size() != 2) return 2;
        const auto [data, size] = resource(1);
        const auto manifest = nlohmann::json::parse(data, data + size);
        const std::string payload_id = manifest.at("payload_id").get<std::string>();
        if (payload_id.empty() || payload_id.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("Invalid launcher package version.");

        fs::path root;
        if (extract_only) {
            root = fs::absolute(args[1]);
        } else {
            PWSTR local = nullptr;
            if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local)))
                throw std::runtime_error("Windows could not find your application folder.");
            root = fs::path(local) / L"RebornLauncher" / L"versions" / payload_id;
            CoTaskMemFree(local);
        }
        extract(manifest, root);
        if (extract_only) return 0;  // Packaging smoke test; no certificates or processes touched.

        const fs::path launcher = root / L"RebornLauncher.exe";
        std::wstring line = L"\"" + launcher.wstring() + L"\" " + command_line;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(launcher.c_str(), line.data(), nullptr, nullptr, FALSE, 0,
                            nullptr, root.c_str(), &startup, &process))
            throw std::runtime_error("The launcher could not open. Please download it again and try once more.");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return 0;
    } catch (const std::exception& error) {
        if (!extract_only) MessageBoxA(nullptr, error.what(), "Reborn Launcher", MB_OK | MB_ICONERROR);
        return 1;
    }
}
