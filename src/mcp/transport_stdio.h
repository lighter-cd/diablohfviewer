// src/mcp/transport_stdio.h
//
// stdio transport: reads newline-delimited JSON-RPC requests from stdin,
// dispatches, writes responses to stdout. Logs go to stderr ONLY so they never
// corrupt the protocol stream.

#pragma once

#include "tools/registry.h"

namespace mcp {

// Run the stdio loop until stdin EOF. Each input line is one JSON-RPC request.
void runStdio(const ToolRegistry& reg);

} // namespace mcp
