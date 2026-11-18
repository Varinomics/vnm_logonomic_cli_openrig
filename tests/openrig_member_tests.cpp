#include "openrig_member.h"

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/json_utilities.h>
#include <logonomic_cli_addon/launcher_utilities.h>

#include <algorithm>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace addon = logonomic::cli_addon;
namespace json  = addon::json;

bool check(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
    }
    return condition;
}

addon::Json_value parse(std::string_view text)
{
    auto parsed = addon::parse_json(text, {}, nullptr);
    if (!parsed) {
        throw std::runtime_error("Invalid JSON fixture.");
    }
    return std::move(*parsed);
}

addon::Json_value specification()
{
    return parse(R"({
        "version": "0.2",
        "name": "compiler-team",
        "env": {"CLAUDE_CONFIG_DIR": "/accounts/work", "EDITOR": "vim"},
        "permission_policy": "builtin:standard",
        "pods": [
            {
                "id": "build",
                "label": "Build",
                "members": [
                    {
                        "id": "worker",
                        "runtime": "claude-code",
                        "agent_ref": "local:./agents/build",
                        "profile": "default",
                        "cwd": "./source with spaces",
                        "claude_mcp": "none",
                        "plugin_dirs": ["./plugins/team"],
                        "launch_wrapper": ["sandbox", "exec", "--profile", "build", "--"]
                    },
                    {
                        "id": "reviewer",
                        "runtime": "codex",
                        "agent_ref": "local:./agents/review",
                        "profile": "review",
                        "cwd": ".",
                        "codex_config_profile": "work",
                        "launch_wrapper": ["review-sandbox", "--"]
                    }
                ]
            },
            {
                "id": "qa",
                "label": "QA",
                "members": [
                    {
                        "id": "worker",
                        "runtime": "pi",
                        "agent_ref": "local:./agents/qa",
                        "profile": "default",
                        "cwd": "./tests",
                        "launch_wrapper": []
                    }
                ]
            }
        ]
    })");
}

std::vector<std::string> stable_prefix()
{
    return {"/opt/logonomic/bin/seat-launch", "work-profile", "--"};
}

addon::Json_value& build_member(addon::Json_value& spec)
{
    return spec.find("pods")->as_array()[0].find("members")->as_array()[0];
}

bool selected_member_preserves_specification()
{
    const auto original = specification();
    auto expected       = original;
    build_member(expected).set("launch_wrapper", parse(R"([
        "/opt/logonomic/bin/seat-launch", "work-profile", "--",
        "sandbox", "exec", "--profile", "build", "--"
    ])"));

    std::string error;
    const auto prepared = openrig::prepare_authored_member(
        original, {"compiler-team", "build", "worker"}, stable_prefix(), &error);
    if (!check(prepared.has_value(), "The explicitly selected member must prepare.")) {
        return false;
    }
    bool ok = true;
    ok &= check(prepared->specification == expected,
        "Only the selected launch_wrapper changes; profiles, policy, paths and other seats remain exact.");
    ok &= check(prepared->runtime == "claude-code", "The authored runtime is retained.");
    ok &= check(prepared->changed && error.empty(), "A new prefix is reported as a successful change.");
    ok &= check(original == specification(), "The input specification is not mutated.");

    const auto repeated = openrig::prepare_authored_member(
        prepared->specification, {"compiler-team", "build", "worker"}, stable_prefix(), &error);
    ok &= check(repeated && !repeated->changed && repeated->specification == expected,
        "Repeating the same prefix must not duplicate it.");

    auto shortened_prefix = stable_prefix();
    shortened_prefix.pop_back();
    const auto shortened = openrig::prepare_authored_member(
        prepared->specification, {"compiler-team", "build", "worker"}, shortened_prefix, &error);
    ok &= check(!shortened && !error.empty(), "A shortened managed prefix is not an unchanged installation.");

    const std::vector<std::string> shorter_delimited_prefix = {stable_prefix().front(), "--"};
    const auto shorter_delimited = openrig::prepare_authored_member(
        prepared->specification, {"compiler-team", "build", "worker"}, shorter_delimited_prefix, &error);
    ok &= check(!shorter_delimited && !error.empty(),
        "A terminal delimiter does not make a different, shorter prefix idempotent.");

    auto suffix_absorbing_prefix = stable_prefix();
    suffix_absorbing_prefix.insert(suffix_absorbing_prefix.end(),
        {"sandbox", "exec", "--profile", "build", "--"});
    const auto suffix_absorbing = openrig::prepare_authored_member(
        prepared->specification, {"compiler-team", "build", "worker"}, suffix_absorbing_prefix, &error);
    ok &= check(!suffix_absorbing && !error.empty(),
        "An owned prefix cannot absorb the inherited wrapper beyond its first delimiter.");

    auto conflicting_prefix = stable_prefix();
    conflicting_prefix[1] = "different-profile";
    const auto conflicting = openrig::prepare_authored_member(
        prepared->specification, {"compiler-team", "build", "worker"}, conflicting_prefix, &error);
    ok &= check(!conflicting && !error.empty(), "A changed prefix at the same stable entrypoint is refused.");
    return ok;
}

