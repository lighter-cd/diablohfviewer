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

// Perform the WS handshake. Returns true on success (101 Switching Protocols
// already sent). Reads the client HTTP request incrementally until the blank
// line, finds Sec-WebSocket-Key, and replies with the computed accept value.
bool doHandshake(SOCKET s) {
    std::string head;
    char c;
    // Read until we have the full header block "\r\n\r\n".
    while (true) {
        if (!recvAll(s, &c, 1)) return false;
        head.push_back(c);
        size_t n = head.size();
        if (n >= 4 && head[n - 4] == '\r' && head[n - 3] == '\n' &&
            head[n - 2] == '\r' && head[n - 1] == '\n')
            break;
        if (head.size() > 16 * 1024) return false;   // guard
    }

    // Extract Sec-WebSocket-Key.
    std::string key;
    const std::string tag = "sec-websocket-key:";
    size_t p = head.find(tag);
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

void handleClient(SOCKET s, const ToolRegistry* reg) {
    if (!doHandshake(s)) { closesocket(s); return; }
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
        auto resp = handleRequest(*reg, req);
        if (resp)
            sendTextFrame(s, resp->dump());
    }
    closesocket(s);
}

} // namespace

void runWs(const ToolRegistry& reg, int port) {
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

    fprintf(stderr, "[ws] WebSocket bridge listening on ws://127.0.0.1:%d\n", port);

    while (true) {
        SOCKET client = accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            fprintf(stderr, "[ws] accept() failed\n");
            continue;
        }
        std::thread(handleClient, client, &reg).detach();
    }

    // Unreachable for a long-running local server; kept for completeness.
    closesocket(listenSock);
    WSACleanup();
}

} // namespace mcp
