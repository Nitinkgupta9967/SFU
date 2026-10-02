#include "sfu/participant.hpp"
#include "sfu/room.hpp"
#include "sfu/signaling.hpp"
#include "sfu/transport.hpp"
#include "sfu/webrtc_backend.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;

struct SocketRuntime {
    SocketRuntime() {
        WSADATA data;
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~SocketRuntime() {
        if (ok) {
            WSACleanup();
        }
    }
    bool ok = false;
};

void closeSocket(Socket socket) {
    closesocket(socket);
}
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;

struct SocketRuntime {
    bool ok = true;
};

void closeSocket(Socket socket) {
    close(socket);
}
#endif

struct ClientState {
    Socket socket = kInvalidSocket;
    std::string room_id;
    std::string participant_id;
};

std::mutex g_mutex;
sfu::SignalingHub g_hub;
sfu::Room g_media_room("browser-media");
std::unordered_map<std::string, ClientState> g_clients;
#ifdef SFU_HAS_LIBDATACHANNEL
std::unique_ptr<sfu::WebRtcBackend> g_backend;
#endif

std::uint16_t parsePort(const char* value, std::uint16_t fallback) {
    if (value == nullptr) {
        return fallback;
    }

    const auto parsed = std::strtoul(value, nullptr, 10);
    if (parsed == 0 || parsed > 65535) {
        return fallback;
    }

    return static_cast<std::uint16_t>(parsed);
}

std::uint32_t leftRotate(std::uint32_t value, std::uint32_t bits) {
    return (value << bits) | (value >> (32U - bits));
}

std::array<std::uint8_t, 20> sha1(const std::string& input) {
    std::vector<std::uint8_t> message(input.begin(), input.end());
    const auto bit_length = static_cast<std::uint64_t>(message.size()) * 8U;
    message.push_back(0x80U);
    while ((message.size() % 64U) != 56U) {
        message.push_back(0U);
    }
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xffU));
    }

    std::uint32_t h0 = 0x67452301U;
    std::uint32_t h1 = 0xefcdab89U;
    std::uint32_t h2 = 0x98badcfeU;
    std::uint32_t h3 = 0x10325476U;
    std::uint32_t h4 = 0xc3d2e1f0U;

    for (std::size_t chunk = 0; chunk < message.size(); chunk += 64U) {
        std::array<std::uint32_t, 80> words{};
        for (std::size_t i = 0; i < 16U; ++i) {
            const auto offset = chunk + (i * 4U);
            words[i] =
                (static_cast<std::uint32_t>(message[offset]) << 24U) |
                (static_cast<std::uint32_t>(message[offset + 1]) << 16U) |
                (static_cast<std::uint32_t>(message[offset + 2]) << 8U) |
                static_cast<std::uint32_t>(message[offset + 3]);
        }
        for (std::size_t i = 16U; i < 80U; ++i) {
            words[i] = leftRotate(words[i - 3U] ^ words[i - 8U] ^ words[i - 14U] ^ words[i - 16U], 1U);
        }

        auto a = h0;
        auto b = h1;
        auto c = h2;
        auto d = h3;
        auto e = h4;

        for (std::size_t i = 0; i < 80U; ++i) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20U) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999U;
            } else if (i < 40U) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1U;
            } else if (i < 60U) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcU;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6U;
            }

            const auto temp = leftRotate(a, 5U) + f + e + k + words[i];
            e = d;
            d = c;
            c = leftRotate(b, 30U);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::array<std::uint8_t, 20> digest{};
    const std::array<std::uint32_t, 5> values{h0, h1, h2, h3, h4};
    for (std::size_t i = 0; i < values.size(); ++i) {
        digest[i * 4U] = static_cast<std::uint8_t>((values[i] >> 24U) & 0xffU);
        digest[(i * 4U) + 1U] = static_cast<std::uint8_t>((values[i] >> 16U) & 0xffU);
        digest[(i * 4U) + 2U] = static_cast<std::uint8_t>((values[i] >> 8U) & 0xffU);
        digest[(i * 4U) + 3U] = static_cast<std::uint8_t>(values[i] & 0xffU);
    }
    return digest;
}

