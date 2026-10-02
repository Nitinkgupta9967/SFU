const childProcess = require("child_process");
const crypto = require("crypto");
const fs = require("fs");
const http = require("http");
const net = require("net");
const os = require("os");
const path = require("path");

const root = path.resolve(__dirname, "..");
const chromePath = process.env.CHROME_PATH || "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe";
const buildDir = process.env.SFU_BUILD_DIR || "build";
const wsServerPath = path.join(root, buildDir, "ws_signaling_server.exe");
const webRoot = path.join(root, "web");

function delay(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function startStaticServer(port) {
  const server = http.createServer((request, response) => {
    const requested = request.url === "/" ? "/index.html" : request.url;
    const filePath = path.join(webRoot, path.normalize(requested).replace(/^(\.\.[/\\])+/, ""));
    if (!filePath.startsWith(webRoot) || !fs.existsSync(filePath)) {
      response.writeHead(404);
      response.end("not found");
      return;
    }

    const extension = path.extname(filePath);
    const contentTypes = {
      ".html": "text/html",
      ".css": "text/css",
      ".js": "application/javascript"
    };
    response.writeHead(200, { "content-type": contentTypes[extension] || "application/octet-stream" });
    response.end(fs.readFileSync(filePath));
  });

  return new Promise((resolve) => {
    server.listen(port, "127.0.0.1", () => resolve(server));
  });
}

async function fetchJson(url, options = {}) {
  const response = await fetch(url, options);
  if (!response.ok) {
    throw new Error(`${url} returned ${response.status}`);
  }
  return response.json();
}

async function waitForChrome(port) {
  for (let attempt = 0; attempt < 50; ++attempt) {
    try {
      return await fetchJson(`http://127.0.0.1:${port}/json/version`);
    } catch {
      await delay(100);
    }
  }
  throw new Error("Chrome remote debugging did not start");
}

async function createTarget(port, url) {
  const response = await fetch(`http://127.0.0.1:${port}/json/new?${encodeURIComponent(url)}`, {
    method: "PUT"
  });
  if (!response.ok) {
    throw new Error(`createTarget failed: ${response.status}`);
  }
  return response.json();
}

class RawWebSocket {
  constructor(webSocketUrl) {
    const parsed = new URL(webSocketUrl);
    this.socket = net.createConnection(Number(parsed.port), parsed.hostname);
    this.buffer = Buffer.alloc(0);
    this.handshakeDone = false;
    this.fragmentOpcode = 0;
    this.fragmentPayloads = [];
    this.onMessage = () => {};

    this.ready = new Promise((resolve, reject) => {
      const key = crypto.randomBytes(16).toString("base64");
      const requestPath = `${parsed.pathname}${parsed.search}`;
      const request = [
        `GET ${requestPath} HTTP/1.1`,
        `Host: ${parsed.host}`,
        "Upgrade: websocket",
        "Connection: Upgrade",
        `Sec-WebSocket-Key: ${key}`,
        "Sec-WebSocket-Version: 13",
        "",
        ""
      ].join("\r\n");

      this.socket.once("connect", () => this.socket.write(request));
      this.socket.once("error", reject);
      this.socket.on("data", (chunk) => {
        this.buffer = Buffer.concat([this.buffer, chunk]);
        if (!this.handshakeDone) {
          const headerEnd = this.buffer.indexOf("\r\n\r\n");
          if (headerEnd < 0) {
            return;
          }
          const headerText = this.buffer.subarray(0, headerEnd).toString("utf8");
          this.buffer = this.buffer.subarray(headerEnd + 4);
          if (!headerText.startsWith("HTTP/1.1 101")) {
            reject(new Error(`WebSocket upgrade failed: ${headerText.split("\r\n")[0]}`));
            return;
          }
          this.handshakeDone = true;
          resolve();
        }
        this.readFrames();
      });
    });
  }

  readFrames() {
    while (this.buffer.length >= 2) {
      const first = this.buffer[0];
      const second = this.buffer[1];
      const fin = (first & 0x80) !== 0;
      const opcode = first & 0x0f;
      const masked = (second & 0x80) !== 0;
      let offset = 2;
      let length = second & 0x7f;

      if (length === 126) {
        if (this.buffer.length < offset + 2) {
          return;
        }
        length = this.buffer.readUInt16BE(offset);
        offset += 2;
      } else if (length === 127) {
        if (this.buffer.length < offset + 8) {
          return;
        }
        const high = this.buffer.readUInt32BE(offset);
        const low = this.buffer.readUInt32BE(offset + 4);
        if (high !== 0) {
          throw new Error("Frame too large for smoke test");
        }
        length = low;
        offset += 8;
      }

      const maskOffset = masked ? 4 : 0;
      if (this.buffer.length < offset + maskOffset + length) {
        return;
      }

      let payload = this.buffer.subarray(offset + maskOffset, offset + maskOffset + length);
      if (masked) {
        const mask = this.buffer.subarray(offset, offset + 4);
        payload = Buffer.from(payload.map((value, index) => value ^ mask[index % 4]));
      }
      this.buffer = this.buffer.subarray(offset + maskOffset + length);

      if (opcode === 0x1 || opcode === 0x2) {
        if (fin) {
          if (opcode === 0x1) {
            this.onMessage(payload.toString("utf8"));
          }
        } else {
          this.fragmentOpcode = opcode;
          this.fragmentPayloads = [payload];
        }
      } else if (opcode === 0x0) {
        this.fragmentPayloads.push(payload);
        if (fin) {
          const complete = Buffer.concat(this.fragmentPayloads);
          const originalOpcode = this.fragmentOpcode;
          this.fragmentOpcode = 0;
          this.fragmentPayloads = [];
          if (originalOpcode === 0x1) {
            this.onMessage(complete.toString("utf8"));
          }
        }
      } else if (opcode === 0x8) {
        this.close();
      } else if (opcode === 0x9) {
        this.writeFrame(0xA, payload);
      }
    }
  }

  send(text) {
    this.writeFrame(0x1, Buffer.from(text, "utf8"));
  }

  writeFrame(opcode, payload) {
    const mask = crypto.randomBytes(4);
    let header;
    if (payload.length < 126) {
      header = Buffer.alloc(2);
      header[1] = 0x80 | payload.length;
    } else if (payload.length <= 0xffff) {
      header = Buffer.alloc(4);
      header[1] = 0x80 | 126;
      header.writeUInt16BE(payload.length, 2);
    } else {
      header = Buffer.alloc(10);
      header[1] = 0x80 | 127;
      header.writeUInt32BE(0, 2);
      header.writeUInt32BE(payload.length, 6);
    }
    header[0] = 0x80 | opcode;

    const maskedPayload = Buffer.alloc(payload.length);
    for (let index = 0; index < payload.length; ++index) {
      maskedPayload[index] = payload[index] ^ mask[index % 4];
    }
    this.socket.write(Buffer.concat([header, mask, maskedPayload]));
  }

  close() {
    this.socket.end();
  }
}

class CdpClient {
  constructor(webSocketUrl) {
    this.nextId = 1;
    this.pending = new Map();
    this.events = [];
    this.socket = new RawWebSocket(webSocketUrl);
    this.ready = this.socket.ready;
    this.socket.onMessage = (data) => {
      const message = JSON.parse(data);
      if (message.id && this.pending.has(message.id)) {
        const { resolve, reject, timer } = this.pending.get(message.id);
        clearTimeout(timer);
        this.pending.delete(message.id);
        if (message.error) {
          reject(new Error(message.error.message));
        } else {
          resolve(message.result);
        }
        return;
      }
      this.events.push(message);
    };
  }

  async send(method, params = {}) {
    await this.ready;
    const id = this.nextId++;
    this.socket.send(JSON.stringify({ id, method, params }));
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`CDP command timed out: ${method}`));
      }, 10000);
      this.pending.set(id, { resolve, reject, timer });
    });
  }

  async evaluate(expression) {
    const result = await this.send("Runtime.evaluate", {
      expression,
      awaitPromise: true,
      returnByValue: true
    });
    if (result.exceptionDetails) {
      const details = result.exceptionDetails.exception && result.exceptionDetails.exception.description
        ? result.exceptionDetails.exception.description
        : result.exceptionDetails.text;
      throw new Error(details || "evaluation failed");
    }
    return result.result.value;
  }

  close() {
    this.socket.close();
  }
}

