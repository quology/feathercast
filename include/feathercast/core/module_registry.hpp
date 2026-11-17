#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace feathercast::core {
struct ModuleDefinition {
    std::string id;
    std::vector<std::string> dependencies;
    bool optional{true};
    // start failure must release partially acquired resources; stop failure
    // leaves the module active. Callbacks must not reenter this registry.
    std::function<bool()> start;
    std::function<bool()> stop;
};
struct ModuleStatus { std::string id; bool enabled{}; bool optional{}; };

// Controller-thread lifecycle registry. Dependencies must be registered first,
// which makes cycles impossible. No UI or DLL unloading is performed here.
class ModuleRegistry {
public:
    bool add(ModuleDefinition definition, std::string* error = nullptr);
    bool enable(const std::string& id, std::string* error = nullptr);
    bool disable(const std::string& id, std::string* error = nullptr);
    // Shutdown also stops required modules, in reverse registration order.
    bool shutdown(std::string* error = nullptr);
    bool enabled(const std::string& id) const;
    std::vector<ModuleStatus> status() const;
private:
    struct Entry { ModuleDefinition definition; bool enabled{}; };
    bool enable_impl(const std::string&, std::vector<std::string>& started, std::string& error);
    bool has_enabled_dependent(const std::string&) const;
    std::map<std::string, Entry> modules_;
    std::vector<std::string> order_;
};
} // namespace feathercast::core
