#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/launcher_discovery.h>

namespace openrig {

std::vector<logonomic::cli_addon::Launcher_candidate> discover_wsl(
    const logonomic::cli_addon::Discover_request& request, logonomic::cli_addon::Invocation_context& context);

bool recognize_wsl(const logonomic::cli_addon::Recognize_request& request);

} // namespace openrig
