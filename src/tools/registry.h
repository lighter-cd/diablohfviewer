// src/tools/registry.h
//
// The MCP "tool" abstraction. Every capability the server exposes is a named,
// parameterized C function that returns a JSON result. Both transports (stdio
// JSON-RPC and the WebSocket bridge) dispatch through a single ToolRegistry,
// so they share identical behavior.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace mcp {

using json = nlohmann::json;

// Result of running a tool. `ok` distinguishes success from a handled error
// (e.g. file not found). On failure, `value` carries an error detail object.
struct ToolResult {
    bool ok = true;
    json value;
};

using ToolHandler = std::function<ToolResult(const json& params)>;

struct ToolDef {
    std::string  name;          // e.g. "mpq.list"
    std::string  description;   // natural-language help for agents
    json         input_schema;  // JSON Schema (object) for the parameters
    ToolHandler  handler;
};

class ToolRegistry {
public:
    // Register a tool. Duplicate names are ignored (first wins).
    void registerTool(ToolDef def);

    // Dispatch by name. Unknown tool OR a thrown std::exception both become a
    // ToolResult with ok=false (the JSON-RPC layer turns that into an error).
    ToolResult dispatch(const std::string& name, const json& params) const;

    // True if a tool with this name is registered.
    bool has(const std::string& name) const;

    // MCP `tools/list` payload: an array of
    //   { name, description, inputSchema }
    json list() const;

    size_t size() const { return tools_.size(); }

private:
    std::vector<ToolDef> tools_;
};

} // namespace mcp
