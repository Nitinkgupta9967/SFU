#include "sfu/metrics.hpp"

namespace sfu {

void MetricsRegistry::recordReceived(const std::string& participant_id, std::uint64_t bytes) {
    ++totals_.packets_received;
    totals_.bytes_received += bytes;

    auto& stream = per_participant_[participant_id];
    ++stream.packets_received;
    stream.bytes_received += bytes;
}

void MetricsRegistry::recordForwarded(const std::string& participant_id, std::uint64_t bytes) {
    ++totals_.packets_forwarded;
    totals_.bytes_forwarded += bytes;

    auto& stream = per_participant_[participant_id];
    ++stream.packets_forwarded;
    stream.bytes_forwarded += bytes;
}

void MetricsRegistry::recordDropped(const std::string& participant_id) {
    ++totals_.packets_dropped;
    ++per_participant_[participant_id].packets_dropped;
}

RoomMetricsSnapshot MetricsRegistry::snapshot() const {
    return RoomMetricsSnapshot{totals_, per_participant_};
}

}  // namespace sfu
