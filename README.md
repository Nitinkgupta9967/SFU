# C++ SFU Core

This repository starts the MVP SFU as a transport-independent C++17 core.

Current pieces:

- RTP packet parsing and serialization
- Room and participant management
- One-to-many RTP forwarding
- Transport boundary for raw RTP ingress/egress
- RTP-activity based active speaker tracking
- Idle participant removal API
- Signaling hub for join/leave and offer/answer/ICE routing
- WebSocket signaling bridge for browser tabs
- Prebuilt libdatachannel WebRTC backend integration
- Per-room and per-participant packet metrics
- Minimal executable tests through CTest

## Build

Install one C++ toolchain first: Visual Studio Build Tools, MinGW, Clang, or WSL.

```powershell
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows, if CMake picks the wrong generator, use one explicitly:

```powershell
cmake -S . -B build -G "Ninja"
```

or:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

## Run

```powershell
.\build\sfu_server.exe
```

Run the UDP RTP forwarding demo:

```powershell
.\build\udp_rtp_demo.exe 127.0.0.1 5004
```

The demo auto-registers each UDP sender endpoint as a participant. When one endpoint sends a valid RTP datagram, the room forwards it to every other registered endpoint.

In two more terminals, start local RTP clients with different local ports. Start the second while the first is still running:

```powershell
.\build\udp_rtp_client.exe 6001 127.0.0.1 5004 20 10000
.\build\udp_rtp_client.exe 6002 127.0.0.1 5004 20 10000
```

The first packet from each client registers it. Once at least two clients are registered, each client should print forwarded RTP packets from the other endpoint.

For a deterministic single-command forwarding demo:

```powershell
.\build\udp_loopback_demo.exe
```

This starts an in-process UDP SFU plus two local clients and exits successfully only after one client receives forwarded RTP from the other.

Run the TCP signaling server:

```powershell
.\build\tcp_signaling_server.exe 127.0.0.1 7000
```

Then send line-protocol signaling commands from another terminal:

```powershell
.\build\tcp_signaling_client.exe 127.0.0.1 7000 "join|demo|alice||"
.\build\tcp_signaling_client.exe 127.0.0.1 7000 "join|demo|bob||"
.\build\tcp_signaling_client.exe 127.0.0.1 7000 "offer|demo|alice|bob|fake-sdp"
```

The temporary line protocol is `type|room|participant|target|payload`. It exists so the join/leave and relay behavior can be tested before the WebSocket/browser layer is added.

Run the browser WebSocket signaling bridge with the media-enabled x64 backend:

```powershell
cmake -S . -B build-media-x64 -G "NMake Makefiles" -DLIBDATACHANNEL_NUGET_ROOT=D:/SFU/third_party/libdatachannel_media_x64
cmake --build build-media-x64
.\build-media-x64\ws_signaling_server.exe 127.0.0.1 8080
```

Serve the browser demo from another terminal:

```powershell
python -m http.server 9000 -d web
```

Then open `http://127.0.0.1:9000/index.html` in two browser tabs. Use `alice` in one tab and `bob` in the other, connect both, join both, then use **Start WebRTC** to send a real browser SDP offer to the SFU backend. Do not use the old `build\ws_signaling_server.exe` for browser media forwarding.

The WebSocket server injects browser-origin RTP into the SFU room and forwards packets to other browser participants through the libdatachannel backend.

The browser media path uses the curated prebuilt package extracted at:

```text
third_party\libdatachannel_media_x64
```

Do not run `vcpkg install` unless you explicitly want to rebuild dependencies from source.

## Next Milestones

1. Validate two-tab browser media with real camera/microphone.
2. Track SSRC to media kind mappings from negotiated SDP instead of payload-type heuristics.
3. Add codec/SSRC rewriting for robust multi-party forwarding.
4. Add simulcast/RID layer selection and bandwidth adaptation.

See `docs\PROJECT_STATUS.md` and `docs\WEBRTC_INTEGRATION.md`.