async function waitForLog(client, pattern, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const text = await client.evaluate("document.querySelector('#log').textContent");
    if (text.includes(pattern)) {
      return text;
    }
    await delay(250);
  }
  return client.evaluate("document.querySelector('#log').textContent");
}

async function readPageLog(client) {
  return client.evaluate("document.querySelector('#log').textContent");
}

async function readRtcStats(client) {
  return client.evaluate(`(async () => {
    const pc = peerConnection;
    if (!pc) {
      return { error: "missing peerConnection" };
    }

    const stats = await pc.getStats();
    const outbound = [];
    const inbound = [];
    stats.forEach((report) => {
      if (report.type === "outbound-rtp" && !report.isRemote) {
        outbound.push({
          id: report.id,
          kind: report.kind || report.mediaType,
          packetsSent: report.packetsSent || 0,
          bytesSent: report.bytesSent || 0,
          framesEncoded: report.framesEncoded || 0
        });
      }
      if (report.type === "inbound-rtp" && !report.isRemote) {
        inbound.push({
          id: report.id,
          kind: report.kind || report.mediaType,
          packetsReceived: report.packetsReceived || 0,
          bytesReceived: report.bytesReceived || 0,
          framesDecoded: report.framesDecoded || 0
        });
      }
    });

    return {
      connectionState: pc.connectionState,
      iceConnectionState: pc.iceConnectionState,
      signalingState: pc.signalingState,
      transceivers: pc.getTransceivers().map((transceiver) => ({
        mid: transceiver.mid,
        direction: transceiver.direction,
        currentDirection: transceiver.currentDirection,
        senderTrack: transceiver.sender && transceiver.sender.track ? transceiver.sender.track.kind : null,
        receiverTrack: transceiver.receiver && transceiver.receiver.track ? transceiver.receiver.track.kind : null,
        receiverMuted: transceiver.receiver && transceiver.receiver.track ? transceiver.receiver.track.muted : null
      })),
      outbound,
      inbound
    };
  })()`);
}

