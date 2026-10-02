const logElement = document.querySelector("#log");
const roomInput = document.querySelector("#room");
const participantInput = document.querySelector("#participant");
const targetInput = document.querySelector("#target");
const localVideo = document.querySelector("#local-video");
const remoteVideo = document.querySelector("#remote-video");

let socket = null;
let peerConnection = null;
let localStream = null;
let pendingRemoteIceCandidates = [];

function log(message) {
  const time = new Date().toLocaleTimeString();
  logElement.textContent += `[${time}] ${message}\n`;
  logElement.scrollTop = logElement.scrollHeight;
}

function summarize(text) {
  if (text.length <= 180) {
    return text;
  }
  return `${text.slice(0, 180)}... (${text.length} chars)`;
}

function value(input) {
  return input.value.trim();
}

function sendLine(type, target, payload) {
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    log("WebSocket is not connected");
    return;
  }

  const line = `${type}|${value(roomInput)}|${value(participantInput)}|${target}|${payload}`;
  socket.send(line);
  log(`sent ${summarize(line)}`);
}

function parseSignalLine(line) {
  if (line.startsWith("ack|")) {
    line = line.slice(4);
  }

  const parts = line.split("|");
  return {
    type: parts[0] || "",
    room: parts[1] || "",
    participant: parts[2] || "",
    target: parts[3] || "",
    payload: parts.slice(4).join("|")
  };
}

function encodeIceCandidate(candidate) {
  return `${candidate.sdpMid || ""};${candidate.sdpMLineIndex || 0};${candidate.candidate}`;
}

function decodeIceCandidate(payload) {
  const parts = payload.split(";");
  if (parts.length >= 3) {
    return {
      sdpMid: parts[0] || null,
      sdpMLineIndex: Number(parts[1] || 0),
      candidate: parts.slice(2).join(";")
    };
  }
  return { candidate: payload };
}

function createPeerConnection() {
  peerConnection = new RTCPeerConnection({
    iceServers: [{ urls: "stun:stun.l.google.com:19302" }]
  });
  pendingRemoteIceCandidates = [];

  peerConnection.addEventListener("icecandidate", (event) => {
    if (event.candidate) {
      sendLine("ice", "sfu", encodeIceCandidate(event.candidate));
    }
  });

  peerConnection.addEventListener("track", (event) => {
    remoteVideo.srcObject = event.streams[0];
    log("remote track received");
  });

  peerConnection.addEventListener("connectionstatechange", () => {
    log(`connection state ${peerConnection.connectionState}`);
  });
  peerConnection.addEventListener("iceconnectionstatechange", () => {
    log(`ice connection state ${peerConnection.iceConnectionState}`);
  });
  peerConnection.addEventListener("icegatheringstatechange", () => {
    log(`ice gathering state ${peerConnection.iceGatheringState}`);
  });
  peerConnection.addEventListener("signalingstatechange", () => {
    log(`signaling state ${peerConnection.signalingState}`);
  });

  return peerConnection;
}

async function handleSignalMessage(line) {
  const signal = parseSignalLine(line);
  if (!peerConnection || signal.target !== value(participantInput)) {
    return;
  }

  if (signal.type === "answer") {
    window.lastAnswerSdp = signal.payload;
    await peerConnection.setRemoteDescription({ type: "answer", sdp: signal.payload });
    log("applied SFU answer");
    for (const candidate of pendingRemoteIceCandidates) {
      await peerConnection.addIceCandidate(candidate);
      log("added queued SFU ICE candidate");
    }
    pendingRemoteIceCandidates = [];
  }

  if (signal.type === "ice" && signal.payload) {
    const candidate = decodeIceCandidate(signal.payload);
    if (!peerConnection.remoteDescription) {
      pendingRemoteIceCandidates.push(candidate);
      log("queued SFU ICE candidate");
      return;
    }
    await peerConnection.addIceCandidate(candidate);
    log("added SFU ICE candidate");
  }
}

document.querySelector("#connect").addEventListener("click", () => {
  socket = new WebSocket("ws://127.0.0.1:8080");

  socket.addEventListener("open", () => log("connected to ws://127.0.0.1:8080"));
  socket.addEventListener("close", () => log("socket closed"));
  socket.addEventListener("error", () => log("socket error"));
  socket.addEventListener("message", (event) => {
    log(`received ${summarize(event.data)}`);
    handleSignalMessage(event.data).catch((error) => log(`signal error ${error.message}`));
  });
});

document.querySelector("#join").addEventListener("click", () => {
  sendLine("join", "", "browser-joined");
});

document.querySelector("#send-offer").addEventListener("click", () => {
  const payload = `browser-offer-${Date.now()}`;
  sendLine("offer", value(targetInput), payload);
});

document.querySelector("#start-webrtc").addEventListener("click", async () => {
  if (!socket || socket.readyState !== WebSocket.OPEN) {
    log("Connect the WebSocket first");
    return;
  }

  if (!peerConnection) {
    createPeerConnection();
  }

  localStream = await navigator.mediaDevices.getUserMedia({ audio: true, video: true });
  localVideo.srcObject = localStream;
  for (const track of localStream.getTracks()) {
    peerConnection.addTrack(track, localStream);
  }

  const offer = await peerConnection.createOffer();
  await peerConnection.setLocalDescription(offer);
  sendLine("offer", "sfu", offer.sdp);
  log("sent browser SDP offer to SFU");
});

log("Open this page in two browser tabs. Use participants alice and bob, then connect and join each tab.");