bool preserves_literal_arguments()
{
    auto spec = specification();
    auto prior = addon::Json_value::array();
    for (const char* value : {"/opt/seat tools/launcher", "", "'quoted'", "$(unchanged)", "a;b", "révision"}) {
        prior.push_back(addon::Json_value(value));
    }
    build_member(spec).set("launch_wrapper", prior);
    auto prefix = stable_prefix();
    prefix.insert(prefix.begin() + 1, "");
    const auto prepared = openrig::prepare_authored_member(
        spec, {"compiler-team", "build", "worker"}, prefix, nullptr);
    if (!check(prepared.has_value(), "Empty arguments and shell metacharacters are literal argv.")) {
        return false;
    }
    auto expected = addon::Json_value::array();
    for (const auto& argument : prefix) {
        expected.push_back(addon::Json_value(argument));
    }
    for (const auto& argument : prior.as_array()) {
        expected.push_back(argument);
    }
    auto prepared_spec = prepared->specification;
    return check(json::member(build_member(prepared_spec), "launch_wrapper") == expected,
        "Prefix and prior wrapper arguments retain exact bytes and order.");
}

bool explicit_scope_is_required()
{
    const auto spec = specification();
    const std::vector<openrig::Authored_member_selector> invalid = {
        {"",              "build",       "worker"},
        {"compiler-team", "build",       ""},
        {"other-team",    "build",       "worker"},
        {"compiler-team", "missing-pod", "worker"},
        {"compiler-team", "build",       "missing-member"},
        {"compiler-team", "",            "worker"},
        {"compiler-team", std::nullopt,   "worker"},
    };
    bool ok = true;
    for (const auto& selector : invalid) {
        std::string error;
        ok &= check(!openrig::prepare_authored_member(spec, selector, stable_prefix(), &error) && !error.empty(),
            "An absent, mismatched or unscoped member selection is refused.");
    }
    auto duplicate_member = spec;
    auto* members = duplicate_member.find("pods")->as_array()[0].find("members");
    members->push_back(members->as_array()[0]);
    ok &= check(!openrig::prepare_authored_member(
        duplicate_member, {"compiler-team", "build", "worker"}, stable_prefix(), nullptr),
        "A duplicate selected member is ambiguous.");

    auto duplicate_pod = spec;
    auto* pods = duplicate_pod.find("pods");
    pods->push_back(pods->as_array()[0]);
    ok &= check(!openrig::prepare_authored_member(
        duplicate_pod, {"compiler-team", "build", "worker"}, stable_prefix(), nullptr),
        "A duplicate selected pod is ambiguous.");
    return ok;
}

