#include "feathercast/engine/engine.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTimer>
#include <iostream>
#include <atomic>
#include <cstdio>
#include <util/base.h>

using feathercast::engine::Engine;
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("FeatherCastTest");
    QCoreApplication::setApplicationName("FeatherCastEngineSmoke");
    const auto args = app.arguments();
    if (args.size() < 3) { std::cerr << "usage: smoke OBS_RUNTIME OUTPUT_DIRECTORY [STREAM_ENDPOINT] [ENCODER]\n"; return 2; }
    const auto output = QFileInfo(args[2]).absoluteFilePath(); QDir().mkpath(output);
    std::atomic<int> logErrors{0};
    base_set_log_handler([](int level, const char* format, va_list arguments, void* context) {
        if (level <= LOG_ERROR) ++*static_cast<std::atomic<int>*>(context);
        std::vfprintf(stderr, format, arguments); std::fputc('\n', stderr);
    }, &logErrors);
    Engine engine;
    bool failed = false, replaySaved = false;
    QObject::connect(&engine, &Engine::errorOccurred, &app, [&](const QString& text) { failed = true; std::cerr << "ERROR " << text.toStdString() << '\n'; });
    QObject::connect(&engine, &Engine::logMessage, &app, [](const QString& text) { std::cout << "INFO " << text.toStdString() << '\n'; });
    QObject::connect(&engine, &Engine::replaySaved, &app, [&](const QString& file) { replaySaved = QFileInfo(file).size() > 0; std::cout << "REPLAY " << file.toStdString() << '\n'; });
    auto settings = Engine::defaultSettings();
    settings["width"] = 640; settings["height"] = 360; settings["fps"] = 30;
    settings["encoder"] = args.size() > 4 ? args[4] : "obs_x264"; settings["videoBitrateKbps"] = 1000;
    settings["keyframeSeconds"] = 1; settings["replaySeconds"] = 5; settings["delayMs"] = 500; settings["outputDirectory"] = output;
    if (!engine.initialize(args[1], settings) || !engine.addScene("Test") ||
        !engine.addSource("Test", "color", "Test pattern") || !engine.addSource("Test", "test_tone", "Test tone")) return 1;
    const auto plugin = QCoreApplication::applicationDirPath() + "/src/core/feathercast_example_plugin.dll";
    if (QFileInfo::exists(plugin)) {
        if (!engine.loadPlugin(plugin) || !engine.invokePluginAction("tournament.delay90")) return 1;
        engine.setDelayMs(500);
    }
    if (!engine.startRecording(output + "/recording.mkv") || !engine.startReplay() ||
        !engine.startStreaming(args.size() > 3 ? args[3] : output + "/relay.flv")) return 1;
    QTimer::singleShot(2500, &app, [&] { engine.setDelayMs(1800); std::cout << "DELAY increase\n"; });
    QTimer::singleShot(6000, &app, [&] { engine.setDelayMs(0); engine.saveReplay(); std::cout << "DELAY decrease, replay requested\n"; });
    QTimer::singleShot(10000, &app, [&] {
        const auto stats = engine.statistics();
        std::cout << "STATS " << QJsonDocument(stats).toJson(QJsonDocument::Compact).toStdString() << '\n';
        if (!stats["relayConnected"].toBool() || stats["relayError"].toInt() != 0 ||
            stats["streamBytes"].toDouble() <= 0 || stats["effectiveDelayMs"].toDouble() > 1300) failed = true;
        engine.stopStreaming(); engine.stopRecording(); engine.stopReplay();
    });
    QTimer::singleShot(12000, &app, [&] {
        const auto stats = engine.statistics();
        if (stats["streaming"].toBool() || stats["recording"].toBool() || stats["replay"].toBool()) failed = true;
        if (QFileInfo(output + "/recording.mkv").size() <= 0 || !replaySaved) failed = true;
        if (!engine.saveConfiguration(output + "/configuration.json")) failed = true;
        engine.shutdown();
        if (logErrors.load() != 0) { failed = true; std::cerr << "Unexpected OBS error-level messages: " << logErrors << '\n'; }
        std::cout << (failed ? "SMOKE FAILED\n" : "SMOKE PASSED\n"); app.exit(failed ? 1 : 0);
    });
    return app.exec();
}
