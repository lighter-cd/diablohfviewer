// web/src/client.js
//
// Minimal MCP JSON-RPC 2.0 client over the mcp-server WebSocket bridge.
// The backend (src/mcp/transport_ws.cpp) speaks one JSON-RPC request per text
// frame and answers with a matching id; no `initialize` handshake is required.
// Every call() returns a Promise resolved when the response with the same id
// arrives, so concurrent calls are safe.

export class McpClient {
  constructor() {
    this.ws = null;
    this.nextId = 1;
    this.pending = new Map();      // id -> { resolve, reject }
    this.onStatus = null;          // (connected:boolean) => void
  }

  // Open the WebSocket. Resolves on open, rejects on error.
  connect(url) {
    return new Promise((resolve, reject) => {
      let settled = false;
      this.ws = new WebSocket(url);
      this.ws.onopen = () => {
        settled = true;
        if (this.onStatus) this.onStatus(true);
        resolve();
      };
      this.ws.onclose = () => {
        if (this.onStatus) this.onStatus(false);
        if (!settled) reject(new Error("WebSocket closed before open"));
      };
      this.ws.onerror = (e) => {
        if (!settled) { settled = true; reject(e); }
      };
      this.ws.onmessage = (ev) => this._onMessage(ev.data);
    });
  }

  disconnect() {
    if (this.ws) this.ws.close();
  }

  _onMessage(data) {
    let msg;
    try {
      msg = JSON.parse(data);
    } catch {
      console.warn("non-JSON WS message ignored:", data);
      return;
    }
    if (msg.id === undefined || msg.id === null) return; // notifications: ignore
    const entry = this.pending.get(msg.id);
    if (!entry) return;
    this.pending.delete(msg.id);
    if (msg.error) {
      // The backend wraps tool failures in a generic "tool execution failed";
      // the real reason (e.g. "cel requires a positive integer 'width'") lives
      // in error.data.error. Surface it so decode failures are actionable.
      const detail = msg.error.data && msg.error.data.error;
      entry.reject(new Error(detail || msg.error.message || JSON.stringify(msg.error)));
    } else {
      entry.resolve(msg.result);
    }
  }

  _send(method, params) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN)
      return Promise.reject(new Error("WebSocket is not connected"));
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.ws.send(JSON.stringify({ jsonrpc: "2.0", id, method, params }));
    });
  }

  listTools() {
    return this._send("tools/list", {});
  }

  callTool(name, args) {
    return this._send("tools/call", { name, arguments: args || {} });
  }
}
