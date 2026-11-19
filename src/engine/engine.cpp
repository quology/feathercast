#include "feathercast/engine/engine.hpp"
#include "feathercast/core/plugin_api.h"
#include "relay_output.hpp"
#include "microphone_filters.hpp"
#include <obs.h>
#include <obs-audio-controls.h>
#include <graphics/matrix4.h>
#include <type_traits>
#include <QSet>
#include <util/platform.h>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QThread>
#include <QUuid>
#include <QRegularExpression>
#include <QUrl>
#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <wincrypt.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace feathercast::engine {
namespace {
std::atomic<bool> engineOwned{false};
// OBS group bounds rebase child positions and the group origin during render.
// Take/edit the whole root-scene layout under the same recursive scene lock.
// The callback is synchronous; no borrowed callback state survives this call.
template<class F> void sceneAtomic(obs_scene_t* scene, F&& action) {
    obs_scene_atomic_update(scene, [](void* data, obs_scene_t*) {
        (*static_cast<std::remove_reference_t<F>*>(data))();
    }, &action);
}
// OBS33's first render marks scene_size_changed. A group with pending local
// offsets then recenters its children without translating the group origin.
// Initialize the EMPTY scene first, before adding any capture sources or layout.
// This is a private target and cannot activate audio or change program output.
bool initializeSceneCanvas(obs_scene_t* scene) {
    obs_enter_graphics();
    auto* target = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    const bool started = target && gs_texrender_begin(target, 1, 1);
    if (started) { obs_source_video_render(obs_scene_get_source(scene)); gs_texrender_end(target); }
    gs_texrender_destroy(target); obs_leave_graphics();
    return started;
}
bool validTransform(const QJsonObject& patch);
QString normalizeVideoFilters(QJsonArray& chain);
QString applyVideoFilters(obs_source_t* source, const QJsonArray& before, const QJsonArray& after);
const char* sourceId(const QString& type) {
    if (type == "desktop") return "monitor_capture";
    if (type == "window") return "window_capture";
    if (type == "game") return "game_capture";
    if (type == "color") return "color_source_v3";
    if (type == "microphone") return "wasapi_input_capture";
    if (type == "desktop_audio") return "wasapi_output_capture";
    if (type == "application_audio") return "wasapi_process_output_capture";
    if (type == "test_tone") return "feathercast_test_tone";
    return nullptr;
}
bool hasSourceAudio(const QString& type, const QJsonObject& properties) {
    return type == "microphone" || type == "desktop_audio" || type == "application_audio" || type == "test_tone" ||
        ((type == "window" || type == "game" || type == "desktop") && properties.value("capture_audio").toBool());
}
QJsonObject defaultAudio() {
    return {{"volume", 1.0}, {"muted", false}, {"mono", false}, {"balance", 0.5},
            {"sync_offset_ms", 0}, {"monitoring", "off"}, {"tracks", 1}, {"scope", "scene"}, {"mixer_hidden", false}};
}
QString audioPatchError(const QJsonObject& patch) {
    const auto defaults = defaultAudio();
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const auto key = it.key();
        const auto v = it.value();
        bool valid = false;
        if (key == "volume" || key == "balance")
            valid = v.isDouble() && v.toDouble() >= 0 && v.toDouble() <= (key == "volume" ? 2.0 : 1.0);
        else if (key == "tracks" || key == "sync_offset_ms") {
            const int n = v.toInt();
            valid = v.isDouble() && v.toDouble() == n &&
                    (key == "tracks" ? n >= 0 && n <= 63 : n >= -950 && n <= 20000);
        } else if (key == "scope") valid = v == "scene" || v == "global";
        else if (key == "monitoring")
            valid = v == "off" || v == "monitor_only" || v == "monitor_and_output";
        else if (defaults.value(key).isBool()) valid = v.isBool();
        if (!valid) return "Invalid audio property: " + key;
    }
    return {};
}
int trackMask(const QJsonObject& source) {
    return source["properties"].toObject()["audio"].toObject()["tracks"].toInt();
}
// Reserve a free mixer only for the legacy isolated-track shortcut. Manual
// assignments explicitly select any combination of the six shared mixers.
QString normalizeAudio(const QString& type, QJsonObject& properties, int used,
                       const QJsonObject& legacyGain = {}) {
    if (!hasSourceAudio(type, properties)) return {};
    auto patch = properties.value("audio").toObject();
    if (!properties.contains("audio")) patch = legacyGain;
    auto audio = defaultAudio();
    if (!patch.contains("tracks") && properties.value("separate_audio_track").toBool()) {
        int track = properties.value("recording_track").toInt();
        if (track && (used & (1 << (track - 1)))) return "Separate recording track is already in use";
        if (!track) {
            for (int i = 2; i <= 6; ++i) if (!(used & (1 << (i - 1)))) { track = i; break; }
        }
        if (!track) return "No free track for isolated recording audio";
        audio["tracks"] = 1 | (1 << (track - 1));
    }
    for (auto it = patch.begin(); it != patch.end(); ++it) audio[it.key()] = it.value();
    if (const auto error = audioPatchError(audio); !error.isEmpty()) return error;
    properties.remove("separate_audio_track");
    properties.remove("recording_track");
    properties["audio"] = audio;
    return {};
}
void applyAudio(obs_source_t* source, const QJsonObject& audio, const QJsonObject& before = {}) {
    const auto changed = [&](const char* key) { return audio[key] != before[key]; };
    // Mute/gain and the effective routing mask must be in place before the
    // source starts monitoring or becomes active through its first scene item.
    if (changed("muted")) obs_source_set_muted(source, audio["muted"].toBool());
    if (changed("volume")) obs_source_set_volume(source, float(audio["volume"].toDouble()));
    if (changed("mono")) {
        const auto flags = obs_source_get_flags(source);
        obs_source_set_flags(source, audio["mono"].toBool() ? flags | OBS_SOURCE_FLAG_FORCE_MONO : flags & ~OBS_SOURCE_FLAG_FORCE_MONO);
    }
    if (changed("balance")) obs_source_set_balance_value(source, float(audio["balance"].toDouble()));
    if (changed("sync_offset_ms")) obs_source_set_sync_offset(source, qint64(audio["sync_offset_ms"].toInt()) * 1000000);
    if (changed("tracks") || changed("monitoring"))
        obs_source_set_audio_mixers(source, audio["monitoring"] == "monitor_only" ? 0 : audio["tracks"].toInt());
    if (changed("monitoring")) obs_source_set_monitoring_enabled(source, audio["monitoring"] != "off");
}
bool validWindowTarget(const QString& target) {
    const auto parts = target.split(':');
    return parts.size() == 3 && !parts[1].isEmpty() && !parts[2].isEmpty();
}
QString audioSettingsError(const QString& type, const QJsonObject& properties) {
    if (properties.contains("audio")) {
        if (properties["audio"].toObject()["scope"] == "global" && (type == "window" || type == "game")) return "Global scope requires an audio-only source (use Application Audio for window/game audio)";
        if (!hasSourceAudio(type, properties) || !properties["audio"].isObject()) return "Expected audio properties on an audio-capable source";
        if (const auto error = audioPatchError(properties["audio"].toObject()); !error.isEmpty()) return error;
    }
    for (const auto* key : {"capture_audio", "separate_audio_track"})
        if (properties.contains(key) && !properties[key].isBool()) return "Audio capture and separate track options must be Boolean";
    if (properties.value("capture_audio").toBool()) {
        if (type != "desktop" && type != "window" && type != "game") return "Audio capture is not supported for this source type";
        if (type == "desktop" && !validWindowTarget(properties.value("audio_window").toString())) return "Select an application for display audio";
        if (type == "window" && !validWindowTarget(properties.value("window").toString())) return "Select a window for application audio";
        const bool selectedGameWindow = properties.value("capture_mode").toString() == "window" ||
            (!properties.contains("capture_mode") && !properties.value("window").toString().isEmpty());
        if (type == "game" && selectedGameWindow && !validWindowTarget(properties.value("window").toString())) return "Select a game window for application audio";
    }
    if (type == "application_audio" && !validWindowTarget(properties.value("window").toString())) return "Select an application for audio capture";
    if (properties.value("separate_audio_track").toBool() && !hasSourceAudio(type, properties)) return "A separate recording track requires source audio";
    if (properties.contains("recording_track")) {
        const auto track = properties.value("recording_track").toInt(-1);
        if (!properties.value("separate_audio_track").toBool() || track < 2 || track > 6 || properties.value("recording_track").toDouble() != track)
            return "Invalid separate recording track (use tracks 2 through 6)";
    }
    return {};
}
obs_data_t* obsSettings(const QJsonObject& object) {
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return obs_data_create_from_json(bytes.constData());
}
QString vaultPath() {
    const auto directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(directory);
    return directory + "/stream-key.dpapi";
}
// The only synthetic media source is an explicit test tone. It is never enabled
// automatically, and real device capture is separately chosen by the user.
struct Tone {
    obs_source_t* source;
    std::atomic<bool> stop{false};
    std::thread worker;
    explicit Tone(obs_source_t* s, double frequency) : source(s) {
        worker = std::thread([this, frequency] {
            float samples[480]; std::uint64_t cursor = 0;
            auto next = std::chrono::steady_clock::now();
            const auto epoch = os_gettime_ns();
            while (!stop) {
                for (unsigned i = 0; i < 480; ++i) samples[i] = static_cast<float>(0.08 * std::sin(2.0 * 3.141592653589793 * frequency * static_cast<double>(cursor + i) / 48000.0));
                obs_source_audio frame{};
                frame.data[0] = reinterpret_cast<uint8_t*>(samples);
                frame.data[1] = reinterpret_cast<uint8_t*>(samples);
                frame.frames = 480; frame.speakers = SPEAKERS_STEREO;
                frame.format = AUDIO_FORMAT_FLOAT_PLANAR; frame.samples_per_sec = 48000;
                frame.timestamp = epoch + cursor * 1000000000ULL / 48000;
                obs_source_output_audio(source, &frame);
                cursor += 480; next += std::chrono::milliseconds(10); std::this_thread::sleep_until(next);
            }
        });
    }
    ~Tone() { stop = true; if (worker.joinable()) worker.join(); }
};
void registerTone() {
    obs_source_info info{};
    info.id = "feathercast_test_tone"; info.type = OBS_SOURCE_TYPE_INPUT; info.output_flags = OBS_SOURCE_AUDIO;
    info.get_name = [](void*) { return "Test tone (440 Hz)"; };
    info.create = [](obs_data_t* settings, obs_source_t* source) -> void* {
        const auto frequency = obs_data_get_double(settings, "frequency_hz");
        try { return new Tone(source, frequency >= 20 && frequency <= 20000 ? frequency : 440); } catch (...) { return nullptr; }
    };
    info.destroy = [](void* data) { delete static_cast<Tone*>(data); };
    obs_register_source(&info);
}
bool active(obs_output_t* output) { return output && (obs_output_active(output) || obs_output_reconnecting(output)); }
obs_sceneitem_t* publicSceneItem(obs_scene_t* scene, const QString& name) {
    struct Search { QByteArray name; obs_sceneitem_t* item{}; } search{name.toUtf8()};
    obs_scene_enum_items(scene, [](obs_scene_t*, obs_sceneitem_t* item, void* data) {
        auto& search = *static_cast<Search*>(data);
        auto* source = obs_sceneitem_get_source(item);
        if (!obs_obj_is_private(source) && search.name == obs_source_get_name(source)) { search.item = item; return false; }
        if (obs_sceneitem_is_group(item)) {
            search.item = publicSceneItem(obs_sceneitem_group_get_scene(item), QString::fromUtf8(search.name));
            if (search.item) return false;
        }
        return true;
    }, &search);
    return search.item;
}
}

