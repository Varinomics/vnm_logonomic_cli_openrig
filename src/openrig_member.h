#pragma once

#include <logonomic_cli_addon/json.h>
#include <logonomic_cli_addon/launcher_utilities.h>

#include <optional>
#include <span>
#include <string>

namespace openrig {

struct Authored_member_selector
{
    std::string                rig_name;
    std::optional<std::string> pod_id;
    std::string                member_id;
    // When supplied, null expects an absent launch_wrapper; an array expects
    // that exact authored value. An empty array is distinct from absence.
    std::optional<logonomic::cli_addon::Json_value> expected_wrapper = std::nullopt;
};

struct Prepared_member
{
    logonomic::cli_addon::Json_value specification;
    std::string                     runtime;
    bool                            changed = false;
};

/**
 * Plans one new authored member's wrapper without modifying a file or live seat.
 * A pod ID selects pods[].members[]; absence selects the flat nodes[] format.
 * The caller supplies the complete stable, nonsecret wrapper prefix and retains
 * responsibility for upstream spec validation and preserving the spec directory.
 * Package-custody paths and session authority must never enter this prefix.
 * This caller-owned prefix requires one terminal -- argument after the executable
 * and no earlier -- arguments. That delimiter identifies the complete prefix on
 * reapplication; it is not a restriction on OpenRig's existing wrapper grammar.
 * Only launch_wrapper changes; an existing wrapper remains its literal suffix.
 * Reapplying the same prefix is a no-op. A different prefix starting with the
 * same wrapper executable is refused so a changed installation is not stacked.
 */
std::optional<Prepared_member> prepare_authored_member(
    const logonomic::cli_addon::Json_value& specification,
    const Authored_member_selector&         selector,
    std::span<const std::string>             stable_wrapper_argv,
    std::string*                            out_error);

/** Public routing evidence only. The host must authenticate and bind it. */
struct Native_seat_identity
{
    std::string instance_home;
    std::string instance_database;
    std::string node_id;
    std::string session_name;
    std::string runtime;
    std::string occupant_generation;

    bool operator==(const Native_seat_identity&) const = default;
};

/**
 * Reads the native launch's public identity, retaining every field byte-for-byte.
 * The generation is an opaque upstream token, not a counter or a conversation ID.
 * Missing or ambiguous identity is unavailable; credentials are never copied.
 * This does not check a daemon, grant authority, or infer a seat from activity.
 */
std::optional<Native_seat_identity> native_seat_identity(
    const logonomic::cli_addon::Environment& environment,
    std::string*                            out_error);

} // namespace openrig
