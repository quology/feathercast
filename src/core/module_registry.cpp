#include "feathercast/core/module_registry.hpp"
#include <algorithm>
#include <utility>

namespace feathercast::core {
namespace {
bool call(const std::function<bool()>& f) {
    try { return !f || f(); } catch (...) { return false; }
}
bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}
}
bool ModuleRegistry::add(ModuleDefinition definition, std::string* error) {
    if (definition.id.empty() || modules_.contains(definition.id)) return fail(error, "Invalid or duplicate module ID");
    for (const auto& dependency : definition.dependencies)
        if (!modules_.contains(dependency)) return fail(error, "Register dependencies first: " + dependency);
    const auto id = definition.id;
    modules_.emplace(id, Entry{std::move(definition), false});
    order_.push_back(id);
    if (error) error->clear();
    return true;
}
bool ModuleRegistry::enable_impl(const std::string& id, std::vector<std::string>& started, std::string& error) {
    auto& entry = modules_.at(id);
    if (entry.enabled) return true;
    for (const auto& dependency : entry.definition.dependencies)
        if (!enable_impl(dependency, started, error)) return false;
    if (!call(entry.definition.start)) { error = "Module start failed: " + id; return false; }
    entry.enabled = true;
    started.push_back(id);
    return true;
}
bool ModuleRegistry::enable(const std::string& id, std::string* error) {
    if (!modules_.contains(id)) return fail(error, "Unknown module: " + id);
    std::vector<std::string> started;
    std::string message;
    if (!enable_impl(id, started, message)) {
        for (auto it = started.rbegin(); it != started.rend(); ++it) {
            auto& entry = modules_.at(*it);
            if (!has_enabled_dependent(*it) && call(entry.definition.stop)) entry.enabled = false;
            else message += "; rollback left active: " + *it;
        }
        return fail(error, message);
    }
    if (error) error->clear();
    return true;
}
bool ModuleRegistry::has_enabled_dependent(const std::string& id) const {
    for (const auto& [unused, entry] : modules_) {
        const auto& deps = entry.definition.dependencies;
        if (entry.enabled && std::find(deps.begin(), deps.end(), id) != deps.end()) return true;
    }
    return false;
}
bool ModuleRegistry::disable(const std::string& id, std::string* error) {
    auto found = modules_.find(id);
    if (found == modules_.end()) return fail(error, "Unknown module: " + id);
    auto& entry = found->second;
    if (!entry.definition.optional) return fail(error, "Required module: " + id);
    if (has_enabled_dependent(id)) return fail(error, "An enabled module depends on: " + id);
    if (entry.enabled && !call(entry.definition.stop)) return fail(error, "Module stop failed: " + id);
    entry.enabled = false;
    if (error) error->clear();
    return true;
}
bool ModuleRegistry::shutdown(std::string* error) {
    std::string failures;
    for (auto it = order_.rbegin(); it != order_.rend(); ++it) {
        auto& entry = modules_.at(*it);
        if (!entry.enabled) continue;
        if (has_enabled_dependent(*it) || !call(entry.definition.stop)) {
            if (!failures.empty()) failures += "; ";
            failures += "Module remains active: " + *it;
        } else entry.enabled = false;
    }
    if (error) *error = failures;
    return failures.empty();
}
bool ModuleRegistry::enabled(const std::string& id) const {
    const auto found = modules_.find(id);
    return found != modules_.end() && found->second.enabled;
}
std::vector<ModuleStatus> ModuleRegistry::status() const {
    std::vector<ModuleStatus> result;
    for (const auto& id : order_) {
        const auto& entry = modules_.at(id);
        result.push_back({id, entry.enabled, entry.definition.optional});
    }
    return result;
}
} // namespace feathercast::core
