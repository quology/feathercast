#include "feathercast/core/plugin_api.h"
#include <new>

namespace {
struct State { fc_host_api host; };
int FC_CALL initialize(const fc_host_api* host, void** context) {
    if (!context) return 0;
    *context = nullptr;
    if (!host || host->struct_size < sizeof(fc_host_api) ||
        host->abi_version != FC_PLUGIN_ABI_VERSION || !host->request_delay_ms) return 0;
    auto* state = new (std::nothrow) State{*host};
    if (!state) return 0;
    *context = state;
    if (host->log) host->log(host->context, 1, "Tournament delay plugin loaded");
    return 1;
}
void FC_CALL shutdown(void* context) { delete static_cast<State*>(context); }
int FC_CALL tournament_delay(void* context, const char*) {
    if (!context) return 0;
    auto& state = *static_cast<State*>(context);
    return state.host.request_delay_ms(state.host.context, 90'000);
}
const fc_action_descriptor actions[] = {
    {sizeof(fc_action_descriptor), "tournament.delay90", "Set tournament delay to 90 seconds", tournament_delay}
};
const fc_plugin_descriptor descriptor = {
    sizeof(fc_plugin_descriptor), FC_PLUGIN_ABI_VERSION, "org.feathercast.tournament",
    "Tournament delay", "1.0.0", initialize, shutdown, 1, actions
};
}
extern "C" FC_PLUGIN_EXPORT const fc_plugin_descriptor* FC_CALL fc_plugin_query(uint32_t host_abi) {
    return host_abi == FC_PLUGIN_ABI_VERSION ? &descriptor : nullptr;
}
