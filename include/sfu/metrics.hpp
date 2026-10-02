#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace sfu {

struct StreamMetrics {
    std::uint64_t packets_received = 0;
    std::uint64_t packets_forwarded = 0;
    std::uint64_t bytes_received = 0;
    std::uint64_t bytes_forwarded = 0;
    std::uint64_t packets_dropped = 0;
};

struct RoomMetricsSnapshot {
    StreamMetrics totals;
    std::unordered_map<std::string, StreamMetrics> per_participant;
};

class MetricsRegistry {
public:
    void recordReceived(const std::string& participant_id, std::uint64_t bytes);
    void recordForwarded(const std::string& participant_id, std::uint64_t bytes);
    void recordDropped(const std::string& participant_id);

    RoomMetricsSnapshot snapshot() const;

private:
    StreamMetrics totals_;
    std::unordered_map<std::string, StreamMetrics> per_participant_;
};

}  // namespace sfu
