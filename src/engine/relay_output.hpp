#pragma once
#include "feathercast/core/delay_buffer.hpp"
#include <cstdint>
struct obs_output;
namespace feathercast::engine {
struct RelayStats {
    core::DelayStats buffer;
    std::uint64_t bytes{};
    std::uint64_t reconnects{};
    int last_error{};
    bool connected{};
};
void registerRelayOutput();
bool setRelayDelay(obs_output*, std::int64_t microseconds);
RelayStats relayStats(obs_output*);
}