std::string base64(const std::array<std::uint8_t, 20>& bytes) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    for (std::size_t i = 0; i < bytes.size(); i += 3U) {
        const auto b0 = bytes[i];
        const auto b1 = i + 1U < bytes.size() ? bytes[i + 1U] : 0U;
        const auto b2 = i + 2U < bytes.size() ? bytes[i + 2U] : 0U;
        const auto triple = (static_cast<std::uint32_t>(b0) << 16U) |
            (static_cast<std::uint32_t>(b1) << 8U) |
            static_cast<std::uint32_t>(b2);
        output.push_back(alphabet[(triple >> 18U) & 0x3fU]);
        output.push_back(alphabet[(triple >> 12U) & 0x3fU]);
        output.push_back(i + 1U < bytes.size() ? alphabet[(triple >> 6U) & 0x3fU] : '=');
        output.push_back(i + 2U < bytes.size() ? alphabet[triple & 0x3fU] : '=');
    }
    return output;
}

bool sendAll(Socket socket, const std::string& text) {
    const char* data = text.data();
    std::size_t remaining = text.size();
    while (remaining > 0) {
        const auto sent = send(socket, data, static_cast<int>(remaining), 0);
        if (sent <= 0) {
            return false;
        }
        data += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool receiveExact(Socket socket, std::uint8_t* data, std::size_t size) {
    std::size_t received_total = 0;
    while (received_total < size) {
        const auto received = recv(
            socket,
            reinterpret_cast<char*>(data + received_total),
            static_cast<int>(size - received_total),
            0);
        if (received <= 0) {
            return false;
        }
        received_total += static_cast<std::size_t>(received);
    }
    return true;
}

Socket listenOn(const std::string& host, std::uint16_t port) {
    Socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalidSocket) {
        return kInvalidSocket;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        closeSocket(socket);
        return kInvalidSocket;
    }

    int reuse = 1;
    setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(socket, 16) != 0) {
        closeSocket(socket);
        return kInvalidSocket;
    }

    return socket;
}

std::string readHttpHeaders(Socket socket) {
    std::string headers;
    char buffer[512]{};
    while (headers.find("\r\n\r\n") == std::string::npos) {
        const auto received = recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        headers.append(buffer, buffer + received);
        if (headers.size() > 8192U) {
            break;
        }
    }
    return headers;
}

std::string headerValue(const std::string& headers, const std::string& name) {
    const auto found = headers.find(name + ":");
    if (found == std::string::npos) {
        return {};
    }

    auto start = found + name.size() + 1U;
    while (start < headers.size() && headers[start] == ' ') {
        ++start;
    }
    const auto end = headers.find("\r\n", start);
    return headers.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::vector<std::string> splitLimit(const std::string& text, char delimiter, std::size_t max_parts) {
    std::vector<std::string> parts;
    std::size_t start = 0;

    while (parts.size() + 1U < max_parts) {
        const auto found = text.find(delimiter, start);
        if (found == std::string::npos) {
            break;
        }
        parts.push_back(text.substr(start, found - start));
        start = found + 1U;
    }

    parts.push_back(text.substr(start));
    return parts;
}

bool handshake(Socket socket) {
    const auto headers = readHttpHeaders(socket);
    const auto key = headerValue(headers, "Sec-WebSocket-Key");
    if (key.empty()) {
        return false;
    }

    const auto accept = base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
    std::ostringstream response;
    response << "HTTP/1.1 101 Switching Protocols\r\n"
             << "Upgrade: websocket\r\n"
             << "Connection: Upgrade\r\n"
             << "Sec-WebSocket-Accept: " << accept << "\r\n\r\n";
    return sendAll(socket, response.str());
}

std::optional<std::string> readFrame(Socket socket) {
    std::array<std::uint8_t, 2> header{};
    if (!receiveExact(socket, header.data(), header.size())) {
        return std::nullopt;
    }

    const auto opcode = header[0] & 0x0fU;
    if (opcode == 0x08U) {
        return std::nullopt;
    }
    if (opcode != 0x01U) {
        return std::string{};
    }

    const bool masked = (header[1] & 0x80U) != 0;
    std::uint64_t payload_length = header[1] & 0x7fU;
    if (payload_length == 126U) {
        std::array<std::uint8_t, 2> length{};
        if (!receiveExact(socket, length.data(), length.size())) {
            return std::nullopt;
        }
        payload_length = (static_cast<std::uint64_t>(length[0]) << 8U) | length[1];
    } else if (payload_length == 127U) {
        std::array<std::uint8_t, 8> length{};
        if (!receiveExact(socket, length.data(), length.size())) {
            return std::nullopt;
        }
        payload_length = 0;
        for (auto byte : length) {
            payload_length = (payload_length << 8U) | byte;
        }
    }

    std::array<std::uint8_t, 4> mask{};
    if (masked && !receiveExact(socket, mask.data(), mask.size())) {
        return std::nullopt;
    }

    std::string payload(static_cast<std::size_t>(payload_length), '\0');
    if (payload_length > 0 &&
        !receiveExact(socket, reinterpret_cast<std::uint8_t*>(&payload[0]), payload.size())) {
        return std::nullopt;
    }

    if (masked) {
        for (std::size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ mask[i % 4U]);
        }
    }

    return payload;
}

