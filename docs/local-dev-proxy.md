# Local Dev Proxy for Wasteland UI

When the ESP32 is powered on and running the game server, you can do live UI development on your PC without uploading files to the device. A small Node.js proxy server serves the `data/` folder as static files and tunnels WebSocket connections to the ESP32 so the game runs end-to-end from your browser.

---

## How it works

The game client (`network.js`) derives its WebSocket URL from the page host:

```js
const wsUrl = `ws://${location.host}/ws`;
```

If you open `data/index.html` directly as a file, or serve it from a plain static server, the WebSocket will try to connect back to `localhost` and fail. The proxy fixes this by intercepting WebSocket upgrade requests and piping them to the ESP32 at the TCP level — no external npm packages required.

```
Browser ──HTTP──▶ localhost:8080 ──serves──▶ data/index.html
Browser ──WS───▶ localhost:8080/ws ──proxied──▶ 192.168.4.78:80/ws (ESP32)
```

---

## Files

| File | Purpose |
|---|---|
| `.claude/dev-server.js` | The proxy server (Node.js, no dependencies) |
| `.claude/launch.json` | Claude Code launch config — tells Claude how to start it |

---

## Starting the server

Claude Code can start it for you automatically. Just ask:

> "Start the Wasteland UI server"

Claude will run `node .claude/dev-server.js` on port 8080 and open the preview.

To start it manually from a terminal:

```bash
node .claude/dev-server.js
```

Then open `http://localhost:8080` in your browser.

---

## Changing the ESP32 IP

The ESP32 IP is hardcoded near the top of `.claude/dev-server.js`:

```js
const ESP32_HOST = "192.168.4.78";
const ESP32_PORT = 80;
```

Update `ESP32_HOST` if your device gets a different address. The AP-mode default is `192.168.4.1`.

---

## How the WebSocket proxy works

Node's built-in `net` module is used to open a raw TCP connection to the ESP32 and replay the browser's HTTP upgrade handshake byte-for-byte:

```js
server.on("upgrade", (req, clientSocket, head) => {
  const upstream = net.connect(ESP32_PORT, ESP32_HOST, () => {
    // Rebuild and forward the original upgrade request
    upstream.write(reconstructedHeaders);
  });
  upstream.pipe(clientSocket);
  clientSocket.pipe(upstream);
});
```

Once the ESP32 completes the WebSocket handshake, both sockets are piped together bidirectionally. From that point the browser and the ESP32 talk directly through the proxy with no parsing overhead.

---

## Workflow

1. Power on the ESP32 and connect your PC to the `WASTELAND` WiFi AP (or the same LAN).
2. Start the dev server (`node .claude/dev-server.js` or ask Claude).
3. Open `http://localhost:8080` — the full game loads and connects to the live ESP32.
4. Edit files in `data/` and refresh the browser to see changes instantly, no firmware upload needed.
