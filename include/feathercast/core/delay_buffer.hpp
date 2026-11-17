#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace feathercast::core {

using Microseconds = std::int64_t;
enum class MediaKind { Video, Audio };

// Adapter owns codec headers. A keyframe must be a closed-GOP random access point.
// DTS/PTS use a common, nonnegative microsecond epoch. Stream 0 is video;
// audio tracks are 1..8. Input is globally ordered by DTS, strictly per track.
struct EncodedPacket {
    MediaKind kind{MediaKind::Video};
    unsigned track{};
    Microseconds dts_us{};
    Microseconds pts_us{};
    Microseconds duration_us{};
    bool keyframe{};
    std::vector<std::uint8_t> data;
};

struct DelayLimits {
    std::size_t max_payload_bytes{256U * 1024U * 1024U};
    std::size_t max_packets{100000};
    Microseconds max_delay_us{300'000'000};
    // Extra queue duration accommodates keyframe granularity and scheduling.
    Microseconds max_buffer_us{310'000'000};
};

enum class PushResult { Accepted, AcceptedAfterOverflow, WaitingForKeyframe,
                        RejectedOversize, RejectedInvalid };
enum class ReconnectPolicy { KeepBuffered, DiscardBuffered };

struct DelayedPacket {
    EncodedPacket packet;
    Microseconds original_dts_us{};
    // First packet of a new session or keyframe cut. Adapter may need headers.
    bool discontinuity{};
};

struct DelayStats {
    std::size_t payload_bytes{}; // vector capacities, not just sizes
    std::size_t packets{};
    Microseconds target_delay_us{};
    Microseconds queued_span_us{};
    Microseconds effective_delay_us{}; // last emitted packet's age on release
    std::uint64_t dropped_packets{};
    std::uint64_t overflow_events{};
    bool waiting_for_keyframe{true};
    bool connected{true};
};

// Thread-safe bounded queue. No network work or caller callbacks under its lock.
// Exactly one consumer calls pop_ready; network failure requires disconnect().
// now_us is steady-clock time from one epoch, nonnegative and nondecreasing.
class DelayBuffer {
public:
    explicit DelayBuffer(DelayLimits limits = {});
    bool set_target_delay(Microseconds target_us);
    PushResult push(EncodedPacket packet, Microseconds arrival_us);
    std::optional<DelayedPacket> pop_ready(Microseconds now_us);
    void disconnect();
    void reconnect(ReconnectPolicy policy = ReconnectPolicy::KeepBuffered);
    // Full new encoder session: clears input and output timestamp history.
    void reset();
    DelayStats stats() const;

private:
    struct Entry { EncodedPacket packet; Microseconds arrival_us; };
    void discard_front();
    void clear_queue();
    void require_keyframe();
    void trim_to_keyframe();
    void reset_output();
    const DelayLimits limits_;
    mutable std::mutex mutex_;
    std::deque<Entry> queue_;
    DelayStats stats_;
    std::array<Microseconds, 9> input_track_dts_{};
    Microseconds input_dts_{-1};
    Microseconds input_arrival_{-1};
    Microseconds last_poll_{-1};
    Microseconds source_anchor_{};
    Microseconds output_anchor_{};
    Microseconds output_end_{};
    Microseconds pace_source_{};
    Microseconds pace_wall_{};
    bool rebase_{true};
    bool catch_up_{};
    bool paced_{};
};

} // namespace feathercast::core