bool sendFrame(Socket socket, const std::string& payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81U));
    if (payload.size() <= 125U) {
        frame.push_back(static_cast<char>(payload.size()));
    } else if (payload.size() <= 65535U) {
        frame.push_back(static_cast<char>(126U));
        frame.push_back(static_cast<char>((payload.size() >> 8U) & 0xffU));
        frame.push_back(static_cast<char>(payload.size() & 0xffU));
    } else {
        frame.push_back(static_cast<char>(127U));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((payload.size() >> shift) & 0xffU));
        }
    }
    frame += payload;
    return sendAll(socket, frame);
}

std::string encodeIcePayload(const sfu::WebRtcIceCandidate& candidate) {
    std::ostringstream out;
    out << candidate.mid << ';' << candidate.mline_index << ';' << candidate.candidate;
    return out.str();
}

sfu::WebRtcIceCandidate decodeIcePayload(const std::string& payload) {
    const auto parts = splitLimit(payload, ';', 3);
    sfu::WebRtcIceCandidate candidate;
    if (parts.size() == 3U) {
        candidate.mid = parts[0];
        candidate.mline_index = static_cast<std::uint32_t>(std::strtoul(parts[1].c_str(), nullptr, 10));
        candidate.candidate = parts[2];
    } else {
        candidate.candidate = payload;
    }
    return candidate;
}

bool isBackendTarget(const sfu::SignalMessage& message) {
    return message.target_participant_id == "sfu" || message.target_participant_id == "server";
}

bool handleBackendSignal(Socket socket, const sfu::SignalMessage& message) {
#ifdef SFU_HAS_LIBDATACHANNEL
    if (!g_backend || !isBackendTarget(message)) {
        return false;
    }

    if (message.type == sfu::SignalType::Offer) {
        sfu::WebRtcSessionDescription answer;
        try {
            answer = g_backend->handleOffer(
                message.participant_id,
                sfu::WebRtcSessionDescription{"offer", message.payload});
        } catch (const std::exception& error) {
            answer = sfu::WebRtcSessionDescription{"error", error.what()};
        } catch (...) {
            answer = sfu::WebRtcSessionDescription{"error", "unknown WebRTC offer error"};
        }

        sfu::SignalMessage response;
        response.type = answer.type == "error" ? sfu::SignalType::Error : sfu::SignalType::Answer;
        response.room_id = message.room_id;
        response.participant_id = "sfu";
        response.target_participant_id = message.participant_id;
        response.payload = answer.sdp;
        sendFrame(socket, sfu::serializeSignalLine(response));
        return true;
    }

    if (message.type == sfu::SignalType::IceCandidate) {
        bool added = false;
        try {
            added = g_backend->addIceCandidate(message.participant_id, decodeIcePayload(message.payload));
        } catch (const std::exception& error) {
            std::cerr << "backend ICE error " << message.participant_id
                      << " error=" << error.what() << '\n';
        } catch (...) {
            std::cerr << "backend ICE error " << message.participant_id
                      << " error=unknown\n";
        }
        sendFrame(socket, added ? "ack|ice" : "error|||client|failed to add ICE candidate");
        return true;
    }
#else
    (void)socket;
    (void)message;
#endif

    return false;
}

