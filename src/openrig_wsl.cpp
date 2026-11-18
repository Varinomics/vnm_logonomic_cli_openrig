#include "openrig_wsl.h"

#include "openrig_launcher.h"

#include <logonomic_cli_addon/launcher_utilities.h>
#include <logonomic_cli_addon/limits.h>
#include <logonomic_cli_addon/package_source.h>
#include <logonomic_cli_addon/sha256.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <wslapi.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string_view>

namespace openrig {
namespace {

namespace addon = logonomic::cli_addon;
namespace fs    = std::filesystem;

std::optional<fs::path> system_wsl()
{
    std::array<wchar_t, MAX_PATH> directory{};
    const auto length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
    if (!length || length >= directory.size()) return std::nullopt;
    return addon::regular_file(fs::path(directory.data()) / L"wsl.exe");
}

bool wsl2_distribution(const std::string& name)
{
    if (name.empty() || name.front() == '-' || !addon::is_valid_utf8(name)) return false;
    // Resolve only the system API. Without WSL the package can still load and report no candidates.
    const HMODULE library = LoadLibraryExW(L"wslapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library) return false;
    using Get_configuration = decltype(&WslGetDistributionConfiguration);
    const auto get = std::bit_cast<Get_configuration>(GetProcAddress(library, "WslGetDistributionConfiguration"));
    ULONG version = 0, default_uid = 0, count = 0;
    WSL_DISTRIBUTION_FLAGS flags{};
    PSTR* environment = nullptr;
    const auto status = get ? get(addon::wide(name).c_str(), &version, &default_uid, &flags, &environment, &count) : E_FAIL;
    if (SUCCEEDED(status)) {
        for (ULONG index = 0; index < count; ++index) CoTaskMemFree(environment[index]);
        CoTaskMemFree(environment);
    }
    FreeLibrary(library);
    return SUCCEEDED(status) && version == 2;
}

std::string distribution_list(std::string_view bytes)
{
    if (bytes.find('\0') == std::string_view::npos) return addon::is_valid_utf8(bytes) ? std::string(bytes) : "";
    if (bytes.size() % 2 != 0) return {};
    // wsl.exe writes its own redirected lists as UTF-16LE. Guest command output is UTF-8.
    std::wstring wide;
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        const auto character = static_cast<wchar_t>(static_cast<unsigned char>(bytes[i]) |
            (static_cast<unsigned char>(bytes[i + 1]) << 8));
        if (character == 0) return {};
        if (i != 0 || character != 0xfeff) wide.push_back(character);
    }
    const int length = static_cast<int>(wide.size());
    const int utf8_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length, nullptr, 0, nullptr, nullptr);
    if (utf8_size <= 0) return {};
    std::string result(static_cast<std::size_t>(utf8_size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), length, result.data(), utf8_size, nullptr, nullptr);
    return result;
}

bool guest_rig(std::string_view word)
{
    return word == "rig" || (word.starts_with('/') && word.ends_with("/rig"));
}

bool usable_word(std::string_view word)
{
    return !word.empty() && addon::is_valid_utf8(word) &&
        std::none_of(word.begin(), word.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

std::string direct_word(std::string_view word)
{
    std::string value = "\"";
    for (char c : word) {
        if (c == '"') value += "\"\"";
        value += c;
    }
    return value + '"';
}

std::optional<std::string> expand_search_value(std::string_view value, const addon::Environment& ambient)
{
    std::string expanded;
    for (;;) {
        const auto open = value.find('%');
        const auto close = open == value.npos ? value.npos : value.find('%', open + 1);
        if (close == value.npos) return expanded + std::string(value);
        expanded.append(value.substr(0, open));
        const auto name = addon::lower(std::string(value.substr(open + 1, close - open - 1)));
        if (name.empty()) {
            expanded += "%%";
        } else {
            const auto found = std::find_if(ambient.begin(), ambient.end(), [&](const auto& entry) {
                return addon::lower(entry.first) == name;
            });
            if (found == ambient.end()) return std::nullopt;
            expanded += found->second;
        }
        value.remove_prefix(close + 1);
    }
}

bool selects_system_wsl(const std::string& program, const addon::Recognize_request& request)
{
    const auto system = system_wsl();
    if (!system) return false;
    const auto ambient = addon::environment_pairs(request.ambient_environment);
    auto environment = ambient;
    if (!addon::path_from_utf8(program).is_absolute()) {
        // Recognition receives the ambient map and the saved KEY=value lines
        // separately. Expand against the captured ambient values, as the
        // launcher does, and decline references to undisclosed variables.
        std::istringstream lines(request.launcher.environment_text);
        bool path_seen = false, extensions_seen = false;
        for (std::string raw; std::getline(lines, raw);) {
            const auto line = addon::trim(raw);
            const auto separator = line.find('=');
            if (separator == std::string::npos) continue;
            const auto name = addon::lower(addon::trim(line.substr(0, separator)));
            if (name != "path" && name != "pathext") continue;
            bool& seen = name == "path" ? path_seen : extensions_seen;
            const auto value = expand_search_value(std::string_view(line).substr(separator + 1), ambient);
            if (seen || !value) return false;
            seen = true;
            addon::set_environment(environment, name, *value);
        }
        // Recognition has no launch cwd with which to resolve a relative
        // PATH entry. It cannot skip that entry and claim a later system WSL.
        std::istringstream directories(addon::environment_value(environment, "PATH"));
        for (std::string directory; std::getline(directories, directory, ';');) {
            if (!directory.empty() && !addon::path_from_utf8(addon::unquote(directory)).is_absolute()) return false;
        }
    }
    const auto selected = addon::resolve_program(program, environment, "");
    std::error_code error;
    return selected && fs::equivalent(*system, *selected, error) && !error;
}

} // namespace

std::vector<addon::Launcher_candidate> discover_wsl(const addon::Discover_request& request, addon::Invocation_context& context)
{
    std::vector<addon::Launcher_candidate> candidates;
    const auto wsl = system_wsl();
    if (!wsl) return candidates;
    const auto program = addon::path_to_utf8(*wsl);
    const auto environment = addon::environment_pairs(request.ambient_environment);
    const auto started = std::chrono::steady_clock::now();
    const auto timeout = [&] {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        return static_cast<int>(std::max<std::int64_t>(0, std::min<std::int64_t>(
            context.request().deadline_ms, addon::limits::k_launch_prepare_deadline_ms) - elapsed.count()));
    };
    const int available = timeout();
    if (!available || context.cancel_requested()) return candidates;
    const auto list = addon::probe({program, "--list", "--running", "--quiet"}, environment,
                                   request.default_home_fallback, available, context);
    if (!list) return candidates;
    const auto names = distribution_list(list->output);
    // The guest shell executes this fixed program, never a formatted distribution, path or launcher argument.
    static constexpr auto identify =
        "p=$(command -v rig) || exit 1; case \"$p\" in /*) ;; *) exit 1;; esac; "
        "printf '%s\\n' \"$p\" && exec \"$p\" --help";
    for (std::size_t begin = 0; begin < names.size() && candidates.size() < addon::limits::k_max_discovery_candidates;) {
        const auto end = names.find('\n', begin);
        auto name = names.substr(begin, end - begin);
        if (name.ends_with('\r')) name.pop_back();
        begin = end == std::string::npos ? names.size() : end + 1;
        if (context.cancel_requested() || !timeout()) break;
        if (!usable_word(name) || !wsl2_distribution(name)) continue;

        const auto probe = addon::probe({program, "--distribution", name, "--exec", "/bin/sh", "-c", identify},
                                        environment, request.default_home_fallback, timeout(), context);
        if (!probe) continue;
        const auto split = probe->output.find('\n');
        if (split == std::string::npos || !openrig_help(std::string_view(probe->output).substr(split + 1))) continue;
        const auto guest = probe->output.substr(0, split);
        if (!guest.starts_with('/') || !usable_word(guest) || !guest_rig(guest)) continue;
        const auto script = direct_word(program) + " --distribution " + direct_word(name) + " --exec " + direct_word(guest);
        const auto invocation = addon::literal_launcher_invocation({script, addon::k_runner_direct, {}}, request.platform_id);
        if (!invocation || invocation->argv != std::vector<std::string>{program, "--distribution", name, "--exec", guest}) continue;

        addon::Launcher_candidate result;
        result.candidate_id = "openrig_wsl_" + addon::to_hex(addon::Sha256::digest(name)).substr(0, 24);
        result.display_name = "OpenRig (" + name + ")";
        result.runner_key   = addon::k_runner_direct;
        result.script_body  = script;
        result.evidence.set("source", addon::Json_value("wsl2-path-and-help"));
        candidates.push_back(std::move(result));
    }
    if (candidates.size() == 1) candidates.front().is_default = true;
    return candidates;
}

bool recognize_wsl(const addon::Recognize_request& request)
{
    const auto invocation = addon::literal_launcher_invocation(request.launcher, request.platform_id);
    if (!invocation || invocation->argv.size() < 5 || !invocation->environment_names.empty()) return false;
    const auto& words = invocation->argv;
    if (!selects_system_wsl(words.front(), request)) return false;

    std::string distribution;
    bool has_user = false, has_directory = false;
    for (std::size_t i = 1; i + 1 < words.size(); i += 2) {
        const auto& option = words[i];
        const auto& value  = words[i + 1];
        if (option == "--exec" || option == "-e") return guest_rig(value) && wsl2_distribution(distribution);
        if (value.empty() || value.front() == '-') return false;
        if ((option == "--distribution" || option == "-d") && distribution.empty()) distribution = value;
        else if ((option == "--user" || option == "-u") && !has_user) has_user = true;
        else if (option == "--cd" && !has_directory) has_directory = true;
        else return false;
    }
    return false;
}

} // namespace openrig
