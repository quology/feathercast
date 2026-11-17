#include "relay_output.hpp"
#include <obs.h>
#include <util/platform.h>
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/packet.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/log.h>
}
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace feathercast::engine {
namespace {
using namespace std::chrono_literals;
std::int64_t nowUs() { return static_cast<std::int64_t>(os_gettime_ns() / 1000); }
struct Relay;
std::mutex registryMutex;
std::map<obs_output_t*, Relay*> registry;

struct Relay {
    obs_output_t* output;
    core::DelayBuffer buffer;
    std::string endpoint;
    std::atomic<bool> stopping{false}, capturing{false}, connected{false};
    std::atomic<std::int64_t> deadline{0};
    std::atomic<std::uint64_t> bytes{0}, reconnects{0};
    std::atomic<int> lastError{0};
    std::thread worker;
    std::mutex wakeMutex;
    std::condition_variable wake;
    AVFormatContext* mux{};
    AVStream* video{};
    AVStream* audio{};
    bool wroteHeader{};

    Relay(obs_output_t* out, obs_data_t* settings)
        : output(out), buffer(core::DelayLimits{
            static_cast<std::size_t>(std::clamp<int64_t>(obs_data_get_int(settings, "memory_mb"), 16, 2048)) * 1024 * 1024,
            100000, 300'000'000, 310'000'000}) {
        buffer.set_target_delay(std::clamp<int64_t>(obs_data_get_int(settings, "delay_ms"), 0, 300000) * 1000);
    }
    static int interrupt(void* ptr) {
        auto& self = *static_cast<Relay*>(ptr);
        return self.stopping || (self.deadline && nowUs() > self.deadline);
    }
    void setDeadline() { deadline = nowUs() + 5'000'000; }
    bool codecData(AVCodecParameters* parameters, obs_encoder_t* encoder) {
        uint8_t* data = nullptr; size_t size = 0;
        if (!obs_encoder_get_extra_data(encoder, &data, &size) || !data || size == 0 || size > 1024 * 1024) return false;
        parameters->extradata = static_cast<uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!parameters->extradata) return false;
        std::memcpy(parameters->extradata, data, size);
        parameters->extradata_size = static_cast<int>(size);
        return true;
    }
    bool openMux() {
        int result = avformat_alloc_output_context2(&mux, nullptr, "flv", endpoint.c_str());
        if (result < 0 || !mux) { lastError = result; return false; }
        mux->interrupt_callback = {interrupt, this};
        mux->max_interleave_delta = 250000;
        mux->flags |= AVFMT_FLAG_FLUSH_PACKETS;
        auto* ve = obs_output_get_video_encoder(output);
        auto* ae = obs_output_get_audio_encoder(output, 0);
        video = avformat_new_stream(mux, nullptr); audio = avformat_new_stream(mux, nullptr);
        if (!video || !audio || !ve || !ae) { lastError = AVERROR(ENOMEM); return false; }
        video->time_base = {1, 1000};
        video->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        video->codecpar->codec_id = AV_CODEC_ID_H264;
        video->codecpar->width = static_cast<int>(obs_encoder_get_width(ve));
        video->codecpar->height = static_cast<int>(obs_encoder_get_height(ve));
        audio->time_base = {1, static_cast<int>(obs_encoder_get_sample_rate(ae))};
        audio->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        audio->codecpar->codec_id = AV_CODEC_ID_AAC;
        audio->codecpar->sample_rate = static_cast<int>(obs_encoder_get_sample_rate(ae));
        av_channel_layout_default(&audio->codecpar->ch_layout, 2);
        if (!codecData(video->codecpar, ve) || !codecData(audio->codecpar, ae)) { lastError = AVERROR_INVALIDDATA; return false; }
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "rw_timeout", "5000000", 0);
        setDeadline();
        result = avio_open2(&mux->pb, endpoint.c_str(), AVIO_FLAG_WRITE, &mux->interrupt_callback, &opts);
        av_dict_free(&opts);
        if (result < 0) { lastError = result; return false; }
        av_dict_set(&opts, "flvflags", "no_duration_filesize", 0);
        setDeadline();
        result = avformat_write_header(mux, &opts);
        av_dict_free(&opts);
        if (result < 0) { lastError = result; return false; }
        wroteHeader = true; connected = true; deadline = 0; lastError = 0;
        return true;
    }
    void closeMux() {
        connected = false;
        if (!mux) return;
        setDeadline();
        if (wroteHeader && !stopping) av_write_trailer(mux);
        if (mux->pb) avio_closep(&mux->pb);
        avformat_free_context(mux);
        mux = nullptr; video = audio = nullptr; wroteHeader = false; deadline = 0;
    }
    bool write(core::DelayedPacket& delayed) {
        auto& packet = delayed.packet;
        auto* stream = packet.kind == core::MediaKind::Video ? video : audio;
        AVPacket* p = av_packet_alloc();
        if (!p) { lastError = AVERROR(ENOMEM); return false; }
        if (packet.data.size() > INT_MAX || av_new_packet(p, static_cast<int>(packet.data.size())) < 0) {
            av_packet_free(&p); lastError = AVERROR(ENOMEM); return false;
        }
        std::memcpy(p->data, packet.data.data(), packet.data.size());
        p->stream_index = stream->index;
        p->dts = av_rescale_q(packet.dts_us, {1, 1000000}, stream->time_base);
        p->pts = av_rescale_q(packet.pts_us, {1, 1000000}, stream->time_base);
        p->duration = av_rescale_q(packet.duration_us, {1, 1000000}, stream->time_base);
        if (packet.keyframe) p->flags |= AV_PKT_FLAG_KEY;
        setDeadline();
        const int result = av_interleaved_write_frame(mux, p);
        av_packet_free(&p);
        deadline = 0;
        if (result < 0) { lastError = result; return false; }
        bytes += packet.data.size();
        return true;
    }
    void run() {
        // An output connection is created once per retry. File destinations must
        // never be reopened/truncated after a failure.
        const bool network = endpoint.starts_with("rtmp://") || endpoint.starts_with("rtmps://");
        unsigned failures = 0;
        bool everConnected = false;
        while (!stopping) {
            if (!openMux()) {
                closeMux();
                if (!network || ++failures > 10) break;
            } else {
                failures = 0;
                if (!capturing) {
                    if (!obs_output_begin_data_capture(output, 0)) { lastError = AVERROR_EXTERNAL; break; }
                    capturing = true;
                }
                if (everConnected) { buffer.reconnect(core::ReconnectPolicy::KeepBuffered); ++reconnects; }
                everConnected = true;
                while (!stopping) {
                    auto packet = buffer.pop_ready(nowUs());
                    if (packet) { if (!write(*packet)) break; }
                    else { std::unique_lock lock(wakeMutex); wake.wait_for(lock, 2ms); }
                }
                buffer.disconnect();
                closeMux();
                if (!network) break;
            }
            if (!stopping) { std::unique_lock lock(wakeMutex); wake.wait_for(lock, 2s, [&] { return stopping.load(); }); }
        }
        closeMux();
        if (capturing.exchange(false)) obs_output_end_data_capture(output);
        if (!stopping) {
            obs_output_set_last_error(output, "Relay connection or encoder failed; endpoint details suppressed");
            obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED);
        }
    }
};

