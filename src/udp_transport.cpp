#include "sfu/udp_transport.hpp"

#include <array>
#include <cstring>
#include <sstream>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sfu {

namespace {

#ifdef _WIN32
constexpr UdpSocket::NativeSocket kInvalidSocket = INVALID_SOCKET;

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data;
        initialized_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }

    ~WinsockRuntime() {
        if (initialized_) {
            WSACleanup();
        }
    }

    bool initialized() const noexcept {
        return initialized_;
    }

private:
    bool initialized_ = false;
};

WinsockRuntime& winsockRuntime() {
    static WinsockRuntime runtime;
    return runtime;
}

int socketLastError() {
    return WSAGetLastError();
}

void closeSocket(UdpSocket::NativeSocket socket) {
    closesocket(socket);
}

void disableUdpConnectionReset(UdpSocket::NativeSocket socket) {
    BOOL disabled = FALSE;
    DWORD bytes_returned = 0;
    WSAIoctl(
        socket,
        SIO_UDP_CONNRESET,
        &disabled,
        sizeof(disabled),
        nullptr,
        0,
        &bytes_returned,
        nullptr,
        nullptr);
}
#else
constexpr UdpSocket::NativeSocket kInvalidSocket = -1;

int socketLastError() {
    return errno;
}

void closeSocket(UdpSocket::NativeSocket socket) {
    close(socket);
}

void disableUdpConnectionReset(UdpSocket::NativeSocket) {}
#endif

UdpEndpoint endpointFromSockaddr(const sockaddr_in& address) {
    std::array<char, INET_ADDRSTRLEN> host{};
    const auto* converted = inet_ntop(AF_INET, &address.sin_addr, host.data(), host.size());
    return UdpEndpoint{
        converted == nullptr ? std::string{} : std::string(host.data()),
        ntohs(address.sin_port)};
}

bool sockaddrFromEndpoint(const UdpEndpoint& endpoint, sockaddr_in& address) {
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    return inet_pton(AF_INET, endpoint.host.c_str(), &address.sin_addr) == 1;
}

}  // namespace

std::string UdpEndpoint::id() const {
    std::ostringstream out;
    out << host << ':' << port;
    return out.str();
}

bool UdpEndpoint::operator==(const UdpEndpoint& other) const {
    return host == other.host && port == other.port;
}

UdpSocket::UdpSocket()
    : socket_(kInvalidSocket) {}

UdpSocket::~UdpSocket() {
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : socket_(std::exchange(other.socket_, kInvalidSocket)),
      last_error_(std::move(other.last_error_)) {}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = std::exchange(other.socket_, kInvalidSocket);
        last_error_ = std::move(other.last_error_);
    }

    return *this;
}

bool UdpSocket::bind(const std::string& host, std::uint16_t port) {
    close();

#ifdef _WIN32
    if (!winsockRuntime().initialized()) {
        last_error_ = "WSAStartup failed";
        return false;
    }
#endif

    socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == kInvalidSocket) {
        setErrorFromSocketApi("socket");
        return false;
    }

    disableUdpConnectionReset(socket_);

    sockaddr_in address{};
    if (!sockaddrFromEndpoint(UdpEndpoint{host, port}, address)) {
        last_error_ = "invalid bind address: " + host;
        close();
        return false;
    }

    if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        setErrorFromSocketApi("bind");
        close();
        return false;
    }

    last_error_.clear();
    return true;
}

std::optional<UdpReceiveResult> UdpSocket::receive(std::size_t max_packet_size) {
    if (!isOpen()) {
        last_error_ = "socket is not open";
        return std::nullopt;
    }

    std::vector<std::uint8_t> buffer(max_packet_size);
    sockaddr_in sender{};
#ifdef _WIN32
    int sender_size = sizeof(sender);
#else
    socklen_t sender_size = sizeof(sender);
#endif

    const auto received = ::recvfrom(
        socket_,
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0,
        reinterpret_cast<sockaddr*>(&sender),
        &sender_size);

    if (received < 0) {
        setErrorFromSocketApi("recvfrom");
        return std::nullopt;
    }

    buffer.resize(static_cast<std::size_t>(received));
    return UdpReceiveResult{std::move(buffer), endpointFromSockaddr(sender)};
}

std::optional<UdpReceiveResult> UdpSocket::receiveWithTimeout(
    std::size_t max_packet_size,
    int timeout_ms) {
    if (!isOpen()) {
        last_error_ = "socket is not open";
        return std::nullopt;
    }

    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(socket_, &read_set);

    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    const auto ready = ::select(
        static_cast<int>(socket_ + 1),
        &read_set,
        nullptr,
        nullptr,
        timeout_ms < 0 ? nullptr : &timeout);

    if (ready < 0) {
        setErrorFromSocketApi("select");
        return std::nullopt;
    }

    if (ready == 0) {
        last_error_.clear();
        return std::nullopt;
    }

    return receive(max_packet_size);
}

bool UdpSocket::sendTo(const UdpEndpoint& endpoint, const std::vector<std::uint8_t>& bytes) {
    if (!isOpen()) {
        last_error_ = "socket is not open";
        return false;
    }

    sockaddr_in address{};
    if (!sockaddrFromEndpoint(endpoint, address)) {
        last_error_ = "invalid destination address: " + endpoint.id();
        return false;
    }

    const auto sent = ::sendto(
        socket_,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()),
        0,
        reinterpret_cast<const sockaddr*>(&address),
        sizeof(address));

    if (sent < 0 || static_cast<std::size_t>(sent) != bytes.size()) {
        setErrorFromSocketApi("sendto");
        return false;
    }

    return true;
}

bool UdpSocket::isOpen() const noexcept {
    return socket_ != kInvalidSocket;
}

std::string UdpSocket::lastError() const {
    return last_error_;
}

void UdpSocket::close() noexcept {
    if (socket_ != kInvalidSocket) {
        closeSocket(socket_);
        socket_ = kInvalidSocket;
    }
}

void UdpSocket::setErrorFromSocketApi(const std::string& prefix) {
    std::ostringstream out;
    out << prefix << " failed with error " << socketLastError();
    last_error_ = out.str();
}

}  // namespace sfu
