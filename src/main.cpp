// src/main.cpp
//
// Entry point for the Node-free C/C++ MCP server. It builds the single
// ToolRegistry (every capability the project exposes), then serves it over one
// or both transports:
//
//   mcp-server --stdio            # JSON-RPC 2.0 over stdin/stdout (AI agents)
//   mcp-server --ws <port>        # WebSocket bridge (TS/Canvas/WebGL frontend)
//   mcp-server --stdio --ws <port># both (handy for development)
//
// Logs go to stderr; stdout carries only the stdio JSON-RPC stream.

#include "mcp/transport_stdio.h"
#include "mcp/transport_ws.h"
#include "tools/registry.h"
#include "tools/mpq_tools.h"
#include "tools/sprite_tools.h"
#include "tools/map_tools.h"
#include "tools/scene_tools.h"

#include <string>
#include <thread>
#include <vector>

namespace {

void printUsage() {
    fprintf(stderr,
        "usage:\n"
        "  mcp-server --stdio                 serve JSON-RPC over stdin/stdout\n"
        "  mcp-server --ws <port>             serve WebSocket bridge on port\n"
        "  mcp-server --stdio --ws <port>     serve both\n");
}

} // namespace

int main(int argc, char** argv) {
    bool useStdio = false;
    bool useWs    = false;
    int  wsPort   = 18765;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--stdio") {
            useStdio = true;
        } else if (a == "--ws") {
            useWs = true;
            if (i + 1 < argc) {
                try { wsPort = std::stoi(argv[++i]); } catch (...) { }
            }
        } else if (a == "-h" || a == "--help") {
            printUsage();
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", a.c_str());
            printUsage();
            return 1;
        }
    }

    if (!useStdio && !useWs) {
        printUsage();
        return 1;
    }

    // Build the tool registry once; both transports share it.
    mcp::ToolRegistry reg;
    mcp::tools::registerMpqTools(reg);
    mcp::tools::registerSpriteTools(reg);
    mcp::tools::registerMapTools(reg);
    mcp::tools::registerSceneTools(reg);
    fprintf(stderr, "[main] registered %zu tool(s)\n", reg.size());

    if (useWs) {
        // Run WS on a background thread so --stdio can share the process.
        std::thread wsThread([&reg, wsPort]() { mcp::runWs(reg, wsPort); });
        if (useStdio) {
            mcp::runStdio(reg);     // blocks on stdin
            // stdio EOF: let the process exit; detach WS thread.
            return 0;
        } else {
            wsThread.join();        // blocks forever (local server)
            return 0;
        }
    }

    mcp::runStdio(reg);
    return 0;
}
