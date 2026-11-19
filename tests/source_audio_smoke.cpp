#include "feathercast/engine/engine.hpp"
#include <obs.h>
#include <util/base.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTimer>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <iostream>

using feathercast::engine::Engine;
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 3) { std::cerr << "usage: source_audio_smoke OBS_RUNTIME OUTPUT_DIRECTORY\n"; return 2; }
    const auto output = QFileInfo(app.arguments()[2]).absoluteFilePath(); QDir().mkpath(output);
    std::atomic<int> logErrors{0};
    base_set_log_handler([](int level, const char* format, va_list arguments, void* context) {
        if (level <= LOG_ERROR) ++*static_cast<std::atomic<int>*>(context);
        std::vfprintf(stderr, format, arguments); std::fputc('\n', stderr);
    }, &logErrors);
    Engine engine;
    bool failed = false, expectError = false, replaySaved = false;
    auto check = [&](bool ok, const char* message) { if (!ok) { failed = true; std::cerr << "FAILED " << message << '\n'; } };
    QObject::connect(&engine, &Engine::errorOccurred, &app, [&](const QString& text) {
        if (!expectError) { failed = true; std::cerr << "ERROR " << text.toStdString() << '\n'; }
    });
    QObject::connect(&engine, &Engine::replaySaved, &app, [&](const QString& path) { replaySaved = QFileInfo(path).size() > 0; });
    auto settings = Engine::defaultSettings();
    settings["width"] = 640; settings["height"] = 360; settings["fps"] = 30;
    settings["encoder"] = "obs_x264"; settings["videoBitrateKbps"] = 1000;
    settings["keyframeSeconds"] = 1; settings["outputDirectory"] = output;
    if (!engine.initialize(app.arguments()[1], settings) || !engine.addScene("Mix")) return 1;
    check(engine.addSource("Mix", "color", "Pattern"), "pattern");
    expectError = true;
    check(!engine.addSource("Mix", "desktop", "Invalid display", {{"capture_audio", true}}), "display requires explicit application");
    check(!engine.addSource("Mix", "application_audio", "Invalid application", {{"window", "invalid"}}), "invalid application target rejected");
    check(!engine.addSource("Mix", "color", "Invalid track", {{"separate_audio_track", true}}), "video-only track rejected");
    expectError = false;

    // Never capture a real user's application. These nonexistent targets are
    // created only in an inactive scene, and removed before recording starts.
    const QString target = "FeatherCast nonexistent fixture:FeatherCastAudioFixture:feathercast-audio-fixture.exe";
    if (engine.sourceAudioAvailable("desktop")) {
        check(engine.addScene("Capture ownership"), "capture scene");
        check(engine.addSource("Capture ownership", "desktop", "Display", {{"capture_audio", true}, {"audio_window", target}, {"separate_audio_track", true}}), "display companion");
        auto* sceneSource = obs_get_source_by_name("Capture ownership");
        auto* scene = obs_scene_from_source(sceneSource);
        struct Items { obs_sceneitem_t* audio{}; int count{}; } items;
        obs_scene_enum_items(scene, [](obs_scene_t*, obs_sceneitem_t* item, void* data) {
            auto& items = *static_cast<Items*>(data); ++items.count;
            if (QString::fromUtf8(obs_source_get_id(obs_sceneitem_get_source(item))) == "wasapi_process_output_capture") items.audio = item;
            return true;
        }, &items);
        check(items.count == 2 && items.audio, "one private companion scene item");
        if (items.audio) {
            auto* source = obs_sceneitem_get_source(items.audio);
            check(obs_source_get_audio_mixers(source) == 3, "companion mix and isolated routing");
            engine.setSourceAudio("Display", .25, true);
            check(obs_source_muted(source) && std::abs(obs_source_get_volume(source) - .25f) < .001, "companion mute and gain");
            engine.setSourceVisible("Capture ownership", "Display", false);
            check(!obs_sceneitem_visible(items.audio), "companion hidden with video");
            engine.setSourceVisible("Capture ownership", "Display", true);
            check(obs_sceneitem_visible(items.audio), "companion shown with video");
        }
        obs_source_release(sceneSource);
        // A public source may legitimately share the private companion's name.
        check(engine.addSource("Capture ownership", "test_tone", "Display audio"), "public name matching private companion");
        engine.setSourceVisible("Capture ownership", "Display audio", false);
        if (items.audio) check(obs_sceneitem_visible(items.audio), "public visibility does not target private namesake");
        check(engine.removeSource("Capture ownership", "Display audio"), "remove public namesake only");
        engine.setSourceAudio("Display", .5, false);
        if (items.audio) check(!obs_source_muted(obs_sceneitem_get_source(items.audio)), "private companion survives namesake removal");
        check(engine.sources("Capture ownership").size() == 1, "companion hidden from source configuration");
        check(engine.statistics()["audioPeaksDb"].toObject().contains("Display"), "companion meter owned by video name");
        check(engine.saveConfiguration(output + "/capture-config.json") && engine.loadConfiguration(output + "/capture-config.json"), "capture config roundtrip");
        check(engine.sources("Capture ownership")[0].toObject()["properties"].toObject()["audio_window"].toString() == target, "application choice persisted");
        check(engine.removeSource("Capture ownership", "Display"), "remove display and companion");
        check(!engine.statistics()["audioPeaksDb"].toObject().contains("Display"), "remove companion meter");
        for (const auto& type : {"window", "game"}) if (engine.sourceAudioAvailable(type)) {
            const auto name = QString("Native %1").arg(type);
            check(engine.addSource("Capture ownership", type, name, {{"capture_audio", true}, {"window", target}, {"separate_audio_track", true}}), "native capture audio");
            auto* source = obs_get_source_by_name(name.toUtf8().constData());
            if (source) {
                auto* properties = obs_source_get_settings(source);
                check(obs_data_get_bool(properties, "capture_audio") && obs_source_get_audio_mixers(source) == 3, "native capture audio and parent mixer");
                if (QString(type) == "game") check(QString::fromUtf8(obs_data_get_string(properties, "capture_mode")) == "window", "selected game target respected");
                obs_data_release(properties); obs_source_release(source);
            } else check(false, "native capture source exists");
            check(engine.removeSource("Capture ownership", name), "native parent audio teardown");
        }
        check(engine.removeScene("Capture ownership"), "remove capture scene");
    } else std::cout << "SKIP native application capture unavailable\n";

    check(engine.addSource("Mix", "test_tone", "Isolated 440 Hz", {{"frequency_hz", 440}, {"separate_audio_track", true}}), "isolated tone");
    check(engine.addSource("Mix", "test_tone", "Mixed 880 Hz", {{"frequency_hz", 880}}), "mixed tone");
    for (auto value : engine.sources("Mix")) {
        const auto source = value.toObject(); const auto properties = source["properties"].toObject();
        check(!properties.contains("capture_audio"), "optional capture setting not synthesized");
        if (source["name"].toString() != "Isolated 440 Hz")
            check(!properties.contains("separate_audio_track") && !properties.contains("recording_track"), "normal sources retain legacy option-free properties");
    }
    for (const auto& name : {"Isolated 440 Hz", "Mixed 880 Hz"}) {
        auto* source = obs_get_source_by_name(name);
        check(source && obs_source_get_audio_mixers(source) == (QString(name).startsWith("Isolated") ? 3u : 1u), "explicit mixer masks");
        if (source) obs_source_release(source);
    }
    const QString tone = "Isolated 440 Hz", other = "Mixed 880 Hz";
    auto* native = obs_get_source_by_name(tone.toUtf8().constData());
    if (!native) return 1;
    check(engine.audioSources().size() == 2, "audio rows exclude video-only sources");
    check(engine.setAudioProperties(tone, {{"muted", true}, {"volume", .75}, {"mono", true},
                                           {"balance", .25}, {"sync_offset_ms", -175}, {"tracks", 0}}), "advanced audio patch");
    check(obs_source_muted(native) && obs_source_get_volume(native) == .75f &&
          (obs_source_get_flags(native) & OBS_SOURCE_FLAG_FORCE_MONO) &&
          obs_source_get_balance_value(native) == .25f && obs_source_get_sync_offset(native) == -175000000 &&
          obs_source_get_audio_mixers(native) == 0, "native gain, mono, pan, signed offset, and empty mask");
    const auto advanced = engine.audioProperties(tone);
    expectError = true;
    for (auto bad : {QJsonObject{{"tracks", 64}}, QJsonObject{{"volume", 3}}, QJsonObject{{"balance", -.1}},
                     QJsonObject{{"sync_offset_ms", 1.5}}, QJsonObject{{"monitoring", "invalid"}}, QJsonObject{{"mono", 1}}}) {
        bad["muted"] = false;
        check(!engine.setAudioProperties(tone, bad) && engine.audioProperties(tone) == advanced && obs_source_muted(native),
              "invalid multi-field patch preserves native and saved state");
    }
    expectError = false;
    if (engine.audioMonitoringAvailable()) {
        check(engine.setAudioProperties(tone, {{"monitoring", "monitor_only"}, {"tracks", 33}}) &&
              obs_source_get_monitoring_enabled(native) && obs_source_get_audio_mixers(native) == 0 &&
              engine.audioProperties(tone)["tracks"].toInt() == 33, "muted monitor only preserves desired tracks");
        check(engine.setAudioProperties(tone, {{"monitoring", "monitor_and_output"}}) &&
              obs_source_get_monitoring_enabled(native) && obs_source_get_audio_mixers(native) == 33, "monitor and output restores tracks");
        check(engine.setAudioProperties(tone, {{"monitoring", "off"}}) &&
              !obs_source_get_monitoring_enabled(native) && obs_source_get_audio_mixers(native) == 33, "monitor off restores tracks");
    } else std::cout << "SKIP monitoring endpoint unavailable\n";
    obs_source_release(native);
    const auto savedAudio = engine.audioProperties(tone);
    const QJsonObject staleGain{{tone, QJsonObject{{"volume", .1}, {"muted", false}}}};
    check(engine.saveConfiguration(output + "/tones-config.json") && engine.loadConfiguration(output + "/tones-config.json", staleGain) &&
          engine.audioProperties(tone) == savedAudio, "canonical properties override stale UI gain and survive roundtrip");
    const auto before = engine.sources("Mix");
    QFile saved(output + "/tones-config.json"); saved.open(QIODevice::ReadOnly);
    auto document = QJsonDocument::fromJson(saved.readAll()).object(); saved.close();
    auto scenes = document["scenes"].toArray(); auto scene = scenes[0].toObject(); auto sources = scene["sources"].toArray();
    auto saveFixture = [&](const QString& path) {
        scene["sources"] = sources; scenes[0] = scene; document["scenes"] = scenes;
        QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(document).toJson()) > 0;
    };
    auto secondTone = sources[2].toObject();
    secondTone["properties"] = QJsonObject{{"audio", QJsonObject{{"tracks", -1}}}};
    sources[2] = secondTone;
    check(saveFixture(output + "/invalid-config.json"), "write invalid graph");
    expectError = true;
    check(!engine.loadConfiguration(output + "/invalid-config.json") && engine.sources("Mix") == before, "invalid audio import rejected before graph mutation");
    expectError = false;
    auto firstTone = sources[1].toObject();
    firstTone["properties"] = QJsonObject{{"separate_audio_track", true}, {"frequency_hz", 440}};
    sources[1] = firstTone;
    secondTone["properties"] = QJsonObject{{"audio", QJsonObject{{"tracks", 3}}}, {"frequency_hz", 880}};
    sources[2] = secondTone;
    check(saveFixture(output + "/legacy-config.json") && engine.loadConfiguration(output + "/legacy-config.json", staleGain),
          "legacy shortcut migration");
    auto migrated = engine.audioProperties(tone);
    check(migrated["tracks"] == 5 && migrated["volume"] == .1 && !migrated["muted"].toBool(),
          "legacy UI gain imported and isolation skips later manual assignment");
    check(engine.setAudioProperties(tone, {{"tracks", 3}, {"volume", 1}}) && engine.setAudioProperties(other, {{"tracks", 1}}),
          "shared manual tracks allowed, original tone fixture restored");
    // Fill remaining slots in another scene; sources in inactive scenes stay silent.
    check(engine.addScene("Capacity"), "capacity scene");
    for (int i = 0; i < 4; ++i) check(engine.addSource("Capacity", "test_tone", QString("Capacity %1").arg(i), {{"separate_audio_track", true}}), "track allocation");
    expectError = true;
    check(!engine.addSource("Capacity", "test_tone", "Overflow", {{"separate_audio_track", true}}), "sixth separate track rejected");
    expectError = false;
    check(engine.removeScene("Capacity"), "scene and track capacity released");
    if (failed) { engine.shutdown(); return 1; }
    check(engine.startStreaming(output + "/mixed-relay.flv") && engine.startReplay() && engine.startRecording(output + "/isolated.mkv"), "concurrent output start");
    expectError = true;
    check(!engine.removeSource("Mix", "Isolated 440 Hz"), "isolated removal blocked while recording");
    check(!engine.removeScene("Mix"), "isolated scene removal blocked while recording");
    check(!engine.addSource("Mix", "test_tone", "Live extra track", {{"separate_audio_track", true}}), "track topology fixed while recording");
    expectError = false;
    const auto liveBefore = engine.audioProperties(other);
    expectError = true;
    check(!engine.setAudioProperties(other, {{"tracks", 4}, {"volume", 0}}) && engine.audioProperties(other) == liveBefore,
          "recording rejects entire routing patch");
    expectError = false;
    check(engine.setAudioProperties(other, {{"volume", .99}, {"sync_offset_ms", 1}}) &&
          engine.setAudioProperties(other, {{"volume", 1}, {"sync_offset_ms", 0}}), "gain and sync remain live during recording");
    QTimer::singleShot(5000, &app, [&] { engine.saveReplay(); engine.stopRecording(); });
    QTimer::singleShot(6000, &app, [&] {
        check(engine.statistics()["streaming"].toBool(), "shared encoder survives recording stop");
        check(engine.startRecording(output + "/restart.mkv"), "recording restart with separate encoder");
    });
    QTimer::singleShot(8500, &app, [&] { engine.stopRecording(); engine.stopReplay(); engine.stopStreaming(); });
    QTimer::singleShot(10000, &app, [&] {
        check(QFileInfo(output + "/isolated.mkv").size() > 0 && QFileInfo(output + "/restart.mkv").size() > 0, "recording files created");
        check(replaySaved, "mixed replay saved");
        check(engine.removeSource("Mix", "Isolated 440 Hz"), "remove isolated source after stop");
        check(engine.addSource("Mix", "test_tone", "Gap 1320 Hz", {{"frequency_hz", 1320}, {"separate_audio_track", true}, {"recording_track", 3}}), "explicit track after gap");
        check(engine.startRecording(output + "/gap.mkv"), "recording preserves track gap");
        auto* recording = obs_get_output_by_name("FeatherCast recording");
        if (recording) {
            auto* gap = obs_output_get_audio_encoder(recording, 1);
            auto* isolated = obs_output_get_audio_encoder(recording, 2);
            check(gap && isolated && obs_encoder_get_mixer_index(gap) == 1 && obs_encoder_get_mixer_index(isolated) == 2, "physical MKV slots preserve assigned track numbers");
            obs_output_release(recording);
        } else check(false, "gap recording exists");
    });
    QTimer::singleShot(12500, &app, [&] { engine.stopRecording(); });
    QTimer::singleShot(13500, &app, [&] {
        check(engine.removeSource("Mix", "Gap 1320 Hz"), "remove gap fixture");
        check(engine.addSource("Mix", "test_tone", tone, {{"frequency_hz", 440}, {"audio", QJsonObject{{"tracks", 5}}}}) &&
              engine.setAudioProperties(other, {{"tracks", 6}}), "shared and excluded routing fixture");
        check(engine.startRecording(output + "/routing.mkv"), "record manual routing");
    });
    QTimer::singleShot(18500, &app, [&] { engine.stopRecording(); });
    QTimer::singleShot(19500, &app, [&] {
        check(QFileInfo(output + "/routing.mkv").size() > 0, "manual routing recording exists");
        engine.shutdown();
        check(logErrors.load() == 0, "no OBS error logs or shutdown lifetime errors");
        std::cout << (failed ? "SOURCE AUDIO SMOKE FAILED\n" : "SOURCE AUDIO SMOKE PASSED\n"); app.exit(failed ? 1 : 0);
    });
    return app.exec();
}
