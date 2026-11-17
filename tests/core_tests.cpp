#include "feathercast/core/automation.hpp"
#include "feathercast/core/delay_buffer.hpp"
#include "feathercast/core/module_registry.hpp"
#include "feathercast/core/plugin_api.h"

#include <atomic>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace feathercast::core;
namespace {
#define CHECK(condition) do { if (!(condition)) throw std::runtime_error(std::string(__func__) + ": " #condition); } while (0)
EncodedPacket video(Microseconds time, bool key = false, std::size_t bytes = 10) {
    return {MediaKind::Video, 0, time, time, 10, key, std::vector<std::uint8_t>(bytes, 0x55)};
}
EncodedPacket audio(Microseconds time, unsigned track = 1) {
    return {MediaKind::Audio, track, time, time, 10, false, std::vector<std::uint8_t>(3, 0x33)};
}
void delay_wait_and_increase() {
    DelayBuffer buffer;
    CHECK(buffer.set_target_delay(100));
    CHECK(!buffer.set_target_delay(-1));
    CHECK(!buffer.set_target_delay(400'000'000));
    CHECK(buffer.push(video(0), 1000) == PushResult::WaitingForKeyframe);
    CHECK(buffer.push(video(10, true), 1010) == PushResult::Accepted);
    CHECK(!buffer.pop_ready(1109));
    auto first = buffer.pop_ready(1110);
    CHECK(first && first->packet.keyframe && first->packet.dts_us == 0 && first->discontinuity);
    CHECK(buffer.push(video(20), 1020) == PushResult::Accepted);
    CHECK(buffer.set_target_delay(200));
    CHECK(!buffer.pop_ready(1219));
    auto second = buffer.pop_ready(1220);
    CHECK(second && second->packet.dts_us == 10 && !second->discontinuity);
    CHECK(buffer.stats().effective_delay_us == 200);
}
void delay_cut_audio_and_pacing() {
    DelayBuffer buffer;
    CHECK(buffer.set_target_delay(100));
    CHECK(buffer.push(video(0, true), 1000) == PushResult::Accepted);
    CHECK(buffer.push(audio(0), 1000) == PushResult::Accepted);
    CHECK(buffer.pop_ready(1100)->packet.dts_us == 0);
    CHECK(buffer.pop_ready(1100)->packet.dts_us == 0);
    for (int i = 10; i <= 90; i += 10) {
        CHECK(buffer.push(video(i, i == 50), 1000 + i) == PushResult::Accepted);
        CHECK(buffer.push(audio(i), 1000 + i) == PushResult::Accepted);
    }
    CHECK(buffer.set_target_delay(20));
    auto cut = buffer.pop_ready(1101);
    CHECK(cut && cut->packet.keyframe && cut->original_dts_us == 50 && cut->discontinuity);
    CHECK(cut->packet.dts_us == 10);
    auto cut_audio = buffer.pop_ready(1101);
    CHECK(cut_audio && cut_audio->packet.kind == MediaKind::Audio);
    CHECK(cut_audio->packet.pts_us == cut->packet.pts_us);
    CHECK(!buffer.pop_ready(1102)); // Backlog must be paced, not burst.
    CHECK(buffer.pop_ready(1111)->packet.dts_us == 20);
    CHECK(buffer.stats().dropped_packets == 8);
}
void delay_caps_and_decoder_recovery() {
    DelayBuffer buffer({30, 3, 100, 110});
    CHECK(buffer.push(video(0, true), 0) == PushResult::Accepted);
    CHECK(buffer.push(video(10), 10) == PushResult::Accepted);
    CHECK(buffer.push(video(20), 20) == PushResult::Accepted);
    CHECK(buffer.push(video(30), 30) == PushResult::WaitingForKeyframe);
    CHECK(buffer.stats().payload_bytes == 0 && buffer.stats().waiting_for_keyframe);
    CHECK(buffer.stats().overflow_events == 1);
    CHECK(buffer.push(video(40, true), 40) == PushResult::Accepted);
    CHECK(buffer.push(video(50, false, 31), 50) == PushResult::RejectedOversize);
    CHECK(buffer.stats().packets == 0);
    CHECK(buffer.push(video(60), 60) == PushResult::WaitingForKeyframe);
    CHECK(buffer.push(video(70, true), 70) == PushResult::Accepted);
    CHECK(buffer.pop_ready(70)->packet.keyframe);
    // Cap vector capacity too, so a small size cannot hide large allocation.
    auto reserved = video(80, true);
    reserved.data.reserve(100);
    CHECK(buffer.push(std::move(reserved), 80) == PushResult::RejectedOversize);
    DelayBuffer span({1000, 100, 100, 110});
    CHECK(span.push(video(0, true), 0) == PushResult::Accepted);
    CHECK(span.push(video(200, true), 200) == PushResult::AcceptedAfterOverflow);
    CHECK(span.stats().packets == 1);
}
void delay_invalid_and_reconnect() {
    DelayBuffer buffer;
    CHECK(buffer.push(video(100, true), 1000) == PushResult::Accepted);
    CHECK(buffer.pop_ready(1000));
    CHECK(buffer.push(video(110), 1010) == PushResult::Accepted);
    CHECK(buffer.push(video(120, true), 1020) == PushResult::Accepted);
    buffer.disconnect();
    CHECK(!buffer.pop_ready(1100));
    buffer.reconnect();
    auto first = buffer.pop_ready(1100);
    CHECK(first && first->original_dts_us == 120 && first->packet.dts_us == 0);
    CHECK(first->discontinuity && buffer.stats().dropped_packets == 1);
    CHECK(buffer.push(video(120), 1030) == PushResult::RejectedInvalid);
    CHECK(buffer.push(video(130), 1040) == PushResult::WaitingForKeyframe);
    auto bad = video(140, true); bad.track = 9;
    CHECK(buffer.push(std::move(bad), 1050) == PushResult::RejectedInvalid);
    CHECK(buffer.push(video(std::numeric_limits<Microseconds>::max(), true), 1060) == PushResult::RejectedInvalid);
    buffer.reset();
    CHECK(buffer.push(video(0, true), 0) == PushResult::Accepted);
    CHECK(buffer.pop_ready(0));
    CHECK(buffer.push(video(10, true), 10) == PushResult::Accepted);
    buffer.reconnect(ReconnectPolicy::DiscardBuffered);
    CHECK(buffer.stats().packets == 0 && buffer.stats().waiting_for_keyframe);
}
void delay_timestamp_property() {
    DelayBuffer buffer({5000, 200, 100, 150});
    std::array<Microseconds, 9> last;
    last.fill(-1);
    std::uint64_t emitted = 0;
    for (Microseconds tick = 0; tick < 10000; tick += 10) {
        if (tick % 250 == 0) CHECK(buffer.set_target_delay((tick / 250 % 3) * 40));
        CHECK(buffer.push(video(tick, tick % 50 == 0), tick) != PushResult::RejectedInvalid);
        CHECK(buffer.push(audio(tick), tick) != PushResult::RejectedInvalid);
        while (auto packet = buffer.pop_ready(tick)) {
            const auto track = packet->packet.track;
            CHECK(packet->packet.dts_us > last[track]);
            CHECK(packet->packet.pts_us >= packet->packet.dts_us);
            if (packet->discontinuity) CHECK(packet->packet.keyframe);
            last[track] = packet->packet.dts_us;
            ++emitted;
        }
        CHECK(buffer.stats().payload_bytes <= 5000);
        CHECK(buffer.stats().packets <= 200);
        CHECK(buffer.stats().queued_span_us <= 150);
    }
    CHECK(emitted > 100);
}
void delay_concurrent_access() {
    DelayBuffer buffer({4096, 64, 1000, 2000});
    std::atomic<bool> done = false;
    std::thread producer([&] {
        for (int i = 0; i < 20000; ++i) buffer.push(video(i * 10, i % 10 == 0), i * 10);
        done = true;
    });
    Microseconds now = 0;
    while (!done.load()) {
        buffer.set_target_delay(now % 1000);
        buffer.pop_ready(now++);
        const auto stats = buffer.stats();
        CHECK(stats.payload_bytes <= 4096 && stats.packets <= 64);
    }
    producer.join();
    CHECK(buffer.stats().packets <= 64);
}
void delay_timer_jitter_does_not_accumulate() {
    DelayBuffer buffer;
    for (int i = 0; i < 200; ++i) CHECK(buffer.push(video(i * 10000, i == 0), i * 10000) == PushResult::Accepted);
    int emitted = 0;
    for (Microseconds now = 0; now <= 2'000'000; now += 11000)
        while (buffer.pop_ready(now)) ++emitted;
    CHECK(emitted == 200); // Per-packet reanchoring would emit only 182.
}
Rule process_rule(std::string id, int priority, std::string process, std::string scene) {
    Rule rule;
    rule.id = std::move(id); rule.priority = priority;
    rule.any_of = {{Condition{ConditionType::ProcessRunning, std::move(process), 0, false}}};
    rule.actions = {{ActionType::SwitchScene, std::move(scene), 0}};
    rule.debounce_ms = 20; rule.cooldown_ms = 100;
    return rule;
}
void automation_priority_debounce_cooldown() {
    AutomationEngine engine;
    auto game = process_rule("game", 100, "game.exe", "Gameplay");
    auto chat = process_rule("chat", 40, "chat.exe", "Chat");
    game.actions.push_back({ActionType::SetDelayMs, {}, 90'000});
    CHECK(engine.set_rules({chat, game}));
    AutomationSnapshot snapshot;
    snapshot.processes = {"game.exe", "chat.exe"};
    CHECK(engine.evaluate(snapshot, 0).actions.empty());
    auto decision = engine.evaluate(snapshot, 20);
    CHECK(decision.rule_id == "game" && decision.actions.size() == 2);
    CHECK(engine.evaluate(snapshot, 21).cooldown_blocked);
    CHECK(engine.evaluate(snapshot, 120).actions.empty()); // No repeated loop.
    snapshot.processes.erase("game.exe");
    CHECK(engine.evaluate(snapshot, 130).rule_id == "chat");
    snapshot.processes.insert("game.exe");
    CHECK(engine.evaluate(snapshot, 140).actions.empty());
    CHECK(engine.evaluate(snapshot, 160).actions.size() == 2);
}
void automation_logic_override_fallback() {
    AutomationEngine engine;
    auto rule = process_rule("game", 100, "game.exe", "Gameplay");
    rule.debounce_ms = 0; rule.cooldown_ms = 0;
    rule.any_of[0].push_back({ConditionType::ForegroundProcess, "game.exe", 0, false});
    rule.any_of.push_back({Condition{ConditionType::Hotkey, "go", 0, false}});
    CHECK(engine.set_rules({rule}));
    engine.set_fallback_scene("Desktop");
    AutomationSnapshot snapshot; snapshot.processes.insert("game.exe");
    CHECK(engine.evaluate(snapshot, 0).rule_id == "@fallback");
    snapshot.foreground_process = "game.exe";
    engine.set_manual_override_until(100);
    CHECK(engine.evaluate(snapshot, 50).manual_override);
    CHECK(engine.evaluate(snapshot, 100).rule_id == "game");
    CHECK(engine.evaluate(snapshot, 101).actions.empty());
    snapshot.processes.clear();
    CHECK(engine.evaluate(snapshot, 102).rule_id == "@fallback");
    snapshot.hotkeys.insert("go");
    CHECK(engine.evaluate(snapshot, 103).rule_id == "game");
    auto invalid = rule; invalid.cooldown_ms = -1;
    CHECK(!engine.set_rules({invalid}));
    CHECK(engine.rules().size() == 1 && engine.rules()[0].cooldown_ms == 0);
    CHECK(!engine.set_rules({rule, rule}));
    invalid = rule; invalid.repeat_while_matching = true;
    CHECK(!engine.set_rules({invalid}));
    CHECK(engine.evaluate(snapshot, 99).actions.empty()); // Clock rollback ignored.
}
void automation_multiple_actions_and_tie() {
    AutomationEngine engine;
    auto a = process_rule("a", 10, "game", "A");
    auto b = process_rule("b", 10, "game", "B");
    a.debounce_ms = b.debounce_ms = 0;
    a.actions = {{ActionType::StartRecording, {}, 0}, {ActionType::StopRecording, {}, 0}};
    CHECK(engine.set_rules({b, a}));
    AutomationSnapshot snapshot; snapshot.processes.insert("game");
    const auto result = engine.evaluate(snapshot, 0);
    CHECK(result.rule_id == "a" && result.actions.size() == 2);
}
void modules_rollback_and_dependencies() {
    ModuleRegistry modules;
    int starts = 0, stops = 0;
    CHECK(modules.add({"core", {}, false, [&] { ++starts; return true; }, [&] { ++stops; return true; }}));
    CHECK(modules.add({"capture", {"core"}, true, [] { return true; }, [] { return true; }}));
    CHECK(modules.add({"failed", {"capture"}, true, [] { return false; }, {}}));
    std::string error;
    CHECK(!modules.enable("failed", &error));
    CHECK(starts == 1 && stops == 1 && !modules.enabled("capture"));
    CHECK(modules.enable("capture"));
    CHECK(!modules.disable("core"));
    CHECK(!modules.add({"cycle", {"cycle"}, true, {}, {}}));
    CHECK(modules.shutdown());
    CHECK(starts == 2 && stops == 2);
    CHECK(!modules.enabled("core"));
}
void modules_stop_failure_preserves_dependencies() {
    ModuleRegistry modules;
    bool can_stop = false;
    CHECK(modules.add({"core", {}, false, {}, {}}));
    CHECK(modules.add({"capture", {"core"}, true, {}, [&] { return can_stop; }}));
    CHECK(modules.enable("capture"));
    CHECK(!modules.shutdown());
    CHECK(modules.enabled("core") && modules.enabled("capture"));
    can_stop = true;
    CHECK(modules.shutdown());
}
void plugin_contract() {
    CHECK(fc_plugin_query(999) == nullptr);
    const auto* plugin = fc_plugin_query(FC_PLUGIN_ABI_VERSION);
    CHECK(plugin && plugin->struct_size == sizeof(fc_plugin_descriptor));
    std::int64_t requested = -1;
    fc_host_api host{sizeof(fc_host_api), FC_PLUGIN_ABI_VERSION, &requested, nullptr,
        [](void* context, int64_t value) -> int { *static_cast<std::int64_t*>(context) = value; return 1; }};
    void* state = nullptr;
    CHECK(plugin->initialize(&host, &state) == 1);
    CHECK(state && plugin->action_count == 1);
    CHECK(plugin->actions[0].invoke(state, "{}") == 1);
    CHECK(requested == 90'000);
    plugin->shutdown(state);
    host.abi_version = 999;
    CHECK(plugin->initialize(&host, &state) == 0 && state == nullptr);
}
}

int main() {
    const std::pair<const char*, std::function<void()>> tests[] = {
        {"delay wait/increase", delay_wait_and_increase},
        {"delay cut/audio/pacing", delay_cut_audio_and_pacing},
        {"delay caps/recovery", delay_caps_and_decoder_recovery},
        {"delay invalid/reconnect", delay_invalid_and_reconnect},
        {"delay timestamp property", delay_timestamp_property},
        {"delay concurrent access", delay_concurrent_access},
        {"delay timer jitter", delay_timer_jitter_does_not_accumulate},
        {"automation priority/debounce/cooldown", automation_priority_debounce_cooldown},
        {"automation logic/override/fallback", automation_logic_override_fallback},
        {"automation actions/tie", automation_multiple_actions_and_tie},
        {"module rollback/dependencies", modules_rollback_and_dependencies},
        {"module failed stop", modules_stop_failure_preserves_dependencies},
        {"plugin ABI", plugin_contract}
    };
    int failed = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << std::size(tests) - failed << '/' << std::size(tests) << " passed\n";
    return failed ? 1 : 0;
}
