#include "openrig_member.h"

#include <logonomic_cli_addon/json_utilities.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openrig {
namespace {

namespace addon = logonomic::cli_addon;
namespace json  = addon::json;

template<class T>
std::optional<T> failure(std::string* out_error, std::string_view message)
{
    if (out_error) {
        *out_error = message;
    }
    return std::nullopt;
}

bool native_wrapper_runtime(std::string_view runtime)
{
    return runtime == "claude-code" || runtime == "codex" || runtime == "pi";
}

bool identity_text(std::string_view value)
{
    return !value.empty() && value.find('\0') == std::string_view::npos &&
        addon::is_valid_utf8(value);
}

bool ecmascript_whitespace_only(std::string_view value)
{
    // OpenRig checks argv[0].trim(). Match ECMAScript's WhiteSpace and
    // LineTerminator set, which includes BOM but does not include NEL.
    constexpr std::array<std::string_view, 25> whitespace = {
        "\t", "\n", "\v", "\f", "\r", " ",
        "\xc2\xa0", "\xe1\x9a\x80",
        "\xe2\x80\x80", "\xe2\x80\x81", "\xe2\x80\x82", "\xe2\x80\x83",
        "\xe2\x80\x84", "\xe2\x80\x85", "\xe2\x80\x86", "\xe2\x80\x87",
        "\xe2\x80\x88", "\xe2\x80\x89", "\xe2\x80\x8a", "\xe2\x80\xa8",
        "\xe2\x80\xa9", "\xe2\x80\xaf", "\xe2\x81\x9f", "\xe3\x80\x80",
        "\xef\xbb\xbf",
    };
    while (!value.empty()) {
        const auto character = std::find_if(
            whitespace.begin(), whitespace.end(),
            [value](std::string_view candidate) { return value.starts_with(candidate); });
        if (character == whitespace.end()) {
            return false;
        }
        value.remove_prefix(character->size());
    }
    return true;
}

bool wrapper_arguments(std::span<const std::string> argv)
{
    for (const auto& argument : argv) {
        if (!addon::is_valid_utf8(argument) ||
            argument.find('\0') != std::string::npos ||
            argument.find('\r') != std::string::npos ||
            argument.find('\n') != std::string::npos)
        {
            return false;
        }
    }
    if (argv.empty()) {
        return true;
    }
    const auto& executable = argv.front();
    return !ecmascript_whitespace_only(executable) &&
        (executable.starts_with('/') || executable.find('/') == std::string::npos);
}

bool stable_wrapper_prefix(std::span<const std::string> argv)
{
    // This delimiter belongs to the provider caller contract. OpenRig permits
    // arbitrary existing wrapper arguments, including additional delimiters.
    return argv.size() >= 2 && wrapper_arguments(argv) && argv.back() == "--" &&
        std::find(argv.begin() + 1, argv.end() - 1, "--") == argv.end() - 1;
}

std::optional<std::size_t> selected_index(
    const addon::Json_value& values,
    std::string_view         selected_id)
{
    if (!values.is_array()) {
        return std::nullopt;
    }
    std::optional<std::size_t> found;
    for (std::size_t index = 0; index < values.as_array().size(); ++index) {
        const auto& entry = values.as_array()[index];
        const auto& id    = json::member(entry, "id");
        if (!id.is_string() || !identity_text(id.as_string())) {
            return std::nullopt;
        }
        if (id.as_string() == selected_id) {
            if (found) {
                return std::nullopt;
            }
            found = index;
        }
    }
    return found;
}

} // namespace