bool native_runtime_and_wrapper_contract()
{
    bool ok = true;
    for (const char* runtime : {"claude-code", "codex", "pi"}) {
        auto spec = specification();
        build_member(spec).set("runtime", addon::Json_value(runtime));
        build_member(spec).erase("launch_wrapper");
        const auto prepared = openrig::prepare_authored_member(
            spec, {"compiler-team", "build", "worker"}, stable_prefix(), nullptr);
        ok &= check(prepared && prepared->runtime == runtime,
            "Each source-confirmed local native wrapper runtime can be prepared.");
    }
    for (const char* runtime : {"terminal", "omp", "openshell-claude", "community-example", ""}) {
        auto spec = specification();
        build_member(spec).set("runtime", addon::Json_value(runtime));
        ok &= check(!openrig::prepare_authored_member(
            spec, {"compiler-team", "build", "worker"}, stable_prefix(), nullptr),
            "Runtime-specific wrapper support cannot be inferred for other runtimes.");
    }

    const std::vector<addon::Json_value> invalid_wrappers = {
        addon::Json_value(),
        addon::Json_value("launcher"),
        parse(R"([1])"),
        parse(R"([""])"),
        parse(R"(["  \t"])"),
        parse(R"(["\uFEFF"])"),
        parse(R"(["./launcher"])"),
        parse(R"(["launcher","line\nfeed"])"),
        parse(R"(["launcher","carriage\rreturn"])"),
        parse(R"(["launcher","nul\u0000byte"])"),
    };
    for (const auto& wrapper : invalid_wrappers) {
        auto spec = specification();
        build_member(spec).set("launch_wrapper", wrapper);
        ok &= check(!openrig::prepare_authored_member(
            spec, {"compiler-team", "build", "worker"}, stable_prefix(), nullptr),
            "Malformed authored wrappers cannot be normalized into a different command.");
    }
    auto spec = specification();
    build_member(spec).set("launch_wrapper", addon::Json_value::array());
    ok &= check(openrig::prepare_authored_member(
        spec, {"compiler-team", "build", "worker"}, stable_prefix(), nullptr).has_value(),
        "An explicit empty authored wrapper represents no existing prefix.");

    auto nel_wrapper = spec;
    build_member(nel_wrapper).set("launch_wrapper", parse(R"(["\u0085"])"));
    ok &= check(openrig::prepare_authored_member(
        nel_wrapper, {"compiler-team", "build", "worker"}, stable_prefix(), nullptr).has_value(),
        "OpenRig's ECMAScript trim does not reject NEL as an executable name.");

    const std::vector<std::string> bom_prefix = {"\xef\xbb\xbf", "work-profile", "--"};
    ok &= check(!openrig::prepare_authored_member(
        spec, {"compiler-team", "build", "worker"}, bom_prefix, nullptr),
        "OpenRig's ECMAScript trim rejects a BOM-only executable name.");

    const std::vector<std::string> nel_prefix = {"\xc2\x85", "work-profile", "--"};
    ok &= check(openrig::prepare_authored_member(
        spec, {"compiler-team", "build", "worker"}, nel_prefix, nullptr).has_value(),
        "The owned prefix uses the same ECMAScript executable whitespace rule.");

    const std::vector<std::vector<std::string>> invalid_prefixes = {
        {}, {"", "--"}, {"local/launcher", "--"}, {"launcher", "line\nfeed", "--"},
        {"launcher"}, {"launcher", "work-profile"}, {"launcher", "--", "suffix"},
        {"launcher", "--", "--"},
    };
    for (const auto& prefix : invalid_prefixes) {
        ok &= check(!openrig::prepare_authored_member(
            spec, {"compiler-team", "build", "worker"}, prefix, nullptr),
            "The owned prefix requires valid native argv and exactly one terminal delimiter.");
    }
    return ok;
}

bool flat_node_format()
{
    auto spec = parse(R"({
        "schema_version": 1,
        "version": "0.1",
        "name": "build-rig",
        "nodes": [{"id": "reviewer", "runtime": "codex", "cwd": ".", "model": "default"}],
        "edges": []
    })");
    auto expected = spec;
    auto wrapper = addon::Json_value::array();
    for (const auto& argument : stable_prefix()) {
        wrapper.push_back(addon::Json_value(argument));
    }
    expected.find("nodes")->as_array()[0].set("launch_wrapper", std::move(wrapper));
    const auto prepared = openrig::prepare_authored_member(
        spec, {"build-rig", std::nullopt, "reviewer"}, stable_prefix(), nullptr);
    bool ok = check(prepared && prepared->changed && prepared->specification == expected,
        "An explicit flat-node selector changes only that node's wrapper.");
    spec.set("pods", addon::Json_value());
    expected.set("pods", addon::Json_value());
    const auto null_pods = openrig::prepare_authored_member(
        spec, {"build-rig", std::nullopt, "reviewer"}, stable_prefix(), nullptr);
    ok &= check(null_pods && null_pods->changed && null_pods->specification == expected,
        "A non-array pods field retains upstream's flat-node format selection.");
    return ok;
}