const char* getName(void*) { return "FeatherCast dynamic delay relay"; }
void* create(obs_data_t* settings, obs_output_t* output) {
    try {
        auto* relay = new Relay(output, settings);
        std::lock_guard lock(registryMutex); registry.emplace(output, relay); return relay;
    } catch (...) { return nullptr; }
}
void destroy(void* ptr) {
    auto* relay = static_cast<Relay*>(ptr);
    { std::lock_guard lock(registryMutex); registry.erase(relay->output); }
    relay->stopping = true; relay->wake.notify_all();
    if (relay->worker.joinable()) relay->worker.join();
    delete relay;
}
bool start(void* ptr) {
    auto& relay = *static_cast<Relay*>(ptr);
    if (relay.worker.joinable()) relay.worker.join();
    if (!obs_output_can_begin_data_capture(relay.output, 0) || !obs_output_initialize_encoders(relay.output, 0)) return false;
    obs_data_t* settings = obs_output_get_settings(relay.output);
    relay.endpoint = obs_data_get_string(settings, "endpoint");
    obs_data_release(settings);
    if (relay.endpoint.empty()) return false;
    relay.buffer.reset(); relay.buffer.reconnect(core::ReconnectPolicy::DiscardBuffered);
    relay.stopping = false; relay.bytes = 0; relay.reconnects = 0; relay.lastError = 0;
    try { relay.worker = std::thread([&relay] { try { relay.run(); } catch (...) {
        relay.lastError = AVERROR_EXTERNAL;
        relay.closeMux();
        if (relay.capturing.exchange(false)) obs_output_end_data_capture(relay.output);
        obs_output_signal_stop(relay.output, OBS_OUTPUT_ERROR);
    } }); } catch (...) { return false; }
    return true;
}
void stop(void* ptr, uint64_t) {
    auto& relay = *static_cast<Relay*>(ptr);
    relay.stopping = true; relay.wake.notify_all();
    // Worker owns muxer and completes capture. Never join on callback thread.
}
void encoded(void* ptr, encoder_packet* packet) {
    auto& relay = *static_cast<Relay*>(ptr);
    if (relay.stopping) return;
    if (!packet) { relay.lastError = AVERROR_INVALIDDATA; relay.stopping = true; relay.wake.notify_all(); return; }
    try {
        if (packet->size > 16U * 1024U * 1024U || packet->timebase_den <= 0 || packet->timebase_num <= 0) {
            relay.lastError = AVERROR_INVALIDDATA; relay.stopping = true; relay.wake.notify_all(); return;
        }
        core::EncodedPacket p;
        p.kind = packet->type == OBS_ENCODER_VIDEO ? core::MediaKind::Video : core::MediaKind::Audio;
        p.track = packet->type == OBS_ENCODER_VIDEO ? 0u : static_cast<unsigned>(packet->track_idx + 1);
        p.dts_us = av_rescale_q(packet->dts, {packet->timebase_num, packet->timebase_den}, {1, 1000000});
        p.pts_us = av_rescale_q(packet->pts, {packet->timebase_num, packet->timebase_den}, {1, 1000000});
        // OBS AAC exposes encoder priming at -1024 samples. It precedes the
        // video epoch and is not playable program content. Omit it before the
        // bounded queue rather than treating it as a corrupt/disordered packet.
        if (packet->type == OBS_ENCODER_AUDIO && p.dts_us < 0) return;
        p.duration_us = packet->type == OBS_ENCODER_VIDEO
            ? av_rescale_q(1, {packet->timebase_num, packet->timebase_den}, {1, 1000000})
            : av_rescale_q(static_cast<int64_t>(obs_encoder_get_frame_size(packet->encoder)),
                           {1, static_cast<int>(obs_encoder_get_sample_rate(packet->encoder))}, {1, 1000000});
        p.keyframe = packet->keyframe;
        p.data.assign(packet->data, packet->data + packet->size);
        const auto result = relay.buffer.push(std::move(p), nowUs());
        if (result == core::PushResult::RejectedInvalid) {
            if (relay.lastError != AVERROR_INVALIDDATA)
                blog(LOG_WARNING, "FeatherCast rejected packet: kind=%d track=%zu dts=%lld pts=%lld num=%d den=%d size=%zu", static_cast<int>(packet->type), packet->track_idx, static_cast<long long>(packet->dts), static_cast<long long>(packet->pts), packet->timebase_num, packet->timebase_den, packet->size);
            relay.lastError = AVERROR_INVALIDDATA;
        }
        relay.wake.notify_one();
    } catch (...) { relay.lastError = AVERROR(ENOMEM); relay.stopping = true; relay.wake.notify_all(); }
}
uint64_t totalBytes(void* ptr) { return static_cast<Relay*>(ptr)->bytes.load(); }
}
void registerRelayOutput() {
    // FFmpeg protocol diagnostics can include full URLs. The host exposes only
    // numeric transport errors and never forwards these credential-bearing logs.
    av_log_set_callback([](void*, int, const char*, va_list) {});
    obs_output_info info{};
    info.id = "feathercast_relay"; info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED;
    info.get_name = getName; info.create = create; info.destroy = destroy;
    info.start = start; info.stop = stop; info.encoded_packet = encoded;
    // Intentional A/V cuts are exposed in RelayStats, not OBS's network dropped
    // video-frame metric (which would miscount audio and mislabel user cuts).
    info.get_total_bytes = totalBytes;
    info.encoded_video_codecs = "h264"; info.encoded_audio_codecs = "aac";
    obs_register_output(&info);
}
bool setRelayDelay(obs_output_t* output, std::int64_t microseconds) {
    std::lock_guard lock(registryMutex);
    auto found = registry.find(output);
    return found != registry.end() && found->second->buffer.set_target_delay(microseconds);
}
RelayStats relayStats(obs_output_t* output) {
    std::lock_guard lock(registryMutex);
    auto found = registry.find(output);
    if (found == registry.end()) return {};
    auto& r = *found->second;
    return {r.buffer.stats(), r.bytes, r.reconnects, r.lastError, r.connected};
}
} // namespace feathercast::engine