std::optional<Prepared_member> prepare_authored_member(
    const addon::Json_value&        specification,
    const Authored_member_selector& selector,
    std::span<const std::string>     stable_wrapper_argv,
    std::string*                    out_error)
{
    if (out_error) {
        out_error->clear();
    }
    if (!specification.is_object() ||
        !identity_text(selector.rig_name) ||
        !identity_text(selector.member_id) ||
        (selector.pod_id && !identity_text(*selector.pod_id)))
    {
        return failure<Prepared_member>(out_error, "An explicit rig and member selection is required.");
    }
    const auto& rig_name = json::member(specification, "name");
    if (!rig_name.is_string() || rig_name.as_string() != selector.rig_name) {
        return failure<Prepared_member>(out_error, "The selected rig differs from the authored specification.");
    }
    if (!stable_wrapper_prefix(stable_wrapper_argv)) {
        return failure<Prepared_member>(out_error,
            "The stable wrapper requires valid argv with one terminal -- after its executable.");
    }

    std::optional<std::size_t> pod_index;
    const addon::Json_value* members = nullptr;
    if (selector.pod_id) {
        if (specification.find("nodes")) {
            return failure<Prepared_member>(out_error, "A pod selection requires a pod-format specification.");
        }
        const auto& pods = json::member(specification, "pods");
        pod_index = selected_index(pods, *selector.pod_id);
        if (!pod_index) {
            return failure<Prepared_member>(out_error, "The selected pod must occur exactly once.");
        }
        members = &json::member(pods.as_array()[*pod_index], "members");
    }
    else {
        if (json::member(specification, "pods").is_array()) {
            return failure<Prepared_member>(out_error, "A pod-format specification requires an explicit pod.");
        }
        members = &json::member(specification, "nodes");
    }
    const auto member_index = selected_index(*members, selector.member_id);
    if (!member_index) {
        return failure<Prepared_member>(out_error, "The selected member must occur exactly once.");
    }
    const auto& member  = members->as_array()[*member_index];
    const auto runtime = json::text(member, "runtime");
    if (!native_wrapper_runtime(runtime)) {
        return failure<Prepared_member>(out_error, "The member requires a supported local native wrapper runtime.");
    }

    std::vector<std::string> previous_wrapper;
    if (const auto* wrapper = member.find("launch_wrapper")) {
        if (!wrapper->is_array()) {
            return failure<Prepared_member>(out_error, "The authored launch_wrapper must be an argv array.");
        }
        previous_wrapper.reserve(wrapper->as_array().size());
        for (const auto& argument : wrapper->as_array()) {
            if (!argument.is_string()) {
                return failure<Prepared_member>(out_error, "Every launch_wrapper argument must be a string.");
            }
            previous_wrapper.push_back(argument.as_string());
        }
        if (!wrapper_arguments(previous_wrapper)) {
            return failure<Prepared_member>(out_error, "The authored launch_wrapper has an invalid argument.");
        }
    }

    if (!previous_wrapper.empty() && previous_wrapper.front() == stable_wrapper_argv.front()) {
        const auto delimiter = std::find(previous_wrapper.begin() + 1, previous_wrapper.end(), "--");
        if (delimiter != previous_wrapper.end() &&
            std::equal(stable_wrapper_argv.begin(), stable_wrapper_argv.end(),
                previous_wrapper.begin(), delimiter + 1))
        {
            return Prepared_member{specification, runtime, false};
        }
        return failure<Prepared_member>(out_error, "The member already selects this wrapper with different arguments.");
    }

    addon::Json_value::Array composed;
    composed.reserve(stable_wrapper_argv.size() + previous_wrapper.size());
    for (const auto& argument : stable_wrapper_argv) {
        composed.emplace_back(argument);
    }
    for (auto& argument : previous_wrapper) {
        composed.emplace_back(std::move(argument));
    }

    Prepared_member prepared{specification, runtime, true};
    auto* prepared_members = selector.pod_id
        ? prepared.specification.find("pods")->as_array()[*pod_index].find("members")
        : prepared.specification.find("nodes");
    prepared_members->as_array()[*member_index].set("launch_wrapper", addon::Json_value(std::move(composed)));
    return prepared;
}

std::optional<Native_seat_identity> native_seat_identity(
    const addon::Environment& environment,
    std::string*              out_error)
{
    if (out_error) {
        out_error->clear();
    }
    constexpr std::array<std::string_view, 6> names = {
        "OPENRIG_HOME",
        "OPENRIG_DB",
        "OPENRIG_NODE_ID",
        "OPENRIG_SESSION_NAME",
        "OPENRIG_RUNTIME",
        "OPENRIG_OCCUPANT_GENERATION",
    };
    std::array<std::string_view, names.size()> values;
    for (std::size_t index = 0; index < names.size(); ++index) {
        bool found = false;
        for (const auto& [name, value] : environment) {
            if (name != names[index]) {
                continue;
            }
            if (found || !identity_text(value)) {
                return failure<Native_seat_identity>(out_error, "Native seat identity is invalid or ambiguous.");
            }
            values[index] = value;
            found = true;
        }
        if (!found) {
            return failure<Native_seat_identity>(out_error, "The native launch does not provide complete seat identity.");
        }
    }
    if (!native_wrapper_runtime(values[4])) {
        return failure<Native_seat_identity>(out_error, "The native runtime has no supported wrapper contract.");
    }
    return Native_seat_identity{
        std::string(values[0]),
        std::string(values[1]),
        std::string(values[2]),
        std::string(values[3]),
        std::string(values[4]),
        std::string(values[5]),
    };
}

} // namespace openrig