// Callback data stays alive until OBS has detached the meter and removed its callback.
struct AudioMeter {
    obs_volmeter_t* meter{};
    std::atomic<float> peak{-100.0f};
    std::atomic<std::uint64_t> updated{};
    static void update(void* data, const float*, const float* peaks, const float*) {
        auto* self = static_cast<AudioMeter*>(data);
        float value = -100.0f;
        for (int i = 0; i < 2; ++i) if (std::isfinite(peaks[i])) value = std::max(value, peaks[i]);
        self->peak.store(value, std::memory_order_relaxed);
        self->updated.store(os_gettime_ns(), std::memory_order_release);
    }
    explicit AudioMeter(obs_source_t* source) {
        meter = obs_volmeter_create(OBS_FADER_LOG);
        if (meter) { obs_volmeter_add_callback(meter, update, this); obs_volmeter_attach_source(meter, source); }
    }
    ~AudioMeter() {
        if (meter) { obs_volmeter_detach_source(meter); obs_volmeter_remove_callback(meter, update, this); obs_volmeter_destroy(meter); }
    }
    double reading() const {
        const auto stamp = updated.load(std::memory_order_acquire);
        return stamp && os_gettime_ns() - stamp < 1000000000ULL ? peak.load(std::memory_order_relaxed) : -100.0;
    }
};
struct Engine::Impl {
    Engine* q;
    bool ready{};
    QString runtime;
    QString binaryDirectory;
    QString previousWorkingDirectory;
    QJsonObject config{Engine::defaultSettings()};
    struct Scene { obs_scene_t* scene{}; QJsonArray sources; };
    QMap<QString, Scene> scenes;
    std::map<QString, std::unique_ptr<AudioMeter>> meters;
    QMap<QString, obs_source_t*> audioOwners; // Borrowed from the owning scene items.
    bool monitoringConfigured{};
    // Borrowed scene items; the owning scene keeps each private companion alive.
    QMap<QString, obs_sceneitem_t*> audioCompanions;
    QString selected, program;
    QStringList sceneOrder;
    bool staging{};
    std::atomic<bool> previewSeparate{};
    obs_scene_t* globalAudio{};
    QMap<QString, obs_sceneitem_t*> globalItems;
    void refreshGlobalAudio() {
        if (staging || !globalAudio) return;
        QMap<QString, obs_source_t*> desired;
        for (const auto& scene : scenes) for (auto value : scene.sources) {
            auto row = value.toObject(); const auto name = row["name"].toString();
            if (row["properties"].toObject()["audio"].toObject()["scope"] == "global")
                if (auto* source = audioOwners.value(name)) desired[name] = source;
        }
        for (auto it = globalItems.begin(); it != globalItems.end();) {
            if (desired.value(it.key()) != obs_sceneitem_get_source(it.value())) { obs_sceneitem_remove(it.value()); it = globalItems.erase(it); }
            else ++it;
        }
        for (auto it = desired.begin(); it != desired.end(); ++it) if (!globalItems.contains(it.key())) globalItems[it.key()] = obs_scene_add(globalAudio, it.value());
    }
    obs_view_t* previewView{};
    obs_source_t* transition{};
    obs_display_t* programDisplay{};
    os_cpu_usage_info_t* cpu{};
    uint64_t cpuSampleNs{};
    double cpuPercent{};
    std::atomic<uint64_t> streamStarted{}, recordingStarted{}, replayStarted{};
    int row(const Scene& scene, const QString& name) const {
        for (int i = 0; i < scene.sources.size(); ++i) if (scene.sources[i].toObject()["name"] == name) return i;
        return -1;
    }
    bool locked(const Scene& scene, QString name) const {
        while (!name.isEmpty()) {
            const int i = row(scene, name); if (i < 0) break;
            auto entry = scene.sources[i].toObject();
            if (entry["locked"].toBool()) return true;
            name = entry["parent"].toString();
        }
        return false;
    }
    void route() {
        if (staging) return;
        if (previewView) obs_view_set_source(previewView, 0, scenes.contains(selected) ? obs_scene_get_source(scenes[selected].scene) : nullptr);
        if (transition) {
            obs_transition_set(transition, scenes.contains(program) ? obs_scene_get_source(scenes[program].scene) : nullptr);
            obs_set_output_source(0, transition);
        }
    }
    obs_display_t* display{};
    obs_encoder_t* video{};
    obs_encoder_t* audio{};
    std::vector<obs_encoder_t*> recordingAudio;
    obs_output_t* stream{};
    obs_output_t* recording{};
    obs_output_t* replay{};
    QString recordingPath;
    std::uint64_t startedNs{};
    DLL_DIRECTORY_COOKIE dllDirectory{};
    struct Plugin { HMODULE library{}; const fc_plugin_descriptor* descriptor{}; void* context{}; };
    std::vector<Plugin> plugins;
    fc_host_api host{};

