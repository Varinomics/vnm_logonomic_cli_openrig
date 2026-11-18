#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>
#include <string_view>

namespace openrig {

bool openrig_help(std::string_view help);
void discover(logonomic::cli_addon::Invocation_context& context);
void recognize(logonomic::cli_addon::Invocation_context& context);
void prepare(logonomic::cli_addon::Invocation_context& context);

} // namespace openrig
