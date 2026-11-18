#include "openrig_launcher.h"

#include <logonomic_cli_addon/adapter_runtime.h>
#include <logonomic_cli_addon/capability_ids.h>
#include <logonomic_cli_addon/launch_preparation.h>
#include <logonomic_cli_addon/launcher_discovery.h>

#include <utility>

namespace {

namespace addon = logonomic::cli_addon;

void invoke(addon::Invocation_context& context)
{
    const auto& request = context.request();
    if (request.capability.capability_id == addon::k_launcher_discovery_capability_id) {
        if (request.operation == addon::k_discover_operation) {
            openrig::discover(context);
        }
        else
        if (request.operation == addon::k_recognize_operation) {
            openrig::recognize(context);
        }
        else {
            context.reject_unknown_operation();
        }
        return;
    }
    if (request.capability.capability_id == addon::k_launch_preparation_capability_id &&
        request.operation == addon::k_prepare_operation)
    {
        openrig::prepare(context);
        return;
    }
    context.reject_unknown_operation();
}

} // namespace

int main()
{
    addon::Adapter_runtime_options options;
    options.adapter_name    = "openrig-addon";
    options.adapter_version = "0.1.0";
    options.capabilities = {
        {addon::k_presentation_capability_id,       addon::k_presentation_version},
        {addon::k_launcher_discovery_capability_id, addon::k_launcher_discovery_version},
        {addon::k_launch_preparation_capability_id, addon::k_launch_preparation_execution_version},
    };
    addon::Adapter_runtime runtime(std::move(options));
    runtime.set_invoke_handler(invoke);
    return runtime.run();
}