    explicit Impl(Engine* owner) : q(owner) {
        host = {sizeof(fc_host_api), FC_PLUGIN_ABI_VERSION, owner,
            [](void* ctx, int, const char* message) { if (message) emit static_cast<Engine*>(ctx)->logMessage(QString::fromUtf8(message)); },
            [](void* ctx, int64_t ms) -> int { return static_cast<Engine*>(ctx)->setDelayMs(ms) ? 1 : 0; }};
    }
    bool fail(const QString& message) { emit q->errorOccurred(message); return false; }
    bool outputsActive() const { return active(stream) || active(recording) || active(replay); }
    void notify() { QMetaObject::invokeMethod(q, [owner = q] { emit owner->stateChanged(); }, Qt::QueuedConnection); }
    static void outputEvent(void* data, calldata_t* cd) {
        auto* self = static_cast<Impl*>(data);
        auto* output = static_cast<obs_output_t*>(calldata_ptr(cd, "output"));
        const auto now = os_gettime_ns();
        if (output == self->stream) self->streamStarted = now;
        if (output == self->recording) self->recordingStarted = now;
        if (output == self->replay) self->replayStarted = now;
        self->notify();
    }
    static void outputStop(void* data, calldata_t* cd) {
        auto* self = static_cast<Impl*>(data);
        const auto code = calldata_int(cd, "code");
        QMetaObject::invokeMethod(self->q, [owner = self->q, code] {
            if (code != 0) emit owner->errorOccurred(QString("Output stopped with error code %1").arg(code));
            emit owner->stateChanged();
        }, Qt::QueuedConnection);
    }
    static void replayEvent(void* data, calldata_t*) {
        auto* self = static_cast<Impl*>(data);
        QMetaObject::invokeMethod(self->q, [owner = self->q] {
            if (!owner->d->replay) return;
            calldata_t cd; calldata_init(&cd);
            proc_handler_call(obs_output_get_proc_handler(owner->d->replay), "get_last_replay", &cd);
            const auto path = QString::fromUtf8(calldata_string(&cd, "path"));
            calldata_free(&cd);
            emit owner->replaySaved(path);
        }, Qt::QueuedConnection);
    }
    void connectOutput(obs_output_t* output, bool isReplay = false) {
        auto* handler = obs_output_get_signal_handler(output);
        signal_handler_connect(handler, "start", outputEvent, this);
        signal_handler_connect(handler, "stop", outputStop, this);
        if (isReplay) signal_handler_connect(handler, "saved", replayEvent, this);
    }
    void releaseOutput(obs_output_t*& output) {
        if (!output) return;
        auto* handler = obs_output_get_signal_handler(output);
        signal_handler_disconnect(handler, "start", outputEvent, this);
        signal_handler_disconnect(handler, "stop", outputStop, this);
        signal_handler_disconnect(handler, "saved", replayEvent, this);
        if (active(output)) obs_output_force_stop(output);
        obs_output_release(output); output = nullptr;
    }
    void releaseEncoders() {
        releaseOutput(stream); releaseOutput(recording); releaseOutput(replay);
        releaseRecordingAudio();
        if (video) { obs_encoder_release(video); video = nullptr; }
        if (audio) { obs_encoder_release(audio); audio = nullptr; }
    }
    void releaseRecordingAudio() {
        for (auto* encoder : recordingAudio) obs_encoder_release(encoder);
        recordingAudio.clear();
    }
    int usedTracks() const {
        int mask = 0;
        for (const auto& scene : scenes) for (auto value : scene.sources) mask |= trackMask(value.toObject());
        return mask;
    }
    bool configureVideo() {
        obs_video_info vi{};
        const auto module = (binaryDirectory + "/libobs-d3d11.dll").toUtf8();
        vi.graphics_module = module.constData();
        vi.fps_num = static_cast<uint32_t>(config["fps"].toInt()); vi.fps_den = 1;
        vi.base_width = vi.output_width = static_cast<uint32_t>(config["width"].toInt());
        vi.base_height = vi.output_height = static_cast<uint32_t>(config["height"].toInt());
        vi.output_format = VIDEO_FORMAT_NV12; vi.adapter = 0; vi.gpu_conversion = true;
        vi.colorspace = VIDEO_CS_709; vi.range = VIDEO_RANGE_PARTIAL; vi.scale_type = OBS_SCALE_BILINEAR;
        const int result = obs_reset_video(&vi);
        return result == OBS_VIDEO_SUCCESS || fail(QString("D3D11 video initialization failed (%1)").arg(result));
    }
    bool ensureEncoders() {
        if (video && audio) return true;
        if (!ready) return fail("Engine is not initialized");
        auto* settings = obs_data_create();
        obs_data_set_int(settings, "bitrate", config["videoBitrateKbps"].toInt());
        obs_data_set_int(settings, "keyint_sec", config["keyframeSeconds"].toInt());
        obs_data_set_string(settings, "rate_control", "CBR");
        obs_data_set_string(settings, "profile", "high");
        obs_data_set_int(settings, "bf", 0);
        obs_data_set_bool(settings, "lookahead", false);
        const auto chosen = config["encoder"].toString();
        if (chosen != "obs_x264") {
            obs_data_set_string(settings, "preset", "p4");
            obs_data_set_string(settings, "tune", "ll");
            obs_data_set_string(settings, "multipass", "disabled");
            const auto id = chosen == "auto" ? QByteArray("obs_nvenc_h264_tex") : chosen.toUtf8();
            video = obs_video_encoder_create(id.constData(), "FeatherCast shared H264", settings, nullptr);
        }
        if (!video) {
            obs_data_set_string(settings, "preset", "veryfast");
            obs_data_set_string(settings, "tune", "zerolatency");
            obs_data_set_string(settings, "x264opts", "bframes=0 open-gop=0");
            video = obs_video_encoder_create("obs_x264", "FeatherCast shared H264", settings, nullptr);
        }
        obs_data_release(settings);
        settings = obs_data_create();
        obs_data_set_int(settings, "bitrate", config["audioBitrateKbps"].toInt());
        audio = obs_audio_encoder_create("ffmpeg_aac", "Mixed audio", settings, 0, nullptr);
        obs_data_release(settings);
        if (!video || !audio) { releaseEncoders(); return fail("No compatible H.264/AAC encoders available"); }
        obs_encoder_set_video(video, obs_get_video()); obs_encoder_set_audio(audio, obs_get_audio());
        return true;
    }
    void attachEncoders(obs_output_t* output) {
        obs_output_set_video_encoder(output, video); obs_output_set_audio_encoder(output, audio, 0);
    }
    static void drawProgram(void* data, uint32_t width, uint32_t height) { drawCanvas(static_cast<Impl*>(data), width, height, false); }
    static void draw(void* data, uint32_t width, uint32_t height) { drawCanvas(static_cast<Impl*>(data), width, height, true); }
    static void drawCanvas(Impl* self, uint32_t width, uint32_t height, bool preview) {
        obs_video_info info{};
        if (!obs_get_video_info(&info)) return;
        const auto baseWidth = info.base_width, baseHeight = info.base_height;
        if (!baseWidth || !baseHeight || !width || !height) return;
        const auto scale = std::min(static_cast<double>(width) / baseWidth, static_cast<double>(height) / baseHeight);
        const int w = static_cast<int>(baseWidth * scale), h = static_cast<int>(baseHeight * scale);
        gs_viewport_push(); gs_projection_push();
        gs_set_viewport((static_cast<int>(width) - w) / 2, (static_cast<int>(height) - h) / 2, w, h);
        gs_ortho(0.0f, static_cast<float>(baseWidth), 0.0f, static_cast<float>(baseHeight), -100.0f, 100.0f);
        if (preview && self->previewSeparate.load(std::memory_order_relaxed) && self->previewView) obs_view_render(self->previewView);
        else obs_render_main_texture();
        gs_projection_pop(); gs_viewport_pop();
    }
    void detachProgram() {
        if (programDisplay) { obs_display_remove_draw_callback(programDisplay, drawProgram, this); obs_display_destroy(programDisplay); programDisplay = nullptr; }
    }
    void detachPreview() {
        if (display) { obs_display_remove_draw_callback(display, draw, this); obs_display_destroy(display); display = nullptr; }
    }
};

