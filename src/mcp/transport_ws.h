// src/mcp/transport_ws.h
//
// WebSocket bridge for browser frontends. Browsers cannot spawn a subprocess
// and talk stdio, so the MCP server also listens on a local WebSocket and
// reuses the exact same JSON-RPC dispatch as the stdio transport.
//
// This is a self-contained, dependency-free RFC6455 server (handshake +
// frame codec over winsock) so the project stays 100% Node-free with no extra
// vendored libraries. It supports text frames, ping/pong, and close, which is
// everything the JSON-RPC contract needs.

#pragma once

#include "tools/registry.h"

namespace mcp {

// Run a WebSocket server bound to 127.0.0.1:<port>. Blocks, accepting clients
// (each served on its own thread). Shares handleRequest() with the stdio path.
void runWs(const ToolRegistry& reg, int port);

} // namespace mcp
