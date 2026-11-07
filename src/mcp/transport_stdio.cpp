// src/mcp/transport_stdio.cpp

#include "mcp/transport_stdio.h"
#include "mcp/jsonrpc.h"

#include <iostream>
#include <string>

namespace mcp {

void runStdio(const ToolRegistry& reg) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;

        json req;
        try {
            req = json::parse(line);
        } catch (...) {
            // Parse error: respond with id=null (per JSON-RPC 2.0).
            json err = json{
                {"jsonrpc", "2.0"},
                {"id",      nullptr},
                {"error",   {{"code", -32700}, {"message", "parse error"}}}};
            std::cout << err.dump() << "\n";
            std::cout.flush();
            continue;
        }

        auto resp = handleRequest(reg, req);
        if (resp) {
            std::cout << resp->dump() << "\n";
            std::cout.flush();
        }
    }
}

} // namespace mcp
