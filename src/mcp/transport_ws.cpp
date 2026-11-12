// src/mcp/transport_ws.cpp
//
// Minimal, dependency-free RFC6455 WebSocket server over winsock. Implements
// exactly what the local MCP bridge needs: the HTTP Upgrade handshake (with the
// mandated SHA-1 + base64 Sec-WebSocket-Accept), decoding of masked client
// frames (text / ping / close / continuation), and sending unmasked text
// frames. Every decoded text message is a JSON-RPC request handled by the same
// mcp::handleRequest used by the stdio transport.

#include "mcp/transport_ws.h"
#include "mcp/jsonrpc.h"
#include "tools/base64.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace mcp {
namespace {

// ---- SHA-1 (FIPS 180-1) ----------------------------------------------------
void sha1(const uint8_t* data, size_t len, uint8_t digest[20]) {
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE,
             h3 = 0x10325476, h4 = 0xC3D2E1F0;
    size_t newLen = ((len + 8) / 64 + 1) * 64;
    std::vector<uint8_t> msg(newLen, 0);
    if (len) std::memcpy(msg.data(), data, len);
    msg[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        msg[newLen - 1 - i] = (uint8_t)((bits >> (8 * i)) & 0xFF);

    for (size_t off = 0; off < newLen; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)msg[off + 4 * i] << 24) |
                   ((uint32_t)msg[off + 4 * i + 1] << 16) |
                   ((uint32_t)msg[off + 4 * i + 2] << 8) |
                   ((uint32_t)msg[off + 4 * i + 3]);
        }
        for (int i = 16; i < 80; i++) {
            uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | ((~b) & d);     k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                k = 0xCA62C1D6; }
            uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    uint32_t hs[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; i++) {
        digest[4 * i]     = (uint8_t)((hs[i] >> 24) & 0xFF);
        digest[4 * i + 1] = (uint8_t)((hs[i] >> 16) & 0xFF);
        digest[4 * i + 2] = (uint8_t)((hs[i] >> 8) & 0xFF);
        digest[4 * i + 3] = (uint8_t)(hs[i] & 0xFF);
    }
}

