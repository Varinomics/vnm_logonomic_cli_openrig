#include "openrig_launcher.h"

#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/launcher_discovery.h>
#include <logonomic_cli_addon/launcher_utilities.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openrig {
namespace {

namespace addon = logonomic::cli_addon;

bool supported_platform(std::string_view platform)
{
    return platform == "linux-x86_64" || platform == "macos-arm64";
}

bool openrig_help(std::string_view help)
{
    bool rig_usage = false;
    bool identity  = false;
    while (!help.empty()) {
        const auto end = help.find('\n');
        const auto line = addon::trim(help.substr(0, end), addon::Launcher_whitespace::ASCII);
        // Commander prints the root description before its options and command
        // lists. A later example mentioning OpenRig cannot identify another rig.
        if (line == "Options:" || line == "Commands:") {
            break;
        }
        rig_usage |= line.starts_with("Usage: rig ");
        identity  |= line.find("OpenRig") != std::string::npos;
        if (end == std::string_view::npos) {
            break;
        }
        help.remove_prefix(end + 1);
    }
    return rig_usage && identity;
}

void reject(addon::Invocation_context& context, const addon::Contract_failure& failure)
{
    context.fail(addon::Result_status::FAILED, failure.reason_code, failure.detail);
}

} // namespace

void discover(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_discover_request(context.request().payload, &failure);
    if (!request) {
        reject(context, failure);
        return;
    }

    std::vector<addon::Launcher_candidate> candidates;
    if (supported_platform(request->platform_id)) {
        const auto environment = addon::environment_pairs(request->ambient_environment);
        const auto program = addon::resolve_program("rig", environment, request->default_home_fallback);
        if (program) {
            // Bare rig opens a dashboard; --version is unlabelled. Root help
            // identifies OpenRig without invoking a daemon or native seat action.
            const auto output = addon::probe(
                {program->string(), "--help"}, environment, request->default_home_fallback, 3000, context);
            if (output && openrig_help(output->output)) {
                addon::Launcher_candidate candidate;
                candidate.candidate_id = "openrig";
                candidate.display_name = "OpenRig";
                candidate.script_body  = "rig";
                candidate.runner_key = addon::preferred_discovery_runner(*program, addon::k_runner_bash);
                candidate.is_default = true;
                candidate.evidence.set("source", addon::Json_value("path-and-help"));
                candidates.push_back(std::move(candidate));
            }
        }
    }
    context.succeed(addon::encode_candidates(candidates));
}

void recognize(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_recognize_request(context.request().payload, &failure);
    if (!request) {
        reject(context, failure);
        return;
    }

    addon::Launcher_recognition recognition;
    if (supported_platform(request->platform_id)) {
        const auto invocation = addon::literal_launcher_invocation(request->launcher, request->platform_id);
        recognition.recognized = invocation && !invocation->argv.empty() &&
            addon::base_name(invocation->argv.front()) == "rig";
    }
    if (recognition.recognized) {
        recognition.display_name = "OpenRig";
        recognition.evidence.set("source", addon::Json_value("literal-invocation"));
    }
    context.succeed(addon::encode_recognition(recognition));
}

void prepare(addon::Invocation_context& context)
{
    addon::Contract_failure failure;
    const auto request = addon::decode_prepare_request(context.request().payload, &failure);
    if (!request) {
        reject(context, failure);
        return;
    }
    if (request->initial_prompt) {
        context.fail(addon::Result_status::UNAVAILABLE, addon::reason::k_initial_prompt_unavailable);
        return;
    }

    addon::Launch_patch patch;
    patch.profile_storage = request->profile_storage;
    patch.account_context = request->account_context;
    patch.disposition     = addon::Launch_disposition{false, false, false};
    // A dashboard has no seat authority. The SDK's unmanaged result preserves
    // the complete selected invocation without pretending to install hooks.
    context.succeed(addon::encode_launch_patch(patch));
}

} // namespace openrig