async function waitForCondition(predicate, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await predicate()) {
      return true;
    }
    await delay(250);
  }
  return predicate();
}

async function setupParticipant(client, participant) {
  await client.send("Runtime.enable");
  await client.send("Page.enable");
  await client.send("Page.bringToFront");
  await waitForCondition(
    async () => client.evaluate("Boolean(document.querySelector('#participant') && document.querySelector('#log'))"),
    5000);
  await client.evaluate(`document.querySelector('#participant').value = ${JSON.stringify(participant)}`);
  await client.evaluate("document.getElementById('connect').click()");
  await waitForLog(client, "connected to ws://127.0.0.1:8080", 5000);
  await client.evaluate("document.getElementById('join').click()");
  await waitForLog(client, "joined", 5000);
}

async function startWebRtc(client) {
  await client.send("Page.bringToFront");
  await client.evaluate("document.getElementById('start-webrtc').click()");
  return waitForLog(client, "applied SFU answer", 15000);
}

function tailLog(text, lineCount = 80) {
  const lines = text.trim().split(/\r?\n/);
  return lines.slice(Math.max(0, lines.length - lineCount)).join("\n");
}

function killProcessTree(processHandle) {
  if (!processHandle || !processHandle.pid) {
    return;
  }
  if (process.platform === "win32") {
    childProcess.spawnSync("taskkill", ["/pid", String(processHandle.pid), "/t", "/f"], {
      stdio: "ignore"
    });
    return;
  }
  processHandle.kill();
}