Engine::Engine(QObject* parent) : QObject(parent), d(std::make_unique<Impl>(this)) {}
Engine::~Engine() { shutdown(); }
QJsonObject Engine::defaultSettings() {
    return {{"width", 1280}, {"height", 720}, {"fps", 60}, {"encoder", "auto"},
        {"videoBitrateKbps", 6000}, {"audioBitrateKbps", 160}, {"keyframeSeconds", 2},
        {"outputDirectory", QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + "/VELA"},
        {"streamEndpoint", ""}, {"delayMs", 0}, {"delayMemoryMb", 256},
        {"studioMode", false}, {"transition", "fade"}, {"transitionDurationMs", 300}, {"replaySeconds", 30}, {"replayMemoryMb", 256}, {"previewEnabled", true}, {"replayEnabled", true}, {"pluginsEnabled", true}};
}
bool Engine::initialize(QString runtimeRoot, QJsonObject settings) {
    if (d->ready) return true;
    if (engineOwned.exchange(true)) return d->fail("Another libobs engine already owns this process");
    if (runtimeRoot.isEmpty()) {
        const auto appDir = QCoreApplication::applicationDirPath();
        runtimeRoot = QFileInfo::exists(appDir + "/core") ? appDir : appDir + "/obs";
#ifdef FEATHERCAST_DEV_RUNTIME
        if (!QFileInfo::exists(runtimeRoot + "/core")) runtimeRoot = QString::fromUtf8(FEATHERCAST_DEV_RUNTIME);
#endif
    }
    d->runtime = QDir::cleanPath(QFileInfo(runtimeRoot).absoluteFilePath());
    d->binaryDirectory = QFileInfo::exists(d->runtime + "/obs.dll") ? d->runtime : d->runtime + "/bin/64bit";
    if (!QFileInfo::exists(d->binaryDirectory + "/obs.dll")) { engineOwned = false; return d->fail("OBS runtime not found"); }
    if (!settings.isEmpty() && !applySettings(settings)) { engineOwned = false; return false; }
    const auto bin = QDir::toNativeSeparators(d->binaryDirectory);
    // Official win-capture initializes graphics hooks from ../../core relative
    // to cwd. Keep its native layout contract even when the frontend sits at
    // the root of a portable package; all application paths remain absolute.
    const auto compatibilityDirectory = d->runtime + "/bin/64bit";
    if (!QDir(compatibilityDirectory).exists()) { engineOwned = false; return d->fail("OBS runtime bin/64bit directory missing"); }
    d->previousWorkingDirectory = QDir::currentPath();
    QDir::setCurrent(compatibilityDirectory);
    d->dllDirectory = AddDllDirectory(reinterpret_cast<LPCWSTR>(bin.utf16()));
    qputenv("PATH", bin.toUtf8() + ';' + qgetenv("PATH"));
    if (!obs_startup("en-US", nullptr, nullptr)) { engineOwned = false; return d->fail("libobs startup failed"); }
    d->ready = true;
    obs_add_data_path((d->runtime + "/data/libobs/").toUtf8().constData());
    obs_audio_info ai{48000, SPEAKERS_STEREO};
    if (!obs_reset_audio(&ai) || !d->configureVideo()) { shutdown(); return false; }
    d->monitoringConfigured = obs_audio_monitoring_available() && obs_set_audio_monitoring_device("Default", "default");
    const QStringList modules{"image-source", "obs-x264", "obs-ffmpeg", "obs-nvenc", "obs-qsv11", "win-capture", "win-wasapi", "obs-transitions", "obs-filters"};
    for (const auto& name : modules) {
        const auto directory = d->runtime + "/core/" + name;
        const auto file = (directory + "/" + name + ".dll").toUtf8();
        const auto data = (directory + "/data").toUtf8();
        obs_module_t* module = nullptr;
        if (obs_open_module(&module, file.constData(), data.constData()) != MODULE_SUCCESS || !obs_init_module(module))
            emit logMessage("Optional OBS module unavailable: " + name);
    }
    obs_post_load_modules();
    registerRelayOutput(); registerTone();
    d->globalAudio = obs_scene_create_private("VELA global audio");
    obs_set_output_source(1, obs_scene_get_source(d->globalAudio));
    d->previewView = obs_view_create();
    d->transition = obs_source_create_private("fade_transition", "VELA program transition", nullptr);
    if (!d->previewView || !d->transition) { shutdown(); return d->fail("OBS preview/transition runtime unavailable"); }
    d->cpu = os_cpu_usage_info_start(); d->cpuSampleNs = os_gettime_ns(); d->cpuPercent = 0;
    d->startedNs = os_gettime_ns();
    emit stateChanged(); return true;
}
void Engine::shutdown() {
    for (auto it = d->plugins.rbegin(); it != d->plugins.rend(); ++it) {
        try { it->descriptor->shutdown(it->context); } catch (...) {}
        FreeLibrary(it->library);
    }
    d->plugins.clear();
    if (!d->ready) return;
    d->detachPreview(); d->detachProgram(); d->releaseEncoders();
    if (d->previewView) { obs_view_destroy(d->previewView); d->previewView = nullptr; }
    if (d->cpu) { os_cpu_usage_info_destroy(d->cpu); d->cpu = nullptr; }
    obs_set_output_source(1, nullptr);
    if (d->globalAudio) { obs_scene_release(d->globalAudio); d->globalAudio = nullptr; d->globalItems.clear(); }
    d->meters.clear();
    d->audioOwners.clear();
    d->audioCompanions.clear();
    obs_set_output_source(0, nullptr);
    if (d->transition) { obs_transition_clear(d->transition); obs_source_release(d->transition); d->transition = nullptr; }
    for (const auto& scene : d->scenes) {
        // OBS 33's program canvas owns a strong scene reference. Remove the
        // source from that canvas before releasing our own reference.
        obs_source_remove(obs_scene_get_source(scene.scene));
        obs_scene_release(scene.scene);
    }
    d->scenes.clear(); d->sceneOrder.clear(); d->selected.clear(); d->program.clear();
    // Source/output release is queued by libobs. Drain those destructors before
    // obs_shutdown force-destroys remaining sources, otherwise shutdown races
    // queued scene destruction and can double-destroy its child sources.
    while (obs_wait_for_destroy_queue()) {}
    obs_shutdown(); d->ready = false; engineOwned = false;
    if (!d->previousWorkingDirectory.isEmpty()) QDir::setCurrent(d->previousWorkingDirectory);
    if (d->dllDirectory) { RemoveDllDirectory(d->dllDirectory); d->dllDirectory = nullptr; }
}
bool Engine::initialized() const { return d->ready; }
QJsonObject Engine::settings() const { return d->config; }
bool Engine::applySettings(const QJsonObject& settings) {
    if (d->outputsActive()) return d->fail("Stop all outputs before changing encoder/video settings");
    auto candidate = d->config;
    for (auto it = settings.begin(); it != settings.end(); ++it) if (candidate.contains(it.key())) candidate[it.key()] = it.value();
    const auto range = [&](const char* key, int low, int high) { const auto v = candidate[key].toInt(-1); return v >= low && v <= high; };
    if (!range("width", 320, 7680) || !range("height", 180, 4320) || candidate["width"].toInt() % 2 || candidate["height"].toInt() % 2 ||
        !range("fps", 1, 240) || !range("videoBitrateKbps", 100, 200000) || !range("audioBitrateKbps", 32, 320) ||
        !range("keyframeSeconds", 1, 10) || !range("replaySeconds", 1, 300) || !range("replayMemoryMb", 16, 2048) ||
        !range("transitionDurationMs", 50, 20000) || (candidate["transition"] != "fade" && candidate["transition"] != "cut") || !candidate["studioMode"].isBool() ||
        !range("delayMs", 0, 300000) || !range("delayMemoryMb", 16, 2048)) return d->fail("Settings are outside supported limits");
    // Configuration must never persist user/password/query components as keys.
    const QUrl endpoint(candidate["streamEndpoint"].toString());
    if (!endpoint.userInfo().isEmpty() || endpoint.hasQuery() || endpoint.hasFragment()) return d->fail("Keep stream credentials in the protected key field");
    const auto old = d->config; d->config = candidate; d->previewSeparate = candidate["studioMode"].toBool();
    if (d->ready) {
        d->releaseEncoders();
        if (!d->configureVideo()) { d->config = old; d->previewSeparate = old["studioMode"].toBool(); d->configureVideo(); return false; }
    }
    emit stateChanged(); return true;
}
bool Engine::storeStreamKey(const QString& key) {
    const auto bytes = key.toUtf8(); DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(const_cast<char*>(bytes.constData()))}, output{};
    if (!CryptProtectData(&input, L"FeatherCast stream key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return d->fail("Windows could not protect the stream key");
    QSaveFile file(vaultPath());
    const bool success = file.open(QIODevice::WriteOnly) && file.write(reinterpret_cast<char*>(output.pbData), output.cbData) == output.cbData && file.commit();
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData);
    return success || d->fail("Could not save protected stream key");
}
QString Engine::streamKey() const {
    QFile file(vaultPath()); if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return {};
    auto bytes = file.readAll(); DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())}, output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) return {};
    const auto result = QString::fromUtf8(reinterpret_cast<char*>(output.pbData), static_cast<qsizetype>(output.cbData));
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); return result;
}
bool Engine::hasStreamKey() const { return !streamKey().isEmpty(); }
bool Engine::saveConfiguration(const QString& path) const {
    QJsonArray scenes;
    for (const auto& name : d->sceneOrder) scenes.append(QJsonObject{{"name", name}, {"sources", sources(name)}});
    const QJsonObject object{{"version", 1}, {"settings", d->config}, {"scenes", scenes}, {"selectedScene", d->selected}, {"programScene", d->program}};
    QSaveFile file(path); if (!file.open(QIODevice::WriteOnly)) return false;
    const auto bytes = QJsonDocument(object).toJson(); return file.write(bytes) == bytes.size() && file.commit();
}
bool Engine::loadConfiguration(const QString& path, const QJsonObject& legacySourceAudio) {
    QFile file(path); if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) return false;
    QJsonParseError error; const auto doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject() || doc["version"].toInt() != 1) return d->fail("Invalid configuration file");
    // Validate the entire scene graph before settings changes or destruction.
    if (!doc["settings"].isObject() || !doc["scenes"].isArray() || !doc["selectedScene"].isString()) return d->fail("Invalid configuration fields");
    auto savedScenes = doc["scenes"].toArray();
    if (savedScenes.size() > 128) return d->fail("Too many saved scenes");
    QSet<QString> sceneNames, sourceNames;
    int usedTracks = 0;
    for (auto value : savedScenes) {
        auto scene = value.toObject(); auto name = scene["name"].toString();
        if (!value.isObject() || name.trimmed().isEmpty() || name.size() > 128 || sceneNames.contains(name) || !scene["sources"].isArray() || scene["sources"].toArray().size() > 64) return d->fail("Invalid saved scene");
        sceneNames.insert(name);
        for (auto sourceValue : scene["sources"].toArray()) {
            auto source = sourceValue.toObject(); auto sourceName = source["name"].toString();
            if (!sourceValue.isObject() || sourceName.trimmed().isEmpty() || sourceName.size() > 128 || sourceNames.contains(sourceName) || (!sourceId(source["type"].toString()) && source["type"] != "group") || !source["properties"].isObject() || sceneNames.contains(sourceName)) return d->fail("Invalid saved source");
            if (sourceName.contains(QChar(0)) || name.contains(QChar(0)) ||
                (source.contains("parent") && !source["parent"].isString()) ||
                (source.contains("visible") && !source["visible"].isBool()) ||
                (source.contains("locked") && !source["locked"].isBool()) ||
                (source.contains("transform") && (!source["transform"].isObject() || !validTransform(source["transform"].toObject())))) return d->fail("Invalid source layout");
            sourceNames.insert(sourceName);
            const auto type = source["type"].toString();
            auto properties = source["properties"].toObject();
            if (const auto filterError = filters::normalizeSource(type, properties); !filterError.isEmpty()) return d->fail(filterError);
            if (properties.contains("video_filters")) {
                if (!properties["video_filters"].isArray()) return d->fail("Invalid video filters");
                auto chain = properties["video_filters"].toArray();
                if (const auto filterError = normalizeVideoFilters(chain); !filterError.isEmpty()) return d->fail(filterError);
            }
            const auto audioError = audioSettingsError(type, properties);
            if (!audioError.isEmpty()) return d->fail(audioError);
            if (d->ready && (properties.value("capture_audio").toBool() || type == "application_audio") && !sourceAudioAvailable(type))
                return d->fail("Application audio capture is unavailable in this runtime");
            // First reserve every explicit manual mask, regardless of file order.
            usedTracks |= properties["audio"].toObject()["tracks"].toInt();
        }
    }
    for (auto value : savedScenes) {
        QMap<QString, QJsonObject> entries;
        for (auto source : value.toObject()["sources"].toArray()) entries[source.toObject()["name"].toString()] = source.toObject();
        for (auto entry : entries) {
            QSet<QString> ancestors{entry["name"].toString()}; auto parent = entry["parent"].toString();
            while (!parent.isEmpty()) {
                if (ancestors.contains(parent) || !entries.contains(parent) || entries[parent]["type"] != "group") return d->fail("Invalid or cyclic group parent");
                ancestors.insert(parent); parent = entries[parent]["parent"].toString();
            }
        }
    }
    if (doc.object().contains("programScene") && !sceneNames.isEmpty() && !sceneNames.contains(doc["programScene"].toString())) return d->fail("Invalid program scene");
    for (const auto& name : sceneNames) if (sourceNames.contains(name)) return d->fail("Source and scene names must be unique");
    if (!sceneNames.isEmpty() && !sceneNames.contains(doc["selectedScene"].toString())) return d->fail("Invalid selected scene");
    // Reserve explicit legacy track numbers before allocating unnumbered shortcuts.
    // Normalize everything before changing settings or replacing the scene graph.
    for (bool explicitTrack : {true, false}) {
        for (qsizetype i = 0; i < savedScenes.size(); ++i) {
            auto scene = savedScenes[i].toObject(); auto sources = scene["sources"].toArray();
            for (qsizetype j = 0; j < sources.size(); ++j) {
                auto source = sources[j].toObject(); auto properties = source["properties"].toObject();
                if (properties.contains("recording_track") != explicitTrack) continue;
                const auto error = normalizeAudio(source["type"].toString(), properties, usedTracks,
                                                  legacySourceAudio[source["name"].toString()].toObject());
                if (!error.isEmpty()) return d->fail(error);
                source["properties"] = properties;
                usedTracks |= trackMask(source);
                sources[j] = source;
            }
            scene["sources"] = sources; savedScenes[i] = scene;
        }
    }
    const auto previousSettings = d->config;
    if (!applySettings(doc["settings"].toObject())) return false;
    if (!d->ready) return true;
    // Keep the old graph, meters and live view references until the candidate is
    // completely constructed. Temporary native names free the global namespace.
    auto oldScenes = std::move(d->scenes); auto oldOrder = d->sceneOrder;
    auto oldMeters = std::move(d->meters); auto oldOwners = std::move(d->audioOwners); auto oldCompanions = std::move(d->audioCompanions);
    const auto oldSelected = d->selected, oldProgram = d->program;
    std::vector<std::pair<obs_source_t*, QByteArray>> renamed;
    const auto prefix = QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8();
    auto hideName = [&](obs_source_t* source) {
        renamed.emplace_back(source, obs_source_get_name(source));
        obs_source_set_name(source, (prefix + QByteArray::number(renamed.size())).constData());
    };
    for (const auto& old : oldScenes) {
        // Resolve every child first: after group names change recursive lookup is still safe.
        for (auto row : old.sources) hideName(obs_sceneitem_get_source(publicSceneItem(old.scene, row.toObject()["name"].toString())));
        hideName(obs_scene_get_source(old.scene));
    }
    d->scenes.clear(); d->sceneOrder.clear(); d->selected.clear(); d->program.clear(); d->staging = true;
    const bool blocked = blockSignals(true);
    bool success = true;
    for (auto value : savedScenes) {
        auto scene = value.toObject(); const auto name = scene["name"].toString(); auto rows = scene["sources"].toArray();
        if (!addScene(name)) { success = false; break; }
        for (auto value : rows) { auto row = value.toObject(); if (!addSource(name, row["type"].toString(), row["name"].toString(), row["properties"].toObject())) { success = false; break; } }
        if (!success) break;
        // Parents are created before any moves. Move deepest children last, so
        // transforms are restored only after all native group resizing settles.
        QMap<QString, QJsonObject> entries; for (auto row : rows) entries[row.toObject()["name"].toString()] = row.toObject();
        for (int depth = 1; depth <= rows.size(); ++depth) for (auto value : rows) {
            auto row = value.toObject(); auto parent = row["parent"].toString(); int count = 0;
            for (auto p = parent; !p.isEmpty(); p = entries[p]["parent"].toString()) ++count;
            if (count == depth && !moveSource(name, row["name"].toString(), parent, 1000)) success = false;
        }
        // Restore canonical sibling order, including root items.
        for (auto value : rows) { auto row = value.toObject(); if (!moveSource(name, row["name"].toString(), row["parent"].toString(), 1000)) success = false; }
        for (auto value : rows) { auto row = value.toObject(); setSourceTransform(name, row["name"].toString(), row["transform"].toObject()); setSourceVisible(name, row["name"].toString(), row["visible"].toBool(true)); }
        for (auto value : rows) { auto row = value.toObject(); setSourceLocked(name, row["name"].toString(), row["locked"].toBool()); }
    }
    blockSignals(blocked); d->staging = false;
    if (!success) {
        d->meters.clear(); d->audioOwners.clear(); d->audioCompanions.clear();
        for (const auto& scene : d->scenes) { obs_source_remove(obs_scene_get_source(scene.scene)); obs_scene_release(scene.scene); }
        d->scenes.clear(); while (obs_wait_for_destroy_queue()) {}
        for (auto& [source, name] : renamed) obs_source_set_name(source, name.constData());
        d->scenes = std::move(oldScenes); d->sceneOrder = oldOrder;
        d->meters = std::move(oldMeters); d->audioOwners = std::move(oldOwners); d->audioCompanions = std::move(oldCompanions);
        d->selected = oldSelected; d->program = oldProgram; applySettings(previousSettings); d->route();
        return d->fail("Could not construct imported scene collection; previous collection restored");
    }
    d->selected = doc["selectedScene"].toString(); d->program = doc["programScene"].toString(d->selected); d->route(); d->refreshGlobalAudio();
    oldMeters.clear(); oldOwners.clear(); oldCompanions.clear();
    for (const auto& scene : oldScenes) { obs_source_remove(obs_scene_get_source(scene.scene)); obs_scene_release(scene.scene); }
    while (obs_wait_for_destroy_queue()) {}
    emit scenesChanged(); emit stateChanged(); return true;
}

