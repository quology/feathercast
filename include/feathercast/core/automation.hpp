#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace feathercast::core {

enum class ConditionType { Always, ProcessRunning, ForegroundProcess,
    WindowTitleContains, Streaming, Recording, ReplayRunning,
    AudioAboveDb, ElapsedAtLeastMs, Hotkey, ExternalFlag };
struct Condition {
    ConditionType type{ConditionType::Always};
    std::string text;
    double number{};
    bool negate{};
};
// OR between groups, AND within each group. Empty group is true; no groups false.
using ConditionGroup = std::vector<Condition>;

enum class ActionType { SwitchScene, StartStreaming, StopStreaming, StartRecording,
    StopRecording, StartReplay, StopReplay, SaveReplay, SetDelayMs, PluginAction, Log };
struct Action {
    ActionType type{ActionType::SwitchScene};
    std::string argument;
    std::int64_t value{};
};
struct Rule {
    std::string id;
    int priority{};
    bool enabled{true};
    std::vector<ConditionGroup> any_of;
    std::vector<Action> actions;
    std::int64_t debounce_ms{250};
    std::int64_t cooldown_ms{1000};
    // Default triggers on becoming the winner; persistent matches do not loop.
    bool repeat_while_matching{false};
};
struct AutomationSnapshot {
    // Normalize executable basenames to ASCII lowercase in the platform adapter.
    std::unordered_set<std::string> processes;
    std::string foreground_process;
    std::string window_title;
    std::string current_scene;
    std::unordered_set<std::string> hotkeys;
    std::unordered_map<std::string, bool> external_flags;
    bool streaming{};
    bool recording{};
    bool replay_running{};
    double audio_peak_db{-100.0};
    std::int64_t elapsed_ms{};
};
struct AutomationDecision {
    std::string rule_id;
    std::vector<Action> actions;
    bool manual_override{};
    bool cooldown_blocked{};
};

// Single-thread owned (normally GUI/controller). Snapshot acquisition is outside
// this class; decisions contain values, never callbacks or recursively run rules.
class AutomationEngine {
public:
    // Transactional validation; failed import keeps existing rules and state.
    bool set_rules(std::vector<Rule> rules, std::string* error = nullptr);
    const std::vector<Rule>& rules() const { return rules_; }
    static bool matches(const Rule&, const AutomationSnapshot&);
    AutomationDecision evaluate(const AutomationSnapshot&, std::int64_t now_ms);
    void set_manual_override_until(std::int64_t until_ms);
    void clear_manual_override();
    void set_fallback_scene(std::string scene);
    void reset_runtime();
private:
    struct Runtime {
        std::optional<std::int64_t> match_since;
        std::optional<std::int64_t> last_fired;
    };
    std::vector<Rule> rules_;
    std::unordered_map<std::string, Runtime> runtime_;
    std::string winner_;
    std::string fallback_scene_;
    std::int64_t override_until_{};
    std::int64_t last_evaluation_{-1};
};
} // namespace feathercast::core
