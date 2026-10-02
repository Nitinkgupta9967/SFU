#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sfu {

struct UdpEndpoint {
    std::string host;
    std::uint16_t port = 0;

    std::string id() const;
    bool operator==(const UdpEndpoint& other) const;
};

struct UdpReceiveResult {
    std::vector<std::uint8_t> bytes;
    UdpEndpoint sender;
};

class UdpSocket {
public:
#ifdef _WIN32
    using NativeSocket = std::uintptr_t;
#else
    using NativeSocket = int;
#endif

    UdpSocket();
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    bool bind(const std::string& host, std::uint16_t port);
    std::optional<UdpReceiveResult> receive(std::size_t max_packet_size);
    std::optional<UdpReceiveResult> receiveWithTimeout(
        std::size_t max_packet_size,
        int timeout_ms);
    bool sendTo(const UdpEndpoint& endpoint, const std::vector<std::uint8_t>& bytes);

    bool isOpen() const noexcept;
    std::string lastError() const;

private:
    void close() noexcept;
    void setErrorFromSocketApi(const std::string& prefix);

    NativeSocket socket_;
    std::string last_error_;
};

}  // namespace sfu
