#include "feathercast/core/automation.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace feathercast::core {
namespace {
bool matches_condition(const Condition& condition, const AutomationSnapshot& s) {
    bool result = false;
    switch (condition.type) {
    case ConditionType::Always: result = true; break;
    case ConditionType::ProcessRunning: result = s.processes.contains(condition.text); break;
    case ConditionType::ForegroundProcess: result = s.foreground_process == condition.text; break;
    case ConditionType::WindowTitleContains: result = s.window_title.find(condition.text) != std::string::npos; break;
    case ConditionType::Streaming: result = s.streaming; break;
    case ConditionType::Recording: result = s.recording; break;
    case ConditionType::ReplayRunning: result = s.replay_running; break;
    case ConditionType::AudioAboveDb: result = s.audio_peak_db > condition.number; break;
    case ConditionType::ElapsedAtLeastMs: result = static_cast<double>(s.elapsed_ms) >= condition.number; break;
    case ConditionType::Hotkey: result = s.hotkeys.contains(condition.text); break;
    case ConditionType::ExternalFlag: {
        auto found = s.external_flags.find(condition.text);
        result = found != s.external_flags.end() && found->second;
        break;
    }
    }
    return condition.negate ? !result : result;
}
bool is_redundant(const Action& a, const AutomationSnapshot& s) {
    switch (a.type) {
    case ActionType::SwitchScene: return a.argument == s.current_scene;
    case ActionType::StartStreaming: return s.streaming;
    case ActionType::StopStreaming: return !s.streaming;
    case ActionType::StartRecording: return s.recording;
    case ActionType::StopRecording: return !s.recording;
    case ActionType::StartReplay: return s.replay_running;
    case ActionType::StopReplay: return !s.replay_running;
    default: return false;
    }
}
}

bool AutomationEngine::set_rules(std::vector<Rule> rules, std::string* error) {
    const auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (rules.size() > 1024) return fail("At most 1024 rules are supported");
    std::unordered_set<std::string> ids;
    for (const auto& r : rules) {
        if (r.id.empty() || r.id.size() > 256 || !ids.insert(r.id).second)
            return fail("Rule IDs must be nonempty, unique, and at most 256 bytes");
        if (r.debounce_ms < 0 || r.cooldown_ms < 0 ||
            r.debounce_ms > 86'400'000 || r.cooldown_ms > 86'400'000)
            return fail("Debounce and cooldown must be within one day");
        if (r.repeat_while_matching && r.cooldown_ms == 0)
            return fail("Repeated rules require a positive cooldown");
        if (r.any_of.size() > 64 || r.actions.size() > 64)
            return fail("Too many condition groups or actions");
        for (const auto& group : r.any_of) {
            if (group.size() > 64) return fail("Too many conditions in group");
            for (const auto& c : group) {
                if (c.text.size() > 4096 || !std::isfinite(c.number) ||
                    static_cast<unsigned>(c.type) > static_cast<unsigned>(ConditionType::ExternalFlag))
                    return fail("Invalid condition");
            }
        }
        for (const auto& a : r.actions) {
            if (a.argument.size() > 4096 ||
                static_cast<unsigned>(a.type) > static_cast<unsigned>(ActionType::Log) ||
                (a.type == ActionType::SetDelayMs && (a.value < 0 || a.value > 86'400'000)))
                return fail("Invalid action");
        }
    }
    // Lexical ID is a stable tie-breaker independent of import order.
    std::sort(rules.begin(), rules.end(), [](const Rule& a, const Rule& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.id < b.id;
    });
    rules_ = std::move(rules);
    reset_runtime();
    if (error) error->clear();
    return true;
}

bool AutomationEngine::matches(const Rule& r, const AutomationSnapshot& s) {
    return r.enabled && std::any_of(r.any_of.begin(), r.any_of.end(), [&](const auto& group) {
        return std::all_of(group.begin(), group.end(), [&](const auto& c) { return matches_condition(c, s); });
    });
}

AutomationDecision AutomationEngine::evaluate(const AutomationSnapshot& snapshot, std::int64_t now_ms) {
    AutomationDecision result;
    if (now_ms < 0 || now_ms < last_evaluation_) return result;
    last_evaluation_ = now_ms;
    const Rule* winner = nullptr;
    bool any_matching = false;
    for (const auto& rule : rules_) {
        auto& runtime = runtime_[rule.id];
        if (!matches(rule, snapshot)) { runtime.match_since.reset(); continue; }
        any_matching = true;
        if (!runtime.match_since) runtime.match_since = now_ms;
        if (!winner && now_ms - *runtime.match_since >= rule.debounce_ms) winner = &rule;
    }
    if (now_ms < override_until_) { result.manual_override = true; return result; }
    if (!winner) {
        // Do not fight a pending debounce with a fallback scene.
        if (!any_matching && !fallback_scene_.empty() && snapshot.current_scene != fallback_scene_ && winner_ != "@fallback") {
            winner_ = "@fallback";
            result.rule_id = "@fallback";
            result.actions.push_back({ActionType::SwitchScene, fallback_scene_, 0});
        } else if (any_matching || fallback_scene_.empty() || snapshot.current_scene == fallback_scene_) winner_.clear();
        return result;
    }
    result.rule_id = winner->id;
    auto& runtime = runtime_.at(winner->id);
    if (runtime.last_fired && now_ms - *runtime.last_fired < winner->cooldown_ms) {
        result.cooldown_blocked = true;
        return result; // It still owns priority; lower rules cannot oscillate.
    }
    if (winner_ == winner->id && !winner->repeat_while_matching) return result;
    winner_ = winner->id;
    runtime.last_fired = now_ms;
    // Advance snapshot for sequential actions: Start then Stop is not filtered
    // against the stale input state. The executor reports actual failures.
    auto projected = snapshot;
    for (const auto& action : winner->actions) {
        if (is_redundant(action, projected)) continue;
        result.actions.push_back(action);
        switch (action.type) {
        case ActionType::SwitchScene: projected.current_scene = action.argument; break;
        case ActionType::StartStreaming: projected.streaming = true; break;
        case ActionType::StopStreaming: projected.streaming = false; break;
        case ActionType::StartRecording: projected.recording = true; break;
        case ActionType::StopRecording: projected.recording = false; break;
        case ActionType::StartReplay: projected.replay_running = true; break;
        case ActionType::StopReplay: projected.replay_running = false; break;
        default: break;
        }
    }
    return result;
}

void AutomationEngine::set_manual_override_until(std::int64_t until_ms) {
    override_until_ = std::max<std::int64_t>(0, until_ms);
    winner_.clear();
}
void AutomationEngine::clear_manual_override() { override_until_ = 0; winner_.clear(); }
void AutomationEngine::set_fallback_scene(std::string scene) { fallback_scene_ = std::move(scene); }
void AutomationEngine::reset_runtime() {
    runtime_.clear();
    winner_.clear();
    last_evaluation_ = -1;
    // Rule edits do not silently cancel the user's manual override.
}
} // namespace feathercast::core
