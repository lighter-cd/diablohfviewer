// Unit tests for the MCP tool registry and the JSON-RPC layer.
//
// These are PURE tests: they exercise src/tools/registry.cpp and
// src/mcp/jsonrpc.cpp directly and need no MPQ archive or StormLib linkage.
//
// Build & run (MinGW-w64, from repo root):
//   g++ -I 3rdparty -I src -std=c++17 out/core_tests.cpp \
//       src/tools/registry.cpp src/mcp/jsonrpc.cpp \
//       -o out/core_tests.exe && out/core_tests.exe
//
// Exit code 0 = all pass, 1 = at least one failure.

#include "tools/registry.h"
#include "mcp/jsonrpc.h"

#include <cstdio>
#include <stdexcept>

using namespace mcp;

static bool allOk = true;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            allOk = false;                                                  \
            printf("  FAIL: %s\n", msg);                                    \
        }                                                                   \
    } while (0)

// ---------------------------------------------------------------------------
// registry: register / duplicate / dispatch / exceptions / list
// ---------------------------------------------------------------------------
static void testRegistry() {
    // Empty registry behavior.
    {
        ToolRegistry r;
        CHECK(r.size() == 0, "empty registry size == 0");
        CHECK(!r.has("x.y"), "empty registry has() == false");
        ToolResult d = r.dispatch("x.y", json::object());
        CHECK(!d.ok, "dispatch unknown tool -> ok == false");
        CHECK(d.value.value("error", "") == "unknown tool",
              "dispatch unknown tool error message");
    }

    // Register + dispatch a working tool; duplicate registration is ignored.
    {
        ToolRegistry r;
        r.registerTool(ToolDef{
            "t.add", "add two ints", json{{"type", "object"}},
            [](const json& p) -> ToolResult {
                ToolResult res;
                res.value = json{{"sum", p.at("a").get<int>() + p.at("b").get<int>()}};
                return res;
            }});
        CHECK(r.has("t.add"), "has() true after register");
        CHECK(r.size() == 1, "size() == 1 after register");

        // Duplicate name: must be ignored (first wins).
        r.registerTool(ToolDef{
            "t.add", "ignored duplicate", json{},
            [](const json&) -> ToolResult {
                ToolResult r;
                r.value = json{{"sum", -1}};
                return r;
            }});
        CHECK(r.size() == 1, "duplicate registration does not grow size");

        ToolResult d = r.dispatch("t.add", json{{"a", 2}, {"b", 3}});
        CHECK(d.ok, "dispatch ok for registered tool");
        CHECK(d.value.value("sum", 0) == 5, "tool returned expected sum");

        // A handler that throws must be caught into ok == false.
        r.registerTool(ToolDef{
            "t.boom", "throws", json{},
            [](const json&) -> ToolResult { throw std::invalid_argument("boom"); }});
        ToolResult d2 = r.dispatch("t.boom", json::object());
        CHECK(!d2.ok, "throwing handler -> ok == false");
        CHECK(!d2.value.value("error", "").empty(),
              "throwing handler error detail present");

        // list() payload shape.
        json lst = r.list();
        CHECK(lst.is_array(), "list() returns an array");
        CHECK(lst.size() == 2, "list() size matches registered tools");
        bool sawAdd = false;
        for (const auto& t : lst) {
            CHECK(t.contains("name") && t.contains("description") &&
                      t.contains("inputSchema"),
                  "list item has name/description/inputSchema");
            if (t.value("name", "") == "t.add") sawAdd = true;
        }
        CHECK(sawAdd, "list() includes the registered tool");
    }

    printf("registry: %s\n", allOk ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// jsonrpc: tools/list, tools/call, and every error code path
// ---------------------------------------------------------------------------
static void testJsonrpc() {
    ToolRegistry reg;
    reg.registerTool(ToolDef{
        "t.add", "add", json{{"type", "object"}},
        [](const json& p) -> ToolResult {
            ToolResult r;
            r.value = json{{"sum", p.at("a").get<int>() + p.at("b").get<int>()}};
            return r;
        }});
    reg.registerTool(ToolDef{
        "t.boom", "throws", json{},
        [](const json&) -> ToolResult { throw std::runtime_error("nope"); }});

    // tools/list
    auto rl = handleRequest(reg, json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}});
    CHECK(rl.has_value(), "tools/list returns a response");
    CHECK((*rl)["result"]["tools"].is_array(), "tools/list result.tools is array");
    CHECK((*rl)["result"]["tools"].size() == 2, "tools/list reports both tools");
    CHECK((*rl)["id"] == 1, "tools/list echoes id");

    // tools/call success
    auto rc = handleRequest(reg, json{
        {"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/call"},
        {"params", json{{"name", "t.add"}, {"arguments", json{{"a", 4}, {"b", 5}}}}}});
    CHECK(rc.has_value(), "tools/call returns a response");
    CHECK(!(*rc).contains("error"), "tools/call success has no error");
    CHECK((*rc)["result"]["sum"] == 9, "tools/call success result");
    CHECK((*rc)["id"] == 2, "tools/call echoes id");

    // tools/call unknown tool -> -32000
    auto ru = handleRequest(reg, json{
        {"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/call"},
        {"params", json{{"name", "t.nope"}, {"arguments", json{}}}}});
    CHECK(ru->value("error", json{}).value("code", 0) == -32000,
          "unknown tool -> -32000");

    // tools/call throwing handler -> -32000
    auto rb = handleRequest(reg, json{
        {"jsonrpc", "2.0"}, {"id", 4}, {"method", "tools/call"},
        {"params", json{{"name", "t.boom"}, {"arguments", json{}}}}});
    CHECK(rb->value("error", json{}).value("code", 0) == -32000,
          "throwing tool -> -32000");

    // missing 'method' -> -32600
    auto rm = handleRequest(reg, json{{"jsonrpc", "2.0"}, {"id", 5}});
    CHECK(rm->value("error", json{}).value("code", 0) == -32600,
          "missing method -> -32600");

    // unknown method -> -32601
    auto rn = handleRequest(reg, json{{"jsonrpc", "2.0"}, {"id", 6}, {"method", "bogus"}});
    CHECK(rn->value("error", json{}).value("code", 0) == -32601,
          "unknown method -> -32601");

    // params not an object -> -32602
    auto rp = handleRequest(reg, json{
        {"jsonrpc", "2.0"}, {"id", 7}, {"method", "tools/call"},
        {"params", json::array({1, 2})}});
    CHECK(rp->value("error", json{}).value("code", 0) == -32602,
          "non-object params -> -32602");

    // tools/call missing tool name -> -32602
    auto rne = handleRequest(reg, json{
        {"jsonrpc", "2.0"}, {"id", 8}, {"method", "tools/call"},
        {"params", json{{"arguments", json{}}}}});
    CHECK(rne->value("error", json{}).value("code", 0) == -32602,
          "missing name -> -32602");

    // request is not an object -> -32600
    auto rx = handleRequest(reg, json{1});
    CHECK(rx->value("error", json{}).value("code", 0) == -32600,
          "non-object request -> -32600");

    printf("jsonrpc: %s\n", allOk ? "PASS" : "FAIL");
}

int main() {
    printf("=== DiabloHFConverter core tests (registry + jsonrpc) ===\n");
    testRegistry();
    testJsonrpc();
    printf("=== %s ===\n", allOk ? "ALL PASS" : "FAILURES PRESENT");
    return allOk ? 0 : 1;
}
