#include "sfu/room.hpp"
#include "sfu/rtp_packet.hpp"
#include "sfu/transport.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

void rtpPacketRoundTrips() {
    const auto packet = sfu::RtpPacket::create(
        96,
        345,
        90000,
        42,
        std::vector<std::uint8_t>{1, 2, 3, 4},
        true);

    const auto parsed = sfu::RtpPacket::parse(packet.serialize());
    assert(parsed.has_value());
    assert(parsed->payloadType() == 96);
    assert(parsed->sequenceNumber() == 345);
    assert(parsed->timestamp() == 90000);
    assert(parsed->ssrc() == 42);
    assert(parsed->marker());
    assert(parsed->payload() == std::vector<std::uint8_t>({1, 2, 3, 4}));
    assert(parsed->mediaKind() == sfu::MediaKind::Video);
}

void invalidRtpIsRejected() {
    assert(!sfu::RtpPacket::parse({}).has_value());
    assert(!sfu::RtpPacket::parse(std::vector<std::uint8_t>(12, 0)).has_value());
}

void roomForwardsToOtherParticipants() {
    sfu::Room room("test");
    std::vector<std::uint16_t> bob_sequences;
    std::vector<std::uint16_t> carol_sequences;

    auto alice = std::make_shared<sfu::Participant>("alice");
    auto bob = std::make_shared<sfu::Participant>(
        "bob",
        [&](const sfu::RtpPacket& packet) {
            bob_sequences.push_back(packet.sequenceNumber());
        });
    auto carol = std::make_shared<sfu::Participant>(
        "carol",
        [&](const sfu::RtpPacket& packet) {
            carol_sequences.push_back(packet.sequenceNumber());
        });

    assert(room.addParticipant(alice));
    assert(room.addParticipant(bob));
    assert(room.addParticipant(carol));
    assert(!room.addParticipant(std::make_shared<sfu::Participant>("alice")));

    const auto packet = sfu::RtpPacket::create(111, 7, 160, 1001, {9, 9});
    const auto result = room.publishRtp("alice", packet);

    assert(result.recipients == 2);
    assert(result.dropped == 0);
    assert(bob_sequences == std::vector<std::uint16_t>({7}));
    assert(carol_sequences == std::vector<std::uint16_t>({7}));

    const auto metrics = room.metrics();
    assert(metrics.totals.packets_received == 1);
    assert(metrics.totals.packets_forwarded == 2);
    assert(metrics.per_participant.at("alice").packets_received == 1);
    assert(metrics.per_participant.at("bob").packets_forwarded == 1);
    assert(metrics.per_participant.at("carol").packets_forwarded == 1);
}

void unknownPublisherIsDropped() {
    sfu::Room room("test");
    const auto packet = sfu::RtpPacket::create(96, 1, 1, 1, {1});
    const auto result = room.publishRtp("missing", packet);

    assert(result.recipients == 0);
    assert(result.dropped == 1);
    assert(room.metrics().totals.packets_dropped == 1);
}

void rawTransportIngressForwardsSerializedRtp() {
    sfu::Room room("test");
    sfu::RoomRtpIngress ingress(room);
    std::vector<std::vector<std::uint8_t>> bob_packets;

    auto alice = std::make_shared<sfu::Participant>("alice");
    auto bob = std::make_shared<sfu::Participant>(
        "bob",
        sfu::makeRtpPacketSender(
            [&](const std::vector<std::uint8_t>& bytes) {
                bob_packets.push_back(bytes);
            }));

    assert(room.addParticipant(alice));
    assert(room.addParticipant(bob));

    const auto packet = sfu::RtpPacket::create(96, 81, 123456, 789, {5, 6, 7});
    const auto result = ingress.receiveRawRtp("alice", packet.serialize());

    assert(result.status == sfu::InboundRtpStatus::Forwarded);
    assert(result.forwarding.recipients == 1);
    assert(result.forwarding.dropped == 0);
    assert(bob_packets.size() == 1);

    const auto parsed = sfu::RtpPacket::parse(bob_packets.front());
    assert(parsed.has_value());
    assert(parsed->sequenceNumber() == 81);
    assert(parsed->ssrc() == 789);
    assert(parsed->payload() == std::vector<std::uint8_t>({5, 6, 7}));
}

