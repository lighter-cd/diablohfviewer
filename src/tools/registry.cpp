// src/tools/registry.cpp

#include "tools/registry.h"

#include <stdexcept>

namespace mcp {

void ToolRegistry::registerTool(ToolDef def) {
    if (has(def.name))
        return;   // first registration wins; ignore duplicates
    tools_.push_back(std::move(def));
}

bool ToolRegistry::has(const std::string& name) const {
    for (const auto& t : tools_)
        if (t.name == name)
            return true;
    return false;
}

ToolResult ToolRegistry::dispatch(const std::string& name, const json& params) const {
    const ToolDef* found = nullptr;
    for (const auto& t : tools_)
        if (t.name == name) { found = &t; break; }

    if (!found) {
        ToolResult r;
        r.ok = false;
        r.value = json{{"error", "unknown tool"}, {"tool", name}};
        return r;
    }

    try {
        return found->handler(params);
    } catch (const std::exception& ex) {
        ToolResult r;
        r.ok = false;
        r.value = json{{"error", ex.what()}};
        return r;
    } catch (...) {
        ToolResult r;
        r.ok = false;
        r.value = json{{"error", "unknown exception in tool handler"}};
        return r;
    }
}

json ToolRegistry::list() const {
    json arr = json::array();
    for (const auto& t : tools_) {
        arr.push_back(json{
            {"name",        t.name},
            {"description", t.description},
            {"inputSchema", t.input_schema},
        });
    }
    return arr;
}

} // namespace mcp