QStringList Engine::scenes() const { return d->sceneOrder; }
QString Engine::currentScene() const { return d->selected; }
bool Engine::addScene(const QString& name) {
    if (!d->ready || name.trimmed().isEmpty() || name.size() > 128 || name.contains(QChar(0)) || d->scenes.size() >= 128 || d->scenes.contains(name)) return d->fail("Invalid or duplicate scene");
    if (auto* existing = obs_get_source_by_name(name.toUtf8().constData())) { obs_source_release(existing); return d->fail("Names must be unique"); }
    auto* scene = obs_scene_create(name.toUtf8().constData()); if (!scene) return d->fail("Could not create scene");
    if (!initializeSceneCanvas(scene)) { obs_source_remove(obs_scene_get_source(scene)); obs_scene_release(scene); return d->fail("Could not initialize scene canvas"); }
    d->scenes.insert(name, {scene, {}}); d->sceneOrder.append(name); if (d->selected.isEmpty()) selectScene(name);
    emit scenesChanged(); return true;
}
bool Engine::removeScene(const QString& name) {
    auto it = d->scenes.find(name); if (it == d->scenes.end()) return false;
    if (active(d->recording)) for (auto value : it->sources)
        if (trackMask(value.toObject()) & ~1) return d->fail("Stop recording before removing a scene assigned to additional tracks");
    const bool reroute = d->selected == name || d->program == name;
    d->sceneOrder.removeAll(name);
    if (d->selected == name) d->selected = d->sceneOrder.value(0);
    if (d->program == name) d->program = d->sceneOrder.value(0);
    if (reroute) d->route();
    for (auto source : it->sources) {
        const auto sourceName = source.toObject()["name"].toString();
        if (auto* global = d->globalItems.take(sourceName)) obs_sceneitem_remove(global);
        d->meters.erase(sourceName); d->audioOwners.remove(sourceName); d->audioCompanions.remove(sourceName);
    }
    obs_source_remove(obs_scene_get_source(it->scene)); obs_scene_release(it->scene); d->scenes.erase(it);
    if (d->selected.isEmpty() && !d->scenes.isEmpty()) selectScene(d->scenes.firstKey());
    emit scenesChanged(); return true;
}
bool Engine::selectScene(const QString& name) {
    auto found = d->scenes.find(name); if (found == d->scenes.end()) return false;
    const auto previous = d->selected; d->selected = name;
    if (d->staging) return true;
    obs_view_set_source(d->previewView, 0, obs_scene_get_source(found->scene));
    if (!studioMode() || d->program.isEmpty()) {
        if (takePreview()) return true;
        d->selected = previous;
        obs_view_set_source(d->previewView, 0, d->scenes.contains(previous) ? obs_scene_get_source(d->scenes[previous].scene) : nullptr);
        return false;
    }
    emit stateChanged(); return true;
}
bool Engine::addSource(const QString& scene, const QString& type, const QString& name, QJsonObject properties) {
    if (type == "group") {
        if (!addGroup(scene, name)) return false;
        if (properties.contains("video_filters") && !setVideoFilters(scene, name, properties["video_filters"].toArray())) { removeSource(scene, name); return false; }
        return true;
    }
    auto found = d->scenes.find(scene); const char* id = sourceId(type);
    if (found == d->scenes.end() || !id || name.trimmed().isEmpty() || name.size() > 128 || name.contains(QChar(0)) || found->sources.size() >= 64) return d->fail("Invalid source or scene");
    if (auto* existing = obs_get_source_by_name(name.toUtf8().constData())) { obs_source_release(existing); return d->fail("Source names must be unique"); }
    if (type == "game" && !properties.value("window").toString().isEmpty() && !properties.contains("capture_mode")) properties["capture_mode"] = "window";
    if (const auto error = filters::normalizeSource(type, properties); !error.isEmpty()) return d->fail(error);
    const auto audioError = audioSettingsError(type, properties);
    if (!audioError.isEmpty()) return d->fail(audioError);
    if ((properties.value("capture_audio").toBool() || type == "application_audio") && !sourceAudioAvailable(type))
        return d->fail("Application audio capture is unavailable in this runtime");
    if (const auto error = normalizeAudio(type, properties, d->usedTracks()); !error.isEmpty()) return d->fail(error);
    const auto audio = properties.value("audio").toObject();
    if (active(d->recording) && (audio["tracks"].toInt() & ~1)) return d->fail("Stop recording before adding a source assigned to additional tracks");
    if (type == "color") {
        if (!properties.contains("color")) properties["color"] = static_cast<double>(0xff7333eeu);
        if (!properties.contains("width")) properties["width"] = d->config["width"];
        if (!properties.contains("height")) properties["height"] = d->config["height"];
    }
    if (type == "desktop" && !properties.contains("monitor")) properties["monitor"] = 0;
    if (type == "game" && !properties.contains("capture_mode")) properties["capture_mode"] = "any";
    if ((type == "microphone" || type == "desktop_audio") && !properties.contains("device_id")) properties["device_id"] = "default";
    auto videoFilters = properties.value("video_filters").toArray();
    if (properties.contains("video_filters") && !properties["video_filters"].isArray()) return d->fail("Invalid video filters");
    if (const auto error = normalizeVideoFilters(videoFilters); !error.isEmpty()) return d->fail(error);
    if (!videoFilters.isEmpty()) properties["video_filters"] = videoFilters;
    auto captureProperties = properties; captureProperties.remove("audio"); captureProperties.remove("filters"); captureProperties.remove("video_filters");
    auto* settings = obsSettings(captureProperties);
    auto* source = obs_source_create(id, name.toUtf8().constData(), settings, nullptr); obs_data_release(settings);
    if (!source) return d->fail("Capture/source module could not create " + name);
    if (const auto error = filters::apply(source, {}, properties.value("filters").toArray()); !error.isEmpty()) {
        obs_source_release(source); return d->fail(error);
    }
    if (!videoFilters.isEmpty()) {
        if (!(obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO)) { obs_source_release(source); return d->fail("Video filters require video"); }
        if (const auto error = applyVideoFilters(source, {}, videoFilters); !error.isEmpty()) { obs_source_release(source); return d->fail(error); }
    }
    const bool useCompanion = type == "desktop" && properties.value("capture_audio").toBool();
    if (!audio.isEmpty() && !useCompanion) applyAudio(source, audio);
    else if (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) obs_source_set_audio_mixers(source, 0);
    auto* item = obs_scene_add(found->scene, source);
    if (!item) { obs_source_release(source); return d->fail("Could not attach source"); }
    if (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) {
        vec2 bounds{static_cast<float>(d->config["width"].toInt()), static_cast<float>(d->config["height"].toInt())};
        obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER); obs_sceneitem_set_bounds(item, &bounds);
    }
    obs_source_t* audioSource = source;
    if (useCompanion) {
        auto* audioSettings = obsSettings({{"window", properties.value("audio_window")}, {"priority", 1}});
        auto* companion = obs_source_create_private(sourceId("application_audio"), (name + " audio").toUtf8().constData(), audioSettings);
        obs_data_release(audioSettings);
        if (companion) applyAudio(companion, audio);
        auto* audioItem = companion ? obs_scene_add(found->scene, companion) : nullptr;
        if (!audioItem) {
            if (companion) obs_source_release(companion);
            obs_sceneitem_remove(item); obs_source_release(source);
            return d->fail("Could not attach application audio for " + name);
        }
        d->audioCompanions.insert(name, audioItem);
        audioSource = companion;
    }
    if (!audio.isEmpty()) {
        d->audioOwners.insert(name, audioSource);
        d->meters[name] = std::make_unique<AudioMeter>(audioSource);
    }
    if (audioSource != source) obs_source_release(audioSource);
    obs_source_release(source);
    found->sources.append(QJsonObject{{"type", type}, {"name", name}, {"properties", properties}});
    d->refreshGlobalAudio();
    emit scenesChanged(); return true;
}
bool Engine::removeSource(const QString& scene, const QString& name) {
    auto found = d->scenes.find(scene); if (found == d->scenes.end()) return false;
    auto* item = publicSceneItem(found->scene, name); if (!item) return false;
    if (d->locked(*found, name)) return d->fail("Unlock this source and its parent groups before editing");
    if (obs_sceneitem_is_group(item)) {
        // Deleting a group deletes its descendants, after validating every lock/track.
        QStringList descendants{name};
        for (int pass = 0; pass < found->sources.size(); ++pass) for (auto value : found->sources) {
            auto entry = value.toObject(); auto child = entry["name"].toString();
            if (descendants.contains(entry["parent"].toString()) && !descendants.contains(child)) descendants.append(child);
        }
        for (const auto& child : descendants) {
            auto entry = found->sources[d->row(*found, child)].toObject();
            if (d->locked(*found, child) || (active(d->recording) && (trackMask(entry) & ~1))) return d->fail("Group contains locked sources or recording tracks");
        }
        for (auto it = descendants.crbegin(); it != descendants.crend(); ++it) {
            if (*it != name && !removeSource(scene, *it)) return false;
        }
    }
    for (auto value : found->sources) {
        const auto s = value.toObject();
        if (s["name"].toString() == name && (trackMask(s) & ~1) && active(d->recording))
            return d->fail("Stop recording before removing a separate audio track");
    }
    if (auto* global = d->globalItems.take(name)) obs_sceneitem_remove(global);
    d->meters.erase(name);
    d->audioOwners.remove(name);
    if (auto* companion = d->audioCompanions.take(name)) obs_sceneitem_remove(companion);
    obs_sceneitem_remove(item);
    for (qsizetype i = 0; i < found->sources.size(); ++i) if (found->sources[i].toObject()["name"].toString() == name) { found->sources.removeAt(i); break; }
    emit scenesChanged(); return true;
}
QJsonArray Engine::sources(const QString& scene) const {
    auto it = d->scenes.find(scene); if (it == d->scenes.end()) return {};
    QJsonArray result; QMap<QString, int> positions;
    sceneAtomic(it->scene, [&] {
        for (auto value : it->sources) {
            auto entry = value.toObject(); const auto name = entry["name"].toString();
            entry["parent"] = entry["parent"].toString(); entry["order"] = positions[entry["parent"].toString()]++;
            if (auto* item = publicSceneItem(it->scene, name)) {
                entry["visible"] = obs_sceneitem_visible(item); entry["locked"] = obs_sceneitem_locked(item);
                entry["transform"] = sourceTransform(scene, name);
            }
            result.append(entry);
        }
    });
    return result;
}
bool Engine::sourceAudioAvailable(const QString& type) const {
    if (!d->ready || !sourceId(type)) return false;
    if (type == "desktop" || type == "application_audio" || type == "window" || type == "game") {
        if (!obs_get_latest_input_type_id(sourceId("application_audio"))) return false;
        if (type == "window" || type == "game") {
            auto* properties = obs_get_source_properties(sourceId(type));
            const bool supported = properties && obs_properties_get(properties, "capture_audio");
            obs_properties_destroy(properties); return supported;
        }
        return true;
    }
    return hasSourceAudio(type, {});
}
QJsonArray Engine::sourceOptions(const QString& type) const {
    QJsonArray result; if (!d->ready || !sourceId(type)) return result;
    auto* properties = obs_get_source_properties(sourceId(type)); if (!properties) return result;
    const char* keys[] = {"window", "monitor_id", "monitor", "device_id"};
    for (const auto* key : keys) {
        auto* property = obs_properties_get(properties, key);
        if (!property || obs_property_get_type(property) != OBS_PROPERTY_LIST) continue;
        for (size_t i = 0; i < obs_property_list_item_count(property); ++i) {
            if (obs_property_list_item_disabled(property, i)) continue;
            const auto format = obs_property_list_format(property);
            QJsonValue value = format == OBS_COMBO_FORMAT_INT ? QJsonValue(static_cast<double>(obs_property_list_item_int(property, i))) : QJsonValue(QString::fromUtf8(obs_property_list_item_string(property, i)));
            result.append(QJsonObject{{"property", key}, {"name", QString::fromUtf8(obs_property_list_item_name(property, i))}, {"value", value}});
        }
    }
    obs_properties_destroy(properties); return result;
}
void Engine::setSourceVisible(const QString& scene, const QString& name, bool visible) {
    auto found = d->scenes.find(scene); if (found == d->scenes.end()) return;
    if (auto* item = publicSceneItem(found->scene, name)) {
        obs_sceneitem_set_visible(item, visible);
        if (auto* companion = d->audioCompanions.value(name)) obs_sceneitem_set_visible(companion, visible);
        const int i = d->row(*found, name); auto entry = found->sources[i].toObject(); entry["visible"] = visible; found->sources[i] = entry;
        emit scenesChanged();
    }
}
QJsonArray Engine::audioSources() const {
    QJsonArray rows;
    for (auto it = d->scenes.cbegin(); it != d->scenes.cend(); ++it) for (auto value : it->sources) {
        const auto entry = value.toObject(); const auto name = entry["name"].toString();
        if (auto* source = d->audioOwners.value(name)) {
            rows.append(QJsonObject{{"name", name}, {"scene", it.key()}, {"properties", entry["properties"].toObject()["audio"]},
                                    {"active", obs_source_active(source) && obs_source_audio_active(source)}});
        }
    }
    return rows;
}
QJsonObject Engine::audioProperties(const QString& name) const {
    for (const auto& scene : d->scenes) for (auto value : scene.sources) {
        const auto entry = value.toObject();
        if (entry["name"] == name) return entry["properties"].toObject()["audio"].toObject();
    }
    return {};
}
bool Engine::setAudioProperties(const QString& name, const QJsonObject& patch) {
    auto* source = d->audioOwners.value(name);
    if (!source) return d->fail("Audio source is unavailable: " + name);
    if (const auto error = audioPatchError(patch); !error.isEmpty()) return d->fail(error);
    for (auto& scene : d->scenes) for (qsizetype i = 0; i < scene.sources.size(); ++i) {
        auto entry = scene.sources[i].toObject();
        if (entry["name"] != name) continue;
        auto properties = entry["properties"].toObject(); const auto before = properties["audio"].toObject();
        auto audio = before;
        for (auto it = patch.begin(); it != patch.end(); ++it) audio[it.key()] = it.value();
        if (before == audio) return true;
        if (before["tracks"] != audio["tracks"] && recordingActive()) return d->fail("Stop recording before changing track assignments");
        if (before["monitoring"] != audio["monitoring"] && audio["monitoring"] != "off" && !audioMonitoringAvailable())
            return d->fail("Audio monitoring requires a Windows playback device");
        if (audio["scope"] == "global" && (entry["type"] == "window" || entry["type"] == "game")) return d->fail("Use an Application Audio source for global game/window audio");
        applyAudio(source, audio, before);
        properties["audio"] = audio; entry["properties"] = properties; scene.sources[i] = entry;
        if (before["scope"] != audio["scope"]) d->refreshGlobalAudio();
        emit stateChanged();
        return true;
    }
    return false;
}
QJsonArray Engine::microphoneFilterTypes() const {
    Q_ASSERT(QThread::currentThread() == thread());
    return filters::catalog(d->ready);
}
bool Engine::isMicrophone(const QString& name) const {
    Q_ASSERT(QThread::currentThread() == thread());
    for (const auto& scene : d->scenes) for (auto value : scene.sources) {
        const auto entry = value.toObject();
        if (entry["name"] == name) return entry["type"] == "microphone" && d->audioOwners.contains(name);
    }
    return false;
}
QJsonArray Engine::microphoneFilters(const QString& name) const {
    Q_ASSERT(QThread::currentThread() == thread());
    for (const auto& scene : d->scenes) for (auto value : scene.sources) {
        const auto entry = value.toObject();
        if (entry["name"] == name && entry["type"] == "microphone") return entry["properties"].toObject()["filters"].toArray();
    }
    return {};
}
bool Engine::setMicrophoneFilters(const QString& name, QJsonArray chain) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!d->ready || !isMicrophone(name)) return d->fail("Filters require an existing microphone source");
    for (const auto& scene : d->scenes) if (d->row(scene, name) >= 0 && d->locked(scene, name)) return d->fail("Unlock this source and its parent groups before editing filters");
    if (const auto error = filters::normalize(chain, false); !error.isEmpty()) return d->fail(error);
    const auto existing = microphoneFilters(name);
    for (auto value : chain) {
        const auto row = value.toObject(); QJsonArray single{row};
        if (const auto error = filters::normalize(single, true); !error.isEmpty()) {
            bool retained = false;
            for (auto oldValue : existing) {
                const auto old = oldValue.toObject();
                if (old["id"] == row["id"] && old["type"] == row["type"] && old["settings"] == row["settings"]) retained = true;
            }
            if (!retained) return d->fail(error);
        }
    }
    auto* source = d->audioOwners.value(name);
    for (auto& scene : d->scenes) for (qsizetype i = 0; i < scene.sources.size(); ++i) {
        auto entry = scene.sources[i].toObject();
        if (entry["name"] != name) continue;
        auto properties = entry["properties"].toObject(); const auto before = properties["filters"].toArray();
        if (before == chain) return true;
        if (const auto error = filters::apply(source, before, chain); !error.isEmpty()) return d->fail(error);
        properties["filters"] = chain; entry["properties"] = properties; scene.sources[i] = entry;
        return true;
    }
    return false;
}
bool Engine::audioMonitoringAvailable() const {
    if (!d->ready || !d->monitoringConfigured) return false;
    bool found = false;
    // The libobs Windows enumerator uses COM on the calling thread, which can
    // also be a QCoreApplication thread in clients without a Qt GUI.
    const auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    obs_enum_audio_monitoring_devices([](void* data, const char*, const char*) {
        *static_cast<bool*>(data) = true; return false;
    }, &found);
    if (SUCCEEDED(hr)) CoUninitialize();
    return found;
}
bool Engine::recordingActive() const { return active(d->recording); }
void Engine::setSourceAudio(const QString& name, double volume, bool muted) {
    if (d->audioOwners.contains(name))
        setAudioProperties(name, {{"volume", std::clamp(volume, 0.0, 2.0)}, {"muted", muted}});
}
void Engine::attachPreview(quintptr nativeWindow, int width, int height) {
    d->detachPreview(); if (!d->ready || !nativeWindow) return;
    gs_init_data init{}; init.window.hwnd = reinterpret_cast<void*>(nativeWindow); init.cx = std::max(1, width); init.cy = std::max(1, height);
    init.format = GS_BGRA; init.zsformat = GS_ZS_NONE; init.num_backbuffers = 1;
    d->display = obs_display_create(&init, 0xff161616);
    if (d->display) { obs_display_add_draw_callback(d->display, Impl::draw, d.get()); setPreviewEnabled(d->config["previewEnabled"].toBool()); }
    else d->fail("Could not create native video preview");
}
void Engine::resizePreview(int width, int height) { if (d->display) obs_display_resize(d->display, std::max(1, width), std::max(1, height)); }
void Engine::setPreviewEnabled(bool enabled) { d->config["previewEnabled"] = enabled; if (d->display) obs_display_set_enabled(d->display, enabled); }