void rawTransportIngressRejectsInvalidRtp() {
    sfu::Room room("test");
    sfu::RoomRtpIngress ingress(room);

    auto alice = std::make_shared<sfu::Participant>("alice");
    assert(room.addParticipant(alice));

    const auto result = ingress.receiveRawRtp("alice", {1, 2, 3});

    assert(result.status == sfu::InboundRtpStatus::InvalidPacket);
    assert(result.forwarding.recipients == 0);
    assert(result.forwarding.dropped == 0);
    assert(room.metrics().totals.packets_received == 0);
}

void rawTransportIngressReportsUnknownPublisher() {
    sfu::Room room("test");
    sfu::RoomRtpIngress ingress(room);

    const auto packet = sfu::RtpPacket::create(96, 1, 1, 1, {1});
    const auto result = ingress.receiveRawRtp("missing", packet.serialize());

    assert(result.status == sfu::InboundRtpStatus::UnknownPublisher);
    assert(result.forwarding.recipients == 0);
    assert(result.forwarding.dropped == 1);
    assert(room.metrics().totals.packets_dropped == 1);
}

void activeSpeakerTracksRecentRtpActivity() {
    sfu::Room room("test");
    auto alice = std::make_shared<sfu::Participant>("alice");
    auto bob = std::make_shared<sfu::Participant>("bob");

    assert(room.addParticipant(alice));
    assert(room.addParticipant(bob));
    assert(!room.activeSpeakerId().has_value());

    room.publishRtp("alice", sfu::RtpPacket::create(111, 1, 160, 1001, {1}));
    assert(room.activeSpeakerId().has_value());
    assert(*room.activeSpeakerId() == "alice");

    room.publishRtp("bob", sfu::RtpPacket::create(111, 2, 320, 2002, {1, 2, 3, 4, 5}));
    assert(room.activeSpeakerId().has_value());
    assert(*room.activeSpeakerId() == "bob");

    const auto activity = room.participantActivity();
    assert(activity.size() == 2);
    assert(activity[0].participant_id == "alice");
    assert(activity[1].participant_id == "bob");
    assert(!activity[0].active_speaker);
    assert(activity[1].active_speaker);
}

void inactiveParticipantsCanBeRemoved() {
    sfu::Room room("test");
    assert(room.addParticipant(std::make_shared<sfu::Participant>("alice")));
    assert(room.addParticipant(std::make_shared<sfu::Participant>("bob")));
    assert(room.addParticipant(std::make_shared<sfu::Participant>("carol")));

    room.publishRtp("alice", sfu::RtpPacket::create(96, 1, 1, 1, {1}));
    room.publishRtp("alice", sfu::RtpPacket::create(96, 2, 2, 1, {1}));

    const auto removed = room.removeInactiveParticipants(0);
    assert(removed == std::vector<std::string>({"bob", "carol"}));
    assert(room.participantCount() == 1);
    assert(room.findParticipant("alice") != nullptr);
    assert(room.findParticipant("bob") == nullptr);
    assert(room.findParticipant("carol") == nullptr);
}

}  // namespace

int main() {
    rtpPacketRoundTrips();
    invalidRtpIsRejected();
    roomForwardsToOtherParticipants();
    unknownPublisherIsDropped();
    rawTransportIngressForwardsSerializedRtp();
    rawTransportIngressRejectsInvalidRtp();
    rawTransportIngressReportsUnknownPublisher();
    activeSpeakerTracksRecentRtpActivity();
    inactiveParticipantsCanBeRemoved();
    return 0;
}
