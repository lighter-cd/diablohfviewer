// src/mcp/jsonrpc.h
//
// JSON-RPC 2.0 dispatch for the MCP server. Both the stdio transport and the
// WebSocket bridge feed parsed JSON requests into handleRequest() and send the
// returned response back. The protocol surface is intentionally tiny:
//
//   request  -> { jsonrpc:"2.0", id, method:"tools/list" }
//             -> { jsonrpc:"2.0", id, method:"tools/call",
//                   params:{ name, arguments } }
//   response -> { jsonrpc:"2.0", id, result } | { ..., error }
//
// (We implement the subset of MCP that this project needs; the method names
// match the MCP spec so any compliant client works.)

#pragma once

#include <optional>
#include <string>

#include "nlohmann/json.hpp"
#include "tools/registry.h"

namespace mcp {

using json = nlohmann::json;

// Handle one JSON-RPC request object. Returns the response object, or
// std::nullopt if no response should be sent (e.g. a parse error already
// handled at the transport layer). `id` is echoed from the request.
std::optional<json> handleRequest(const ToolRegistry& reg, const json& request);

} // namespace mcp