void removeClient(Socket socket) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto iterator = g_clients.begin(); iterator != g_clients.end();) {
        if (iterator->second.socket == socket) {
            iterator = g_clients.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void handleClient(Socket socket) {
    if (!handshake(socket)) {
        closeSocket(socket);
        return;
    }

    while (true) {
        const auto frame = readFrame(socket);
        if (!frame.has_value()) {
            break;
        }

        const auto parsed = sfu::parseSignalLine(*frame);
        if (!parsed.has_value()) {
            sendFrame(socket, "error|||client|invalid signal line");
            continue;
        }

        if (handleBackendSignal(socket, *parsed)) {
            continue;
        }

        std::vector<std::pair<Socket, std::string>> outbound;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto event = g_hub.handle(*parsed);
            if (parsed->type == sfu::SignalType::Join) {
                g_clients[parsed->participant_id] = ClientState{socket, parsed->room_id, parsed->participant_id};
#ifdef SFU_HAS_LIBDATACHANNEL
                if (g_backend) {
                    g_backend->createParticipant(parsed->participant_id);
                    if (!g_media_room.findParticipant(parsed->participant_id)) {
                        const auto participant_id = parsed->participant_id;
                        auto participant = std::make_shared<sfu::Participant>(
                            participant_id,
                            [participant_id](const sfu::RtpPacket& packet) {
                                if (g_backend) {
                                    g_backend->sendRtp(participant_id, packet.raw());
                                }
                            });
                        g_media_room.addParticipant(participant);
                    }
                }
#endif
            }
            if (parsed->type == sfu::SignalType::Leave) {
                g_clients.erase(parsed->participant_id);
#ifdef SFU_HAS_LIBDATACHANNEL
                if (g_backend) {
                    g_backend->removeParticipant(parsed->participant_id);
                    g_media_room.removeParticipant(parsed->participant_id);
                }
#endif
            }

            outbound.emplace_back(socket, "ack|" + sfu::serializeSignalLine(event.message));
            for (const auto& recipient : event.recipients) {
                const auto found = g_clients.find(recipient);
                if (found != g_clients.end()) {
                    outbound.emplace_back(found->second.socket, sfu::serializeSignalLine(event.message));
                }
            }
        }

        for (const auto& item : outbound) {
            sendFrame(item.first, item.second);
        }
    }

    removeClient(socket);
    closeSocket(socket);
}

}  // namespace

int main(int argc, char** argv) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    SocketRuntime runtime;
    if (!runtime.ok) {
        std::cerr << "socket runtime initialization failed\n";
        return 1;
    }

#ifdef SFU_HAS_LIBDATACHANNEL
    g_backend = std::make_unique<sfu::LibDataChannelBackend>();
    g_backend->onRtp(
        [](const std::string& participant_id, const std::vector<std::uint8_t>& rtp_packet) {
            std::lock_guard<std::mutex> lock(g_mutex);
            sfu::RoomRtpIngress ingress(g_media_room);
            const auto result = ingress.receiveRawRtp(participant_id, rtp_packet);
            std::cout << "browser RTP from " << participant_id
                      << " bytes=" << rtp_packet.size()
                      << " forwarded=" << result.forwarding.recipients
                      << " dropped=" << result.forwarding.dropped << '\n';
        });
    g_backend->onLocalIceCandidate(
        [](const std::string& participant_id, const sfu::WebRtcIceCandidate& candidate) {
            Socket socket = kInvalidSocket;
            std::string room_id;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                const auto found = g_clients.find(participant_id);
                if (found == g_clients.end()) {
                    return;
                }
                socket = found->second.socket;
                room_id = found->second.room_id;
            }

            sfu::SignalMessage message;
            message.type = sfu::SignalType::IceCandidate;
            message.room_id = room_id;
            message.participant_id = "sfu";
            message.target_participant_id = participant_id;
            message.payload = encodeIcePayload(candidate);
            sendFrame(socket, sfu::serializeSignalLine(message));
        });
#endif

    const auto host = argc > 1 ? std::string(argv[1]) : std::string("127.0.0.1");
    const auto port = parsePort(argc > 2 ? argv[2] : nullptr, 8080);
    Socket server = listenOn(host, port);
    if (server == kInvalidSocket) {
        std::cerr << "failed to listen on " << host << ':' << port << '\n';
        return 1;
    }

    std::cout << "websocket signaling server listening on ws://" << host << ':' << port << '\n';
    while (true) {
        Socket client = accept(server, nullptr, nullptr);
        if (client == kInvalidSocket) {
            continue;
        }

        std::thread(handleClient, client).detach();
    }
}
