// src/mcp/jsonrpc.cpp

#include "mcp/jsonrpc.h"

#include <string>

namespace mcp {

namespace {

json errorResponse(const json& id, int code, const std::string& msg,
                   json data = nullptr) {
    json e = json{{"code", code}, {"message", msg}};
    if (data != nullptr)
        e["data"] = std::move(data);
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(e)}};
}

json okResponse(const json& id, json result) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

} // namespace

std::optional<json> handleRequest(const ToolRegistry& reg, const json& req) {
    if (!req.is_object())
        return errorResponse(nullptr, -32600, "invalid request object");

    const json id = req.value("id", json());
    const std::string method = req.value("method", std::string());
    json params = req.value("params", json::object());

    if (method.empty())
        return errorResponse(id, -32600, "missing 'method'");

    if (method == "tools/list") {
        return okResponse(id, json{{"tools", reg.list()}});
    }

    if (method == "tools/call") {
        if (!params.is_object())
            return errorResponse(id, -32602, "'params' must be an object");
        const std::string name = params.value("name", std::string());
        json args = params.value("arguments", json::object());
        if (name.empty())
            return errorResponse(id, -32602, "missing tool 'name' in params");
        if (!args.is_object())
            args = json::object();

        ToolResult tr = reg.dispatch(name, args);
        if (tr.ok)
            return okResponse(id, std::move(tr.value));
        return errorResponse(id, -32000, "tool execution failed", std::move(tr.value));
    }

    return errorResponse(id, -32601, "method not found: " + method);
}

} // namespace mcp