bool Engine::startStreaming(QString endpoint) {
    if (active(d->stream)) return true;
    if (!d->ensureEncoders()) return false;
    if (endpoint.isEmpty()) {
        endpoint = d->config["streamEndpoint"].toString(); const auto key = streamKey();
        if (!key.isEmpty()) endpoint = endpoint.trimmed() + (endpoint.endsWith('/') ? "" : "/") + key;
    }
    if (endpoint.isEmpty()) return d->fail("Configure an RTMP/RTMPS endpoint first");
    if (endpoint.startsWith("file:")) endpoint = QUrl(endpoint).toLocalFile();
    if (!endpoint.startsWith("rtmp://") && !endpoint.startsWith("rtmps://") && !QFileInfo(endpoint).isAbsolute()) return d->fail("Use RTMP/RTMPS or an absolute local FLV path");
    d->releaseOutput(d->stream);
    auto* settings = obs_data_create();
    obs_data_set_string(settings, "endpoint", endpoint.toUtf8().constData());
    obs_data_set_int(settings, "delay_ms", d->config["delayMs"].toInt()); obs_data_set_int(settings, "memory_mb", d->config["delayMemoryMb"].toInt());
    d->stream = obs_output_create("feathercast_relay", "FeatherCast relay", settings, nullptr); obs_data_release(settings);
    if (!d->stream) return d->fail("Could not create streaming relay");
    d->attachEncoders(d->stream); d->connectOutput(d->stream);
    if (!obs_output_start(d->stream)) return d->fail("Streaming encoder failed to start");
    emit stateChanged(); return true;
}
void Engine::stopStreaming() { if (d->stream) obs_output_stop(d->stream); }
bool Engine::startRecording(QString filePath) {
    if (active(d->recording)) return true;
    if (!d->ensureEncoders()) return false;
    const auto directory = d->config["outputDirectory"].toString(); QDir().mkpath(directory);
    if (filePath.isEmpty()) filePath = directory + "/Recording-" + QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss-zzz") + ".mkv";
    const QStorageInfo storage(QFileInfo(filePath).absolutePath());
    if (storage.isValid() && storage.bytesAvailable() < 256LL * 1024 * 1024) return d->fail("Recording requires at least 256 MiB free disk space");
    if (!filePath.endsWith(".mkv", Qt::CaseInsensitive)) return d->fail("Record to MKV for crash resilience; remux after recording");
    d->releaseOutput(d->recording);
    d->releaseRecordingAudio();
    auto* settings = obs_data_create(); obs_data_set_string(settings, "path", filePath.toUtf8().constData());
    d->recording = obs_output_create("ffmpeg_muxer", "FeatherCast recording", settings, nullptr); obs_data_release(settings);
    if (!d->recording) return d->fail("OBS FFmpeg recording output unavailable");
    d->attachEncoders(d->recording); d->connectOutput(d->recording);
    // The shared mix remains output track 1. Dedicated encoders are connected
    // only to recording, so streaming and replay keep their existing mix.
    // Fill gaps to keep saved track numbers stable in the container.
    const auto lastTrack = std::bit_width(static_cast<unsigned>(d->usedTracks() | 1));
    for (int track = 2; track <= lastTrack; ++track) {
        auto* audioSettings = obs_data_create();
        obs_data_set_int(audioSettings, "bitrate", d->config["audioBitrateKbps"].toInt());
        const auto label = QString("Recording track %1").arg(track);
        auto* encoder = obs_audio_encoder_create("ffmpeg_aac", label.toUtf8().constData(), audioSettings, track - 1, nullptr);
        obs_data_release(audioSettings);
        if (!encoder) {
            d->releaseOutput(d->recording); d->releaseRecordingAudio();
            return d->fail("Could not create separate recording audio encoder");
        }
        obs_encoder_set_audio(encoder, obs_get_audio());
        d->recordingAudio.push_back(encoder);
        obs_output_set_audio_encoder(d->recording, encoder, track - 1);
    }
    if (!obs_output_start(d->recording)) return d->fail("Recording failed to start");
    d->recordingPath = filePath; emit stateChanged(); return true;
}
void Engine::stopRecording() { if (d->recording) obs_output_stop(d->recording); }
bool Engine::startReplay() {
    if (!d->config["replayEnabled"].toBool()) return d->fail("Replay module is disabled");
    if (active(d->replay)) return true;
    if (!d->ensureEncoders()) return false;
    const auto directory = d->config["outputDirectory"].toString(); QDir().mkpath(directory);
    d->releaseOutput(d->replay);
    auto* settings = obs_data_create();
    obs_data_set_string(settings, "directory", directory.toUtf8().constData());
    obs_data_set_string(settings, "format", "Replay-%CCYY-%MM-%DD_%hh-%mm-%ss");
    obs_data_set_string(settings, "extension", "mkv");
    obs_data_set_int(settings, "max_time_sec", d->config["replaySeconds"].toInt()); obs_data_set_int(settings, "max_size_mb", d->config["replayMemoryMb"].toInt());
    d->replay = obs_output_create("replay_buffer", "FeatherCast replay", settings, nullptr); obs_data_release(settings);
    if (!d->replay) return d->fail("OBS replay buffer output unavailable");
    d->attachEncoders(d->replay); d->connectOutput(d->replay, true);
    if (!obs_output_start(d->replay)) return d->fail("Replay buffer failed to start");
    emit stateChanged(); return true;
}
void Engine::stopReplay() { if (d->replay) obs_output_stop(d->replay); }
bool Engine::saveReplay() {
    if (!d->config["replayEnabled"].toBool()) return d->fail("Replay module is disabled");
    if (!active(d->replay)) return d->fail("Start replay buffering before saving");
    calldata_t cd; calldata_init(&cd); const bool result = proc_handler_call(obs_output_get_proc_handler(d->replay), "save", &cd); calldata_free(&cd);
    return result || d->fail("Replay save request failed");
}
bool Engine::setDelayMs(qint64 milliseconds) {
    if (milliseconds < 0 || milliseconds > 300000) return d->fail("Delay must be between 0 and 300 seconds");
    d->config["delayMs"] = static_cast<int>(milliseconds);
    const bool result = !d->stream || setRelayDelay(d->stream, milliseconds * 1000);
    emit stateChanged(); return result;
}
QJsonObject Engine::statistics() const {
    QJsonObject result{{"initialized", d->ready}, {"streaming", active(d->stream)}, {"recording", active(d->recording)}, {"replay", active(d->replay)},
        {"currentScene", d->selected}, {"recordingPath", d->recordingPath}, {"targetDelayMs", d->config["delayMs"]}};
    if (!d->ready) return result;
    result["studioMode"] = studioMode(); result["programScene"] = d->program;
    result["transitionActive"] = d->transition && obs_transition_is_active(d->transition);
    const auto cpuNow = os_gettime_ns();
    if (d->cpu && cpuNow - d->cpuSampleNs >= 500000000ULL) {
        const double sample = os_cpu_usage_info_query(d->cpu); d->cpuSampleNs = cpuNow;
        if (std::isfinite(sample) && sample >= 0) d->cpuPercent = sample;
    }
    result["cpuPercent"] = d->cpuPercent;
    const auto elapsed = [](obs_output_t* output, uint64_t start) -> double { return active(output) && start ? double(os_gettime_ns() - start) / 1000000 : 0; };
    result["streamDurationMs"] = elapsed(d->stream, d->streamStarted);
    result["recordingDurationMs"] = elapsed(d->recording, d->recordingStarted);
    result["replayDurationMs"] = elapsed(d->replay, d->replayStarted);
    const auto relay = relayStats(d->stream);
    result["fps"] = obs_get_active_fps(); result["renderMs"] = static_cast<double>(obs_get_average_frame_time_ns()) / 1000000;
    result["renderSkippedFrames"] = static_cast<double>(obs_get_lagged_frames()); result["totalFrames"] = static_cast<double>(obs_get_total_frames());
    result["streamBytes"] = static_cast<double>(relay.bytes); result["relayConnected"] = relay.connected;
    result["relayError"] = relay.last_error; result["reconnects"] = static_cast<double>(relay.reconnects);
    result["delayBytes"] = static_cast<double>(relay.buffer.payload_bytes); result["delayPackets"] = static_cast<double>(relay.buffer.packets);
    result["delayDroppedPackets"] = static_cast<double>(relay.buffer.dropped_packets); result["effectiveDelayMs"] = static_cast<double>(relay.buffer.effective_delay_us) / 1000;
    result["recordingBytes"] = d->recording ? static_cast<double>(obs_output_get_total_bytes(d->recording)) : 0;
    QJsonObject peaks; for (const auto& [name, meter] : d->meters) peaks[name] = meter->reading(); result["audioPeaksDb"] = peaks;
    PROCESS_MEMORY_COUNTERS memory{}; memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory))) result["memoryMiB"] = static_cast<double>(memory.WorkingSetSize) / 1048576;
    return result;
}
core::AutomationSnapshot Engine::automationSnapshot() const {
    core::AutomationSnapshot snapshot;
    snapshot.streaming = active(d->stream); snapshot.recording = active(d->recording); snapshot.replay_running = active(d->replay);
    snapshot.current_scene = d->selected.toStdString();
    for (const auto& [name, meter] : d->meters) snapshot.audio_peak_db = std::max(snapshot.audio_peak_db, meter->reading());
    if (d->ready) snapshot.elapsed_ms = static_cast<int64_t>((os_gettime_ns() - d->startedNs) / 1000000);
    DWORD foregroundPid = 0; const auto foreground = GetForegroundWindow(); GetWindowThreadProcessId(foreground, &foregroundPid);
    wchar_t title[1024]{}; GetWindowTextW(foreground, title, 1024); snapshot.window_title = QString::fromWCharArray(title).toStdString();
    const auto processes = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W process{}; process.dwSize = sizeof(process);
        if (Process32FirstW(processes, &process)) do {
            const auto name = QString::fromWCharArray(process.szExeFile).toLower().toStdString(); snapshot.processes.insert(name);
            if (process.th32ProcessID == foregroundPid) snapshot.foreground_process = name;
        } while (Process32NextW(processes, &process));
        CloseHandle(processes);
    }
    return snapshot;
}
bool Engine::loadPlugin(const QString& absolutePath) {
    if (!d->config["pluginsEnabled"].toBool()) return d->fail("Plugins module is disabled");
    if (!QFileInfo(absolutePath).isAbsolute()) return d->fail("Plugin path must be absolute");
    const auto path = QDir::toNativeSeparators(absolutePath);
    auto library = LoadLibraryExW(reinterpret_cast<LPCWSTR>(path.utf16()), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library) return d->fail("Could not load plugin DLL");
    const auto address = GetProcAddress(library, "fc_plugin_query");
    fc_plugin_query_fn query = nullptr;
    static_assert(sizeof(query) == sizeof(address));
    std::memcpy(&query, &address, sizeof(query));
    const fc_plugin_descriptor* descriptor = nullptr;
    try { if (query) descriptor = query(FC_PLUGIN_ABI_VERSION); } catch (...) {}
    const auto reject = [&] { FreeLibrary(library); return d->fail("Plugin ABI validation failed"); };
    if (!descriptor || descriptor->struct_size < sizeof(fc_plugin_descriptor) || descriptor->abi_version != FC_PLUGIN_ABI_VERSION ||
        !descriptor->id || !descriptor->name || !descriptor->version || !descriptor->initialize || !descriptor->shutdown ||
        descriptor->action_count > 128 || (descriptor->action_count && !descriptor->actions)) return reject();
    for (const auto& plugin : d->plugins) if (std::strcmp(plugin.descriptor->id, descriptor->id) == 0) return reject();
    std::map<std::string, bool> actions;
    for (const auto& plugin : d->plugins) for (uint32_t i = 0; i < plugin.descriptor->action_count; ++i) actions[plugin.descriptor->actions[i].id] = true;
    for (uint32_t i = 0; i < descriptor->action_count; ++i) {
        const auto& action = descriptor->actions[i];
        if (action.struct_size < sizeof(fc_action_descriptor) || !action.id || !action.display_name || !action.invoke || actions.contains(action.id)) return reject();
        actions[action.id] = true;
    }
    void* context = nullptr; bool success = false;
    try { success = descriptor->initialize(&d->host, &context) != 0; } catch (...) {}
    if (!success) return reject();
    d->plugins.push_back({library, descriptor, context}); emit stateChanged(); return true;
}
QJsonArray Engine::pluginActions() const {
    QJsonArray result;
    for (const auto& p : d->plugins) for (uint32_t i = 0; i < p.descriptor->action_count; ++i)
        result.append(QJsonObject{{"id", p.descriptor->actions[i].id}, {"name", p.descriptor->actions[i].display_name}, {"plugin", p.descriptor->name}});
    return result;
}
bool Engine::invokePluginAction(const QString& id, const QString& argumentsJson) {
    if (!d->config["pluginsEnabled"].toBool()) return d->fail("Plugins module is disabled");
    const auto key = id.toUtf8(), args = argumentsJson.toUtf8();
    for (const auto& p : d->plugins) for (uint32_t i = 0; i < p.descriptor->action_count; ++i)
        if (key == p.descriptor->actions[i].id) { try { return p.descriptor->actions[i].invoke(p.context, args.constData()) != 0; } catch (...) { return d->fail("Plugin action threw an exception"); } }
    return d->fail("Unknown plugin action");
}
#include "workspace.inc"
} // namespace feathercast::engine