// ---- socket helpers --------------------------------------------------------
bool recvAll(SOCKET s, char* buf, int n) {
    int got = 0;
    while (got < n) {
        int r = recv(s, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

bool sendAll(SOCKET s, const char* buf, int n) {
    int sent = 0;
    while (sent < n) {
        int r = send(s, buf + sent, n - sent, 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

// Send a server->client text frame (never masked).
bool sendTextFrame(SOCKET s, const std::string& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81);   // FIN + text
    size_t len = payload.size();
    if (len < 126) {
        frame.push_back((uint8_t)len);
    } else if (len < 65536) {
        frame.push_back(126);
        frame.push_back((uint8_t)((len >> 8) & 0xFF));
        frame.push_back((uint8_t)(len & 0xFF));
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; i--)
            frame.push_back((uint8_t)((len >> (8 * i)) & 0xFF));
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return sendAll(s, (const char*)frame.data(), (int)frame.size());
}

// Read the request header block (up to the blank line) without committing to a
// protocol. The same listen socket serves BOTH the WebSocket upgrade and plain
// HTTP GETs for scene artifacts, so we must peek at the head first to decide.
bool readRequestHead(SOCKET s, std::string& head) {
    char c;
    while (true) {
        if (!recvAll(s, &c, 1)) return false;
        head.push_back(c);
        size_t n = head.size();
        if (n >= 4 && head[n - 4] == '\r' && head[n - 3] == '\n' &&
            head[n - 2] == '\r' && head[n - 1] == '\n')
            break;
        if (head.size() > 16 * 1024) return false;   // guard
    }
    return true;
}

void sendHttpStatus(SOCKET s, int code, const char* reason) {
    std::string body = std::to_string(code) + " " + reason;
    std::string resp =
        "HTTP/1.1 " + body + "\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
    sendAll(s, resp.data(), (int)resp.size());
}

// ---- static-file helpers ---------------------------------------------------
std::string toLower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// Minimal URL percent-decode (handles %XX; leaves everything else untouched).
std::string percentDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '%' && i + 2 < in.size()) {
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hexVal(in[i + 1]), lo = hexVal(in[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back((char)((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

// Canonicalize a URL path relative to a web root. Returns false if it would
// escape the root via "..". On success `out` holds the normalized relative path
// (no leading slash; empty string means the root).
bool canonicalRel(const std::string& in, std::string& out) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : in) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) {
                if (cur == "..") {
                    if (parts.empty()) return false;
                    parts.pop_back();
                } else if (cur != ".") {
                    parts.push_back(cur);
                }
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        if (cur == "..") {
            if (parts.empty()) return false;
            parts.pop_back();
        } else if (cur != ".") {
            parts.push_back(cur);
        }
    }
    out.clear();
    for (size_t i = 0; i < parts.size(); i++) {
        out += parts[i];
        if (i + 1 < parts.size()) out += '/';
    }
    return true;
}

// Choose a Content-Type from the file extension.
std::string mimeFor(const std::string& p) {
    size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return "application/octet-stream";
    std::string ext = toLower(p.substr(dot));
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".js" || ext == ".mjs")   return "text/javascript; charset=utf-8";
    if (ext == ".css")                    return "text/css; charset=utf-8";
    if (ext == ".json" || ext == ".map") return "application/json; charset=utf-8";
    if (ext == ".png")                    return "image/png";
    if (ext == ".svg")                    return "image/svg+xml";
    if (ext == ".ico")                    return "image/x-icon";
    if (ext == ".wasm")                   return "application/wasm";
    if (ext == ".txt")                    return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

// Send a small 200 response with a binary/utf-8 body read from disk.
bool sendFile(SOCKET s, const std::string& fsPath, const std::string& contentType,
              bool withBody) {
    std::ifstream f(fsPath, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> body((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    f.close();
    std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: " + contentType + "\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n";
    sendAll(s, header.data(), (int)header.size());
    if (withBody)
        sendAll(s, (const char*)body.data(), (int)body.size());
    return true;
}

// Serve a static file for a plain HTTP GET. Two namespaces share this port:
//   - /scenes/...  -> out/scenes/  (scene build artifacts, legacy mapping)
//   - everything else -> webRoot   (the browser frontend: index.html, *.js,
//                                    *.css, *.png, ...). Path traversal is
//                                    blocked by canonicalRel().
void serveHttp(SOCKET s, const std::string& head, const std::string& webRoot) {
    size_t sp1 = head.find(' ');
    if (sp1 == std::string::npos) { sendHttpStatus(s, 400, "Bad Request"); return; }
    std::string method = head.substr(0, sp1);
    size_t sp2 = head.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) { sendHttpStatus(s, 400, "Bad Request"); return; }
    std::string path = head.substr(sp1 + 1, sp2 - sp1 - 1);

    if (method != "GET" && method != "HEAD") {
        sendHttpStatus(s, 405, "Method Not Allowed");
        return;
    }

    // Drop any query string (?token=...).
    size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);

    // --- /scenes/ artifact namespace (unchanged behavior) ---
    const std::string scenesPrefix = "/scenes/";
    if (path.size() >= scenesPrefix.size() &&
        path.compare(0, scenesPrefix.size(), scenesPrefix) == 0) {
        std::string rel = path.substr(scenesPrefix.size());
        if (rel.empty() || rel.find("..") != std::string::npos ||
            rel.find('/') != std::string::npos || rel.find('\\') != std::string::npos) {
            sendHttpStatus(s, 403, "Forbidden");
            return;
        }
        std::string ct = "application/octet-stream";
        if (rel.size() >= 4 && rel.compare(rel.size() - 4, 4, ".png") == 0)
            ct = "image/png";
        else if (rel.size() >= 5 && rel.compare(rel.size() - 5, 5, ".json") == 0)
            ct = "application/json";
        bool ok = sendFile(s, "out/scenes/" + rel, ct, method == "GET");
        if (!ok) sendHttpStatus(s, 404, "Not Found");
        return;
    }

    // --- web frontend namespace (webRoot) ---
    if (path == "/" || path.empty()) path = "/index.html";
    if (!path.empty() && (path[0] == '/' || path[0] == '\\'))
        path = path.substr(1);

    std::string decoded = percentDecode(path);
    std::string norm;
    if (!canonicalRel(decoded, norm)) {
        sendHttpStatus(s, 403, "Forbidden");
        return;
    }

    std::string fsPath = webRoot;
    if (!fsPath.empty() && fsPath.back() != '/' && fsPath.back() != '\\')
        fsPath += '/';
    fsPath += norm;

    bool ok = sendFile(s, fsPath, mimeFor(fsPath), method == "GET");
    if (!ok) {
        // Treat as a directory request: try <path>/index.html.
        std::string idx = fsPath;
        if (!idx.empty() && idx.back() != '/' && idx.back() != '\\') idx += '/';
        idx += "index.html";
        ok = sendFile(s, idx, "text/html; charset=utf-8", method == "GET");
    }
    if (!ok) sendHttpStatus(s, 404, "Not Found");
}

// Perform the WS handshake. Returns true on success (101 Switching Protocols
// already sent). `head` is the request header read by readRequestHead(); we only
// need to extract Sec-WebSocket-Key and reply with the computed accept value.
bool doHandshake(SOCKET s, const std::string& head) {
    // Extract Sec-WebSocket-Key. Browsers send it as "Sec-WebSocket-Key:" with
    // mixed case, so search case-insensitively but copy the value from the
    // original header (the base64 key itself is case-sensitive).
    std::string key;
    std::string headLower = head;
    for (char& c : headLower)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    const std::string tag = "sec-websocket-key:";
    size_t p = headLower.find(tag);
    if (p == std::string::npos) return false;
    p += tag.size();
    while (p < head.size() && (head[p] == ' ' || head[p] == '\t')) p++;
    while (p < head.size() && head[p] != '\r' && head[p] != '\n')
        key.push_back(head[p++]);

    // accept = base64(sha1(key + GUID))
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string concat = key + GUID;
    uint8_t d[20];
    sha1((const uint8_t*)concat.data(), concat.size(), d);
    std::string accept = mcp::base64::encode(d, 20);

    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";
    return sendAll(s, resp.data(), (int)resp.size());
}

// Decode one complete message from the client (handles continuation frames)
// and return its text payload. Returns false on connection close / error.
bool readMessage(SOCKET s, std::string& outText, bool& isText) {
    outText.clear();
    isText = true;
    std::vector<uint8_t> frag;     // accumulating payload for fragmentation
    bool fragIsText = true;

    while (true) {
        uint8_t hdr[2];
        if (!recvAll(s, (char*)hdr, 2)) return false;

        bool fin = (hdr[0] & 0x80) != 0;
        uint8_t opcode = hdr[0] & 0x0F;
        bool masked = (hdr[1] & 0x80) != 0;
        uint64_t len = hdr[1] & 0x7F;

        if (len == 126) {
            uint8_t ext[2];
            if (!recvAll(s, (char*)ext, 2)) return false;
            len = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (len == 127) {
            uint8_t ext[8];
            if (!recvAll(s, (char*)ext, 8)) return false;
            len = 0;
            for (int i = 0; i < 8; i++)
                len = (len << 8) | ext[i];
        }

        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked) {
            if (!recvAll(s, (char*)mask, 4)) return false;
        }

        std::vector<uint8_t> payload((size_t)len);
        if (len > 0 && !recvAll(s, (char*)payload.data(), (int)len))
            return false;

        if (masked) {
            for (uint64_t i = 0; i < len; i++)
                payload[i] ^= mask[i & 3];
        }

        if (opcode == 0x8) {        // close
            uint8_t closeFrame[2] = {0x88, 0x00};   // FIN + close, len 0
            sendAll(s, (const char*)closeFrame, 2);
            return false;
        }
        if (opcode == 0x9) {        // ping -> pong (echo payload)
            std::vector<uint8_t> pong;
            pong.push_back(0x8A);   // FIN + pong
            size_t pl = payload.size();
            if (pl < 126) pong.push_back((uint8_t)pl);
            else { pong.push_back(126); pong.push_back((uint8_t)((pl>>8)&0xFF)); pong.push_back((uint8_t)(pl&0xFF)); }
            pong.insert(pong.end(), payload.begin(), payload.end());
            sendAll(s, (const char*)pong.data(), (int)pong.size());
            continue;               // keep reading next frame
        }
        if (opcode == 0xA)          // pong (ignore)
            continue;

        // 0x1 text, 0x2 binary, 0x0 continuation
        if (opcode == 0x1) fragIsText = true;
        else if (opcode == 0x2) fragIsText = false;
        frag.insert(frag.end(), payload.begin(), payload.end());

        if (fin) {
            isText = fragIsText;
            outText.assign(frag.begin(), frag.end());
            return true;
        }
    }
}

void handleClient(SOCKET s, const ToolRegistry* reg, const std::string& webRoot) {
    // Read the request head once, then decide: a plain HTTP GET (no WebSocket
    // Upgrade) is served as a static file; an Upgrade request proceeds to the WS
    // handshake.
    std::string head;
    if (!readRequestHead(s, head)) { closesocket(s); return; }

    bool isWs = false;
    {
        std::string hl = head;
        for (char& c : hl) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        isWs = hl.find("upgrade: websocket") != std::string::npos;
    }
    if (!isWs) {
        serveHttp(s, head, webRoot);
        closesocket(s);
        return;
    }

    if (!doHandshake(s, head)) { closesocket(s); return; }
    // Progress sink: the dispatched tool may emit incremental progress, which we
    // forward to the client as JSON-RPC notifications (no id) carrying
    // {pct, stage}. The browser WS delivers them as separate onmessage events
    // interleaved with the final response.
    auto progress = [&](int pct, const std::string& stage) {
        json note = json{
            {"jsonrpc", "2.0"},
            {"method", "$/progress"},
            {"params", {{"pct", pct}, {"stage", stage}}}};
        sendTextFrame(s, note.dump());
    };
    std::string msg;
    bool isText;
    while (readMessage(s, msg, isText)) {
        json req;
        try {
            req = json::parse(msg);
        } catch (...) {
            sendTextFrame(s, json{{"jsonrpc","2.0"},{"id",nullptr},
                {"error",{{"code",-32700},{"message","parse error"}}}}.dump());
            continue;
        }
        auto resp = handleRequest(*reg, req, progress);
        if (resp)
            sendTextFrame(s, resp->dump());
    }
    closesocket(s);
}

} // namespace

void runWs(const ToolRegistry& reg, int port, const std::string& webRoot) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[ws] WSAStartup failed\n");
        return;
    }

    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        fprintf(stderr, "[ws] socket() failed\n");
        WSACleanup();
        return;
    }

    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");   // local-only
    addr.sin_port = htons((u_short)port);

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        fprintf(stderr, "[ws] bind() failed on port %d\n", port);
        closesocket(listenSock);
        WSACleanup();
        return;
    }
    if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
        fprintf(stderr, "[ws] listen() failed\n");
        closesocket(listenSock);
        WSACleanup();
        return;
    }

    fprintf(stderr, "[ws] bridge listening on http://127.0.0.1:%d/ (WebSocket + static web root \"%s\")\n",
            port, webRoot.c_str());

    while (true) {
        SOCKET client = accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            fprintf(stderr, "[ws] accept() failed\n");
            continue;
        }
        std::thread(handleClient, client, &reg, webRoot).detach();
    }

    // Unreachable for a long-running local server; kept for completeness.
    closesocket(listenSock);
    WSACleanup();
}

} // namespace mcp
