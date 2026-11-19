#include "feathercast/engine/engine.hpp"
#include <obs.h>
#include <windows.h>
#include <util/base.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QEventLoop>
#include <QTimer>
#include <iostream>
#include <atomic>
#include <cmath>
using feathercast::engine::Engine;
static void waitMs(int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); }
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 3) return 2;
    const auto directory = QFileInfo(app.arguments()[2]).absoluteFilePath(); QDir().mkpath(directory);
    app.setApplicationName("VELA isolated workspace fixture");
    std::atomic<int> logErrors{};
    base_set_log_handler([](int level, const char* format, va_list arguments, void* data) {
        if (level <= LOG_ERROR) ++*static_cast<std::atomic<int>*>(data);
        std::vfprintf(stderr, format, arguments); std::fputc('\n', stderr);
    }, &logErrors);
    Engine e; bool failed = false; bool expected = false;
    auto check = [&](bool ok, const char* message) { std::cerr << (ok ? "PASS " : "FAIL ") << message << '\n'; if (!ok) failed = true; };
    QObject::connect(&e, &Engine::errorOccurred, [&](QString message) { std::cerr << "ENGINE " << message.toStdString() << '\n'; if (!expected) failed = true; });
    auto config = Engine::defaultSettings(); config["width"] = 640; config["height"] = 360; config["fps"] = 30; config["encoder"] = "obs_x264"; config["outputDirectory"] = directory;
    if (!e.initialize(app.arguments()[1], config)) return 1;
    auto previewWindow = CreateWindowExW(0, L"STATIC", L"VELA fixture preview", WS_OVERLAPPEDWINDOW, 0, 0, 320, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    auto programWindow = CreateWindowExW(0, L"STATIC", L"VELA fixture program", WS_OVERLAPPEDWINDOW, 0, 0, 320, 180, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(previewWindow && programWindow, "two native preview windows");
    e.attachPreview(reinterpret_cast<quintptr>(previewWindow), 320, 180); e.attachProgram(reinterpret_cast<quintptr>(programWindow), 320, 180);
    e.resizePreview(400, 225); e.resizeProgram(400, 225); e.setPreviewEnabled(false); e.setPreviewEnabled(true);
    check(e.addScene("A") && e.addScene("B"), "create scenes");
    check(e.addSource("A", "color", "Red", {{"color", double(0xff0000ffu)}}), "red source");
    check(e.addSource("B", "color", "Blue", {{"color", double(0xffff0000u)}}), "blue source");
    check(e.addSource("A", "test_tone", "Tone"), "synthetic audio");
    check(e.addGroup("A", "Outer") && e.addGroup("A", "Inner"), "native groups");
    check(e.moveSource("A", "Inner", "Outer", 0) && e.moveSource("A", "Red", "Inner", 0) && e.moveSource("A", "Tone", "Outer", 1), "nested grouping");
    auto* a = obs_get_source_by_name("A"); auto* scene = obs_scene_from_source(a);
    auto* outer = obs_scene_get_group(scene, "Outer"); auto* inner = outer ? obs_scene_get_group(obs_sceneitem_group_get_scene(outer), "Inner") : nullptr;
    check(inner && obs_scene_find_source(obs_sceneitem_group_get_scene(inner), "Red"), "real native group nesting"); obs_source_release(a);
    check(e.setSourceTransform("A", "Red", {{"x", 25.0}, {"y", 35.0}, {"rotation", 12.0}, {"cropLeft", 2}}), "transform edit");
    check(e.setSourceLocked("A", "Outer", true), "lock group");
    expected = true; check(!e.setSourceTransform("A", "Red", {{"x", 0}}) && !e.removeSource("A", "Outer") && !e.renameSource("A", "Red", "Denied"), "ancestor lock rejects edits"); expected = false;
    e.setSourceVisible("A", "Red", false); check(e.sources("A")[0].toObject()["name"] != "", "visibility remains available");
    e.setSourceVisible("A", "Red", true); e.setSourceLocked("A", "Outer", false);
    check(e.renameSource("A", "Tone", "Sound") && e.audioProperties("Sound").contains("volume"), "rename migrates audio ownership");
    check(e.setSourceProperties("A", "Red", {{"color", double(0xff00ff00u)}, {"width", 480}, {"height", 270}}), "native property edit");
    expected = true; check(!e.setSourceProperties("A", "Red", {{"unsupported", 7}}), "unrecognized property rejected"); check(!e.moveSource("A", "Outer", "Inner", 0), "group cycle rejected"); expected = false;
    auto videoTypes = e.videoFilterTypes(); check(videoTypes.size() >= 3, "native video filter catalog");
    QJsonArray filters{QJsonObject{{"id", "color1"}, {"name", "Color"}, {"type", "color_filter"}, {"enabled", true}, {"settings", QJsonObject{{"brightness", .1}}}}};
    check(e.setVideoFilters("A", "Red", filters), "native color correction");
    filters.append(QJsonObject{{"id", "crop1"}, {"name", "Crop"}, {"type", "crop_filter"}, {"enabled", true}, {"settings", QJsonObject{{"left", 1}}}});
    check(e.setVideoFilters("A", "Red", filters), "two video filter chain");
    auto* red = obs_get_source_by_name("Red");
    QStringList nativeOrder; obs_source_enum_filters(red, [](obs_source_t*, obs_source_t* filter, void* p) { static_cast<QStringList*>(p)->append(QString::fromUtf8(obs_source_get_name(filter))); }, &nativeOrder);
    check(nativeOrder == QStringList{"VELA video color1", "VELA video crop1"}, "native filter processing order"); auto* nativeFilter = obs_source_get_filter_by_name(red, "VELA video color1"); check(nativeFilter && obs_source_enabled(nativeFilter), "filter attached"); obs_source_release(nativeFilter); obs_source_release(red);
    check(e.setTransition("fade", 150), "transition settings"); e.setStudioMode(true); check(e.selectScene("B"), "select studio preview");
    check(e.currentScene() == "B" && e.programScene() == "A", "preview selection preserves program");
    auto verifyProgram = [&](const char* name) { auto* output = obs_get_output_source(0); auto* target = obs_transition_get_active_source(output); const bool ok = target && QString::fromUtf8(obs_source_get_name(target)) == name; obs_source_release(target); obs_source_release(output); return ok; };
    check(verifyProgram("A"), "native output still A");
    check(e.takePreview("fade"), "start fade"); waitMs(650); check(verifyProgram("B") && !e.statistics()["transitionActive"].toBool(), "fade finishes on B");
    check(e.selectScene("A") && e.takePreview("cut"), "cut preview to program"); check(verifyProgram("A"), "native cut output A");
    std::atomic<double> rms{};
    const auto audioSample = +[](void* p, size_t, audio_data* data) {
        auto* samples = reinterpret_cast<float*>(data->data[0]); double sum = 0;
        for (unsigned i = 0; i < data->frames; ++i) sum += double(samples[i]) * samples[i];
        static_cast<std::atomic<double>*>(p)->store(std::sqrt(sum / data->frames));
    };
    audio_convert_info conversion{48000, AUDIO_FORMAT_FLOAT_PLANAR, SPEAKERS_STEREO, false};
    check(audio_output_connect(obs_get_audio(), 0, &conversion, audioSample, &rms), "observe actual output audio"); waitMs(350); const auto sceneRms = rms.load();
    check(e.setAudioProperties("Sound", {{"scope", "global"}}), "global audio scope"); waitMs(350);
    check(sceneRms > .02 && std::abs(rms.load() - sceneRms) < .006, "global audio does not double mix on scene"); e.selectScene("B"); e.takePreview("cut"); waitMs(250);
    auto* sound = obs_get_source_by_name("Sound"); check(sound && obs_source_active(sound) && rms.load() > .02, "global audio active and audible off scene");
    e.selectScene("A"); // Preview the tone while program stays B.
    check(e.setAudioProperties("Sound", {{"scope", "scene"}}), "restore scene scope"); waitMs(250); check(!obs_source_active(sound) && rms.load() < .0001, "preview-only scene audio stays out of program"); obs_source_release(sound);
    e.selectScene("A"); e.takePreview("cut"); waitMs(250); check(rms.load() > .02, "scene audio enters program after Take"); check(e.setAudioProperties("Sound", {{"scope", "global"}, {"mixer_hidden", true}}), "persist global and hidden mixer");
    check(e.renameScene("A", "Main") && e.moveScene("B", 0), "scene rename/reorder");
    const auto rmsBeforeDuplicate = rms.load();
    check(e.duplicateScene("Main", "Copy"), "duplicate nested scene");
    check(e.sources("Copy").size() == e.sources("Main").size() - 1, "duplicate skips already global audio input");
    waitMs(150);
    check(std::abs(rms.load() - rmsBeforeDuplicate) < .006, "duplicating global audio does not double output");
    for (auto value : e.sources("Main")) { const auto row = value.toObject(); if (row["name"] == "Sound") continue; check(e.sourceTransform("Main", row["name"].toString()) == e.sourceTransform("Copy", "Copy / " + row["name"].toString()), "duplicate transforms match"); }
    check(e.removeScene("Copy"), "remove duplicate"); waitMs(150); check(std::abs(rms.load() - rmsBeforeDuplicate) < .006, "removing duplicate preserves global audio");
    e.setSourceLocked("Main", "Red", true); const auto before = e.sources("Main");
    check(e.saveConfiguration(directory + "/workspace.json"), "save layout");
    check(e.loadConfiguration(directory + "/workspace.json"), "transactional roundtrip");
    const auto after = e.sources("Main");
    check(e.scenes() == QStringList{"B", "Main"} && before == after, "full order/groups/transforms/locks/properties roundtrip");
    check(e.audioProperties("Sound")["scope"] == "global" && e.audioProperties("Sound")["mixer_hidden"].toBool(), "global mixer settings roundtrip");
    QFile saved(directory + "/workspace.json"); saved.open(QIODevice::ReadOnly); auto doc = QJsonDocument::fromJson(saved.readAll()).object(); saved.close();
    auto failedDoc = doc; auto failedScenes = failedDoc["scenes"].toArray(); auto failedScene = failedScenes[0].toObject(); auto failedRows = failedScene["sources"].toArray();
    failedRows.append(QJsonObject{{"name", "Invalid video tone"}, {"type", "test_tone"}, {"properties", QJsonObject{{"video_filters", filters}}}}); failedScene["sources"] = failedRows; failedScenes[0] = failedScene; failedDoc["scenes"] = failedScenes;
    QFile runtimeInvalid(directory + "/runtime-invalid.json"); runtimeInvalid.open(QIODevice::WriteOnly); runtimeInvalid.write(QJsonDocument(failedDoc).toJson()); runtimeInvalid.close();
    expected = true; check(!e.loadConfiguration(runtimeInvalid.fileName()), "runtime construction failure rejected"); expected = false;
    check(e.sources("Main") == after && e.programScene() == "Main" && e.audioProperties("Sound")["scope"] == "global", "runtime failure restores previous graph meters and global audio");
    auto scenes = doc["scenes"].toArray(); auto main = scenes[1].toObject(); auto rows = main["sources"].toArray(); auto row = rows[0].toObject(); row["parent"] = "missing"; rows[0] = row; main["sources"] = rows; scenes[1] = main; doc["scenes"] = scenes;
    QFile invalid(directory + "/invalid.json"); invalid.open(QIODevice::WriteOnly); invalid.write(QJsonDocument(doc).toJson()); invalid.close(); expected = true; check(!e.loadConfiguration(invalid.fileName()), "invalid parent rejected"); expected = false;
    check(e.sources("Main") == after && e.programScene() == "Main", "invalid import preserves graph and output");
    e.setStudioMode(false); check(e.currentScene() == e.programScene(), "studio off takes selected scene");
    check(e.statistics().contains("cpuPercent") && e.statistics().contains("recordingDurationMs"), "status metrics exposed");
    check(e.startRecording(directory + "/workspace.mkv"), "recording fixture"); waitMs(800); check(e.statistics()["recordingDurationMs"].toDouble() > 300, "recording duration advances"); e.stopRecording(); waitMs(600);
    audio_output_disconnect(obs_get_audio(), 0, audioSample, &rms);
    e.attachPreview(0, 0, 0); e.attachProgram(0, 0, 0); DestroyWindow(previewWindow); DestroyWindow(programWindow);
    e.shutdown(); waitMs(50); check(logErrors == 0, "no unexpected OBS error logs"); std::cerr << (failed ? "WORKSPACE FAILED\n" : "WORKSPACE PASSED\n"); return failed ? 1 : 0;
}
