#include <cstdint>
#include <cstdlib>
#include <iostream>
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

Socket connectTo(const std::string& host, std::uint16_t port) {
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

    if (connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        closeSocket(socket);
        return kInvalidSocket;
    }

    return socket;
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

std::string receiveAll(Socket socket) {
    std::string output;
    char buffer[512]{};

    while (true) {
        const auto received = recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        output.append(buffer, buffer + received);
    }

    return output;
}

void printUsage(const char* executable) {
    std::cout << "usage: " << executable
              << " [host] [port] <type|room|participant|target|payload>\n"
              << "example: " << executable << " 127.0.0.1 7000 join|demo|alice||\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    SocketRuntime runtime;
    if (!runtime.ok) {
        std::cerr << "socket runtime initialization failed\n";
        return 1;
    }

    std::string host = "127.0.0.1";
    std::uint16_t port = 7000;
    std::string message;

    if (argc >= 4) {
        host = argv[1];
        port = parsePort(argv[2], 7000);
        message = argv[3];
    } else {
        message = argv[1];
    }

    Socket socket = connectTo(host, port);
    if (socket == kInvalidSocket) {
        std::cerr << "failed to connect to " << host << ':' << port << '\n';
        return 1;
    }

    sendAll(socket, message + "\n");
    std::cout << receiveAll(socket);
    closeSocket(socket);

    return 0;
}
