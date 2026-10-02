#include "sfu/signaling.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

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

    if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closeSocket(socket);
        return kInvalidSocket;
    }

    if (::listen(socket, 16) != 0) {
        closeSocket(socket);
        return kInvalidSocket;
    }

    return socket;
}

std::string receiveLine(Socket socket) {
    std::string line;
    char buffer[256]{};

    while (true) {
        const auto received = recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }

        line.append(buffer, buffer + received);
        const auto newline = line.find('\n');
        if (newline != std::string::npos) {
            line.resize(newline);
            break;
        }
    }

    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    return line;
}

std::string joinRecipients(const std::vector<std::string>& recipients) {
    std::ostringstream out;
    for (std::size_t index = 0; index < recipients.size(); ++index) {
        if (index > 0) {
            out << ',';
        }
        out << recipients[index];
    }
    return out.str();
}

void sendAll(Socket socket, const std::string& text) {
    const char* data = text.data();
    std::size_t remaining = text.size();

    while (remaining > 0) {
        const auto sent = send(socket, data, static_cast<int>(remaining), 0);
        if (sent <= 0) {
            return;
        }

        data += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
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

    const auto host = argc > 1 ? std::string(argv[1]) : std::string("127.0.0.1");
    const auto port = parsePort(argc > 2 ? argv[2] : nullptr, 7000);

    Socket server = listenOn(host, port);
    if (server == kInvalidSocket) {
        std::cerr << "failed to listen on " << host << ':' << port << '\n';
        return 1;
    }

    sfu::SignalingHub hub;
    std::cout << "signaling server listening on " << host << ':' << port << '\n';

    while (true) {
        sockaddr_in peer{};
#ifdef _WIN32
        int peer_size = sizeof(peer);
#else
        socklen_t peer_size = sizeof(peer);
#endif
        Socket client = accept(server, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (client == kInvalidSocket) {
            continue;
        }

        const auto line = receiveLine(client);
        const auto parsed = sfu::parseSignalLine(line);
        if (!parsed.has_value()) {
            sendAll(client, "error|||client|invalid signal line\n");
            closeSocket(client);
            continue;
        }

        const auto event = hub.handle(*parsed);
        const auto response = std::string("ok|") +
            joinRecipients(event.recipients) +
            "|" +
            sfu::serializeSignalLine(event.message) +
            "\n";

        std::cout << "signal " << line << " recipients=" << joinRecipients(event.recipients) << '\n';
        sendAll(client, response);
        closeSocket(client);
    }
}
