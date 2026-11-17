#include "feathercast/core/delay_buffer.hpp"

#include <algorithm>
#include <stdexcept>

namespace feathercast::core {
namespace {
// Restrict externally supplied arithmetic to a safe, far larger than practical
// time domain (over 36,000 years). Rebased output is checked against it as well.
constexpr Microseconds time_limit = Microseconds{1} << 60;
bool valid_time(Microseconds value) { return value >= 0 && value < time_limit; }
bool is_key(const EncodedPacket& p) { return p.kind == MediaKind::Video && p.keyframe; }
}

DelayBuffer::DelayBuffer(DelayLimits limits) : limits_(limits) {
    if (!limits.max_payload_bytes || !limits.max_packets ||
        !valid_time(limits.max_delay_us) || !valid_time(limits.max_buffer_us) ||
        limits.max_buffer_us < limits.max_delay_us)
        throw std::invalid_argument("Invalid delay limits");
    input_track_dts_.fill(-1);
}

bool DelayBuffer::set_target_delay(Microseconds target_us) {
    std::lock_guard lock(mutex_);
    if (target_us < 0 || target_us > limits_.max_delay_us) return false;
    if (target_us < stats_.target_delay_us) catch_up_ = true;
    // Increasing supersedes an uncompleted request to catch up.
    if (target_us > stats_.target_delay_us) catch_up_ = false;
    stats_.target_delay_us = target_us;
    return true;
}

void DelayBuffer::discard_front() {
    stats_.payload_bytes -= queue_.front().packet.data.capacity();
    queue_.pop_front();
    ++stats_.dropped_packets;
}

void DelayBuffer::clear_queue() {
    stats_.dropped_packets += queue_.size();
    queue_.clear();
    stats_.payload_bytes = 0;
}

void DelayBuffer::require_keyframe() {
    clear_queue();
    stats_.waiting_for_keyframe = true;
    rebase_ = true;
    paced_ = false;
}

void DelayBuffer::trim_to_keyframe() {
    while (!queue_.empty() && !is_key(queue_.front().packet)) discard_front();
    stats_.waiting_for_keyframe = queue_.empty();
    rebase_ = true;
    paced_ = false;
}

PushResult DelayBuffer::push(EncodedPacket packet, Microseconds arrival_us) {
    std::lock_guard lock(mutex_);
    const bool track_ok = (packet.kind == MediaKind::Video && packet.track == 0) ||
                          (packet.kind == MediaKind::Audio && packet.track >= 1 && packet.track <= 8);
    if (!track_ok || !valid_time(packet.dts_us) || !valid_time(packet.pts_us) ||
        !valid_time(packet.duration_us) || !valid_time(arrival_us) || packet.data.empty() ||
        packet.dts_us < input_dts_ || arrival_us < input_arrival_ ||
        (track_ok && packet.dts_us <= input_track_dts_[packet.track]) ||
        // Low-latency contract excludes negative composition offsets.
        packet.pts_us < packet.dts_us) {
        require_keyframe();
        ++stats_.dropped_packets;
        return PushResult::RejectedInvalid;
    }
    input_dts_ = packet.dts_us;
    input_arrival_ = arrival_us;
    input_track_dts_[packet.track] = packet.dts_us;
    if (packet.data.capacity() > limits_.max_payload_bytes) {
        require_keyframe();
        ++stats_.dropped_packets;
        return PushResult::RejectedOversize;
    }
    bool overflow = queue_.size() >= limits_.max_packets ||
                    packet.data.capacity() > limits_.max_payload_bytes - stats_.payload_bytes ||
                    (!queue_.empty() && packet.dts_us - queue_.front().packet.dts_us > limits_.max_buffer_us);
    if (overflow) {
        ++stats_.overflow_events;
        // Fail closed rather than output frames dependent on a dropped packet.
        require_keyframe();
    }
    if (stats_.waiting_for_keyframe && !is_key(packet)) {
        ++stats_.dropped_packets;
        return PushResult::WaitingForKeyframe;
    }
    // Commit accounting after allocation succeeds, maintaining invariants on OOM.
    queue_.push_back(Entry{std::move(packet), arrival_us});
    stats_.payload_bytes += queue_.back().packet.data.capacity();
    stats_.waiting_for_keyframe = false;
    return overflow ? PushResult::AcceptedAfterOverflow : PushResult::Accepted;
}

std::optional<DelayedPacket> DelayBuffer::pop_ready(Microseconds now_us) {
    std::lock_guard lock(mutex_);
    if (!valid_time(now_us) || now_us < last_poll_) return std::nullopt;
    last_poll_ = now_us;
    if (!stats_.connected || queue_.empty()) return std::nullopt;

    if (catch_up_) {
        std::size_t chosen = 0;
        for (std::size_t i = 0; i < queue_.size(); ++i) {
            const auto& entry = queue_[i];
            if (entry.arrival_us + stats_.target_delay_us > now_us) break;
            if (is_key(entry.packet)) chosen = i;
        }
        if (chosen > 0) {
            while (chosen--) discard_front();
            rebase_ = true;
            paced_ = false;
            catch_up_ = false;
        } else if (queue_.front().arrival_us + stats_.target_delay_us >= now_us) {
            // Already at the target; there is no backlog to cut.
            catch_up_ = false;
        }
    }

    const auto& head = queue_.front();
    if (head.arrival_us + stats_.target_delay_us > now_us) return std::nullopt;
    // Pace source intervals after a cut/reconnect and after slow network sends.
    // Do not burst an entire GOP to chase an inexact target.
    if (paced_ && pace_wall_ + (head.packet.dts_us - pace_source_) > now_us)
        return std::nullopt;

    const bool discontinuity = rebase_;
    if (rebase_) {
        source_anchor_ = head.packet.dts_us;
        output_anchor_ = output_end_;
    }
    const auto dts = head.packet.dts_us - source_anchor_ + output_anchor_;
    const auto pts = head.packet.pts_us - source_anchor_ + output_anchor_;
    if (!valid_time(dts) || !valid_time(pts) || pts + head.packet.duration_us >= time_limit) {
        require_keyframe();
        return std::nullopt;
    }
    DelayedPacket result{std::move(queue_.front().packet), head.packet.dts_us, discontinuity};
    // Do not rely on moved-from packet fields for source timestamps.
    result.original_dts_us = result.packet.dts_us;
    stats_.payload_bytes -= result.packet.data.capacity();
    stats_.effective_delay_us = now_us - head.arrival_us;
    queue_.pop_front();
    // Keep a stable segment clock: resetting to every actual release time
    // accumulates timer granularity once per audio/video packet. Reanchor only
    // after a substantial stall (including a deliberate delay increase).
    if (!paced_ || now_us - (pace_wall_ + result.original_dts_us - pace_source_) > 100'000) {
        pace_source_ = result.original_dts_us;
        pace_wall_ = now_us;
    }
    paced_ = true;
    rebase_ = false;
    result.packet.dts_us = dts;
    result.packet.pts_us = pts;
    // A microsecond minimum separates a cut even when packet duration is unknown.
    output_end_ = std::max(output_end_, pts + std::max<Microseconds>(1, result.packet.duration_us));
    return result;
}

void DelayBuffer::disconnect() {
    std::lock_guard lock(mutex_);
    stats_.connected = false;
}

void DelayBuffer::reset_output() {
    source_anchor_ = output_anchor_ = output_end_ = pace_source_ = pace_wall_ = 0;
    rebase_ = true;
    paced_ = false;
    stats_.effective_delay_us = 0;
}

void DelayBuffer::reconnect(ReconnectPolicy policy) {
    std::lock_guard lock(mutex_);
    if (policy == ReconnectPolicy::DiscardBuffered) require_keyframe();
    else trim_to_keyframe();
    reset_output(); // New muxer/connection: timestamp zero is intentional.
    stats_.connected = true;
}

void DelayBuffer::reset() {
    std::lock_guard lock(mutex_);
    require_keyframe();
    reset_output();
    input_track_dts_.fill(-1);
    input_dts_ = input_arrival_ = last_poll_ = -1;
    catch_up_ = false;
}

DelayStats DelayBuffer::stats() const {
    std::lock_guard lock(mutex_);
    auto result = stats_;
    result.packets = queue_.size();
    if (queue_.size() > 1) result.queued_span_us = queue_.back().packet.dts_us - queue_.front().packet.dts_us;
    return result;
}
} // namespace feathercast::core