addon::Environment native_environment()
{
    return {
        {"OPENRIG_HOME",                "/home/user/.openrig"},
        {"OPENRIG_DB",                  "/home/user/.openrig/openrig.db"},
        {"OPENRIG_NODE_ID",             "member-compiler"},
        {"OPENRIG_SESSION_NAME",        "build-worker@compiler-team"},
        {"OPENRIG_RUNTIME",             "claude-code"},
        {"OPENRIG_OCCUPANT_GENERATION", "14c58c60-3270-4f13-a0f2-bf5171b97259"},
        {"OPENRIG_ACTIVITY_HOOK_TOKEN", "fixture-only-not-authority"},
        {"LOGONOMIC_SESSION_CAPABILITY", "fixture-only-not-authority"},
    };
}

bool native_identity_keeps_seats_and_generations_separate()
{
    const auto environment = native_environment();
    std::string error;
    const auto identity = openrig::native_seat_identity(environment, &error);
    const openrig::Native_seat_identity expected = {
        "/home/user/.openrig",
        "/home/user/.openrig/openrig.db",
        "member-compiler",
        "build-worker@compiler-team",
        "claude-code",
        "14c58c60-3270-4f13-a0f2-bf5171b97259",
    };
    bool ok = check(identity && *identity == expected && error.empty(),
        "Only the exact public native routing identity is retained.");
    auto other_seat = environment;
    addon::set_environment(other_seat, "OPENRIG_NODE_ID", "member-reviewer");
    addon::set_environment(other_seat, "OPENRIG_SESSION_NAME", "build-reviewer@compiler-team");
    const auto sibling = openrig::native_seat_identity(other_seat, nullptr);
    ok &= check(identity && sibling && *identity != *sibling, "Concurrent seats retain distinct identities.");

    auto successor = environment;
    addon::set_environment(successor, "OPENRIG_OCCUPANT_GENERATION", "934da487-d348-48d0-899d-3efcde62f3d7");
    const auto replaced = openrig::native_seat_identity(successor, nullptr);
    ok &= check(identity && replaced && *identity != *replaced,
        "A replacement generation remains distinct even at the same node and session.");

    auto adopted_name = environment;
    addon::set_environment(adopted_name, "OPENRIG_SESSION_NAME", "existing user's tmux seat");
    const auto adopted = openrig::native_seat_identity(adopted_name, nullptr);
    ok &= check(adopted && adopted->session_name == "existing user's tmux seat",
        "A restored adopted session is not forced into a guessed naming grammar.");
    return ok;
}

bool incomplete_identity_is_unavailable()
{
    bool ok = true;
    const auto environment = native_environment();
    for (std::size_t index = 0; index < 6; ++index) {
        auto missing = environment;
        missing.erase(missing.begin() + index);
        std::string error;
        ok &= check(!openrig::native_seat_identity(missing, &error) && !error.empty(),
            "Every native identity field is required; missing generation is never invented.");
        auto empty = environment;
        empty[index].second.clear();
        ok &= check(!openrig::native_seat_identity(empty, nullptr), "Empty native identity is unavailable.");
    }
    auto duplicate = environment;
    duplicate.push_back({"OPENRIG_OCCUPANT_GENERATION", "ambiguous-successor"});
    ok &= check(!openrig::native_seat_identity(duplicate, nullptr), "Duplicate identity fields are ambiguous.");

    auto malformed = environment;
    malformed[2].second = std::string("node\0suffix", 11);
    ok &= check(!openrig::native_seat_identity(malformed, nullptr), "Native identity cannot contain NUL.");
    malformed[2].second = std::string(1, char(0xff));
    ok &= check(!openrig::native_seat_identity(malformed, nullptr), "Native identity must be representable as UTF-8.");

    auto unsupported = environment;
    addon::set_environment(unsupported, "OPENRIG_RUNTIME", "openshell-claude");
    ok &= check(!openrig::native_seat_identity(unsupported, nullptr), "An unsupported wrapper runtime is unavailable.");
    return ok;
}

} // namespace

int main()
{
    try {
        bool ok = true;
        ok &= selected_member_preserves_specification();
        ok &= preserves_literal_arguments();
        ok &= explicit_scope_is_required();
        ok &= native_runtime_and_wrapper_contract();
        ok &= flat_node_format();
        ok &= native_identity_keeps_seats_and_generations_separate();
        ok &= incomplete_identity_is_unavailable();
        return ok ? 0 : 1;
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