async function main() {
  if (!fs.existsSync(wsServerPath)) {
    throw new Error(`Missing ${wsServerPath}. Build the project first.`);
  }
  if (!fs.existsSync(chromePath)) {
    throw new Error(`Missing Chrome at ${chromePath}`);
  }

  const httpServer = await startStaticServer(9000);
  let sfuOutput = "";
  const wsServer = childProcess.spawn(wsServerPath, ["127.0.0.1", "8080"], {
    cwd: root,
    stdio: ["ignore", "pipe", "pipe"]
  });
  wsServer.stdout.on("data", (data) => {
    const text = data.toString();
    sfuOutput += text;
    process.stdout.write(`[sfu] ${text}`);
  });
  wsServer.stderr.on("data", (data) => {
    const text = data.toString();
    sfuOutput += text;
    process.stderr.write(`[sfu] ${text}`);
  });

  const cdpPort = 9300 + Math.floor(Math.random() * 300);
  const userDataDir = fs.mkdtempSync(path.join(os.tmpdir(), "sfu-chrome-"));
  const chromeArgs = [
    `--remote-debugging-port=${cdpPort}`,
    "--use-fake-ui-for-media-stream",
    "--use-fake-device-for-media-stream",
    "--autoplay-policy=no-user-gesture-required",
    "--no-first-run",
    "--no-default-browser-check",
    `--user-data-dir=${userDataDir}`,
    "about:blank"
  ];
  if (process.env.CHROME_HEADLESS !== "0") {
    chromeArgs.splice(1, 0, "--headless=new");
  }
  const chrome = childProcess.spawn(chromePath, chromeArgs, { stdio: "ignore" });

  try {
    await waitForChrome(cdpPort);
    const url = "http://127.0.0.1:9000/index.html";
    const aliceTarget = await createTarget(cdpPort, url);
    const bobTarget = await createTarget(cdpPort, url);
    const alice = new CdpClient(aliceTarget.webSocketDebuggerUrl);
    const bob = new CdpClient(bobTarget.webSocketDebuggerUrl);

    await setupParticipant(alice, "alice");
    await setupParticipant(bob, "bob");
    await startWebRtc(alice);
    await startWebRtc(bob);
    const observedForwarding = await waitForCondition(() => sfuOutput.includes("forwarded=1"), 15000);
    const aliceLog = await readPageLog(alice);
    const bobLog = await readPageLog(bob);
    const aliceStats = await readRtcStats(alice);
    const bobStats = await readRtcStats(bob);
    const aliceAnswer = await alice.evaluate("window.lastAnswerSdp || ''");
    const bobAnswer = await bob.evaluate("window.lastAnswerSdp || ''");

    console.log("--- alice log ---");
    console.log(tailLog(aliceLog));
    console.log("--- bob log ---");
    console.log(tailLog(bobLog));
    console.log("--- alice stats ---");
    console.log(JSON.stringify(aliceStats, null, 2));
    console.log("--- bob stats ---");
    console.log(JSON.stringify(bobStats, null, 2));

    alice.close();
    bob.close();

    if (!aliceLog.includes("applied SFU answer") || !bobLog.includes("applied SFU answer")) {
      throw new Error("Browser did not apply SFU answers");
    }
    if (!observedForwarding) {
      console.log("--- alice answer ---");
      console.log(aliceAnswer);
      console.log("--- bob answer ---");
      console.log(bobAnswer);
      throw new Error("RTP forwarding was not observed");
    }

    console.log("browser smoke ok");
  } finally {
    killProcessTree(chrome);
    killProcessTree(wsServer);
    httpServer.close();
  }
}

main().catch((error) => {
  console.error(error.stack || error.message);
  process.exit(1);
});
