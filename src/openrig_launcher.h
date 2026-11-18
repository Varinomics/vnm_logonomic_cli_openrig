#pragma once

#include <logonomic_cli_addon/adapter_runtime.h>

namespace openrig {

void discover(logonomic::cli_addon::Invocation_context& context);
void recognize(logonomic::cli_addon::Invocation_context& context);
void prepare(logonomic::cli_addon::Invocation_context& context);

} // namespace openrig
