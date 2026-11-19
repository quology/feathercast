#include "feathercast/engine/engine.hpp"
#include "feathercast/ui/microphone_filters_dialog.hpp"
#include "feathercast/ui/theme.hpp"
#include <obs.h>
#include <util/platform.h>
#include <QtWidgets>
#include <cmath>
#include <iostream>

namespace {
template<typename T> T* control(QWidget& dialog, const QString& name) {
    for (auto* widget : dialog.findChildren<T*>()) if (widget->accessibleName() == name && widget->isVisible()) return widget;
    return nullptr;
}
void events() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QCoreApplication::processEvents(); }
QJsonArray graph(obs_source_t* source) {
    QJsonArray rows;
    obs_source_enum_filters(source, [](obs_source_t*, obs_source_t* filter, void* data) {
        static_cast<QJsonArray*>(data)->append(QJsonObject{{"name", QString::fromUtf8(obs_source_get_name(filter))},
            {"type", QString::fromUtf8(obs_source_get_id(filter))}, {"enabled", obs_source_enabled(filter)}});
    }, &rows);
    return rows;
}
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().size() < 3 || app.arguments().size() > 4) return 2;
    const bool unavailable = app.arguments().contains("--unavailable");
    app.setStyle("Fusion"); feathercast::ui::applyTheme(true);
    const auto out = QFileInfo(app.arguments()[2]).absoluteFilePath(); QDir().mkpath(out);
    feathercast::engine::Engine engine;
    auto settings = engine.defaultSettings(); settings["width"] = 640; settings["height"] = 360;
    settings["fps"] = 30; settings["outputDirectory"] = out;
    bool failed = false, expectedError = false;
    auto check = [&](bool ok, const char* why) { if (!ok) { failed = true; std::cerr << "FAILED " << why << '\n'; } };
    QObject::connect(&engine, &feathercast::engine::Engine::errorOccurred, &app, [&](const QString& error) {
        if (!expectedError) { failed = true; std::cerr << error.toStdString() << '\n'; }
    });
    if (!engine.initialize(app.arguments()[1], settings) || !engine.addScene("Fixture")) return 1;
    // A deliberately nonexistent device ID can never open a user's microphone.
    // Only synthetic samples below enter the actual microphone/filter graph.
    const QString microphone = "Microphone fixture";
    const QJsonObject device{{"device_id", "feathercast-nonexistent-filter-test-device-7a9b33a9"},
                             {"audio", QJsonObject{{"tracks", 0}, {"monitoring", "off"}}}};
    check(engine.addSource("Fixture", "microphone", microphone, device), "microphone fixture without a real device");
    check(engine.addSource("Fixture", "color", "Color"), "legacy video source");
    check(engine.microphoneFilters(microphone).isEmpty(), "no automatically added filters");
    for (auto v : engine.sources("Fixture")) check(!v.toObject()["properties"].toObject().contains("filters"), "legacy sources remain filter-key-free");
    check(engine.saveConfiguration(out + "/legacy.json") && engine.loadConfiguration(out + "/legacy.json"), "legacy roundtrip");
    for (auto v : engine.sources("Fixture")) check(!v.toObject()["properties"].toObject().contains("filters"), "legacy roundtrip remains filter-key-free");
    QMap<QString, QJsonObject> types;
    for (auto v : engine.microphoneFilterTypes()) { auto spec = v.toObject(); types.insert(spec["type"].toString(), spec); }
    auto row = [&](const QString& type, const QString& id) {
        const auto spec = types[type];
        return QJsonObject{{"id", id}, {"name", spec["name"]}, {"type", type}, {"enabled", true}, {"settings", spec["defaults"]}};
    };
    auto compressor = row("compressor", "compressor"), gate = row("noise_gate", "gate"), suppression = row("noise_suppression", "suppression");
    suppression["settings"] = QJsonObject{{"method", "speex"}, {"suppress_level", -30}};
    QJsonArray chain{compressor, gate, suppression};
    auto saveGraph = [&](const QJsonArray& filters, const QString& path, bool onVideo = false) {
        QFile file(out + "/legacy.json"); file.open(QIODevice::ReadOnly);
        auto doc = QJsonDocument::fromJson(file.readAll()).object(); file.close();
        auto scenes = doc["scenes"].toArray(); auto scene = scenes[0].toObject(); auto sources = scene["sources"].toArray();
        auto source = sources[onVideo ? 1 : 0].toObject(); auto properties = source["properties"].toObject(); properties["filters"] = filters;
        source["properties"] = properties; sources[onVideo ? 1 : 0] = source; scene["sources"] = sources; scenes[0] = scene; doc["scenes"] = scenes;
        QFile output(path); return output.open(QIODevice::WriteOnly) && output.write(QJsonDocument(doc).toJson()) > 0;
    };
    check(saveGraph(chain, out + "/chain.json") && engine.loadConfiguration(out + "/chain.json"), "load structurally valid chain");
    auto* native = obs_get_source_by_name(microphone.toUtf8().constData()); if (!native) return 1;
    if (unavailable) {
        for (const auto& spec : types) check(!spec["available"].toBool(), "optional filters unavailable");
        check(graph(native).isEmpty() && engine.microphoneFilters(microphone) == chain, "missing module preserves dormant rows without native filters");
        compressor["enabled"] = false; chain[0] = compressor;
        check(engine.setMicrophoneFilters(microphone, chain), "can bypass dormant row");
        chain.removeLast(); check(engine.setMicrophoneFilters(microphone, chain), "can remove dormant row");
        obs_source_release(native);
        check(engine.saveConfiguration(out + "/dormant.json") && engine.loadConfiguration(out + "/dormant.json") && engine.microphoneFilters(microphone) == chain, "dormant roundtrip");
        std::unique_ptr<QDialog> dialog(feathercast::ui::createMicrophoneFiltersDialog(engine, microphone)); dialog->show(); events();
        auto* add = control<QPushButton>(*dialog, "Add filter"); check(add && !add->isEnabled(), "unavailable UI disables add");
        check(dialog->grab().save(out + "/unavailable.png"), "unavailable screenshot");
        dialog.reset(); engine.shutdown();
        std::cout << (failed ? "FILTERS UNAVAILABLE FAILED\n" : "FILTERS UNAVAILABLE PASSED\n"); return failed ? 1 : 0;
    }
    for (const auto& spec : types) check(spec["available"].toBool(), "packaged filters advertised");
    auto nativeRows = graph(native);
    check(nativeRows.size() == 3 && nativeRows[0].toObject()["type"] == "compressor_filter" &&
          nativeRows[1].toObject()["type"] == "noise_gate_filter" && nativeRows[2].toObject()["type"] == "noise_suppress_filter_v2", "real v2 filters in processing order");
    const auto before = engine.sources("Fixture");
    expectedError = true;
    for (const auto& bad : {QJsonObject{{"ratio", 0}}, QJsonObject{{"attack_time", 1.5}}, QJsonObject{{"output_gain", 33}}, QJsonObject{{"sidechain_source", "other"}}}) {
        auto invalid = chain; auto entry = compressor; entry["settings"] = bad; invalid[0] = entry;
        check(!engine.setMicrophoneFilters(microphone, invalid) && engine.sources("Fixture") == before && graph(native) == nativeRows, "invalid live edit preserves graph and config");
        check(saveGraph(invalid, out + "/invalid.json") && !engine.loadConfiguration(out + "/invalid.json") && engine.sources("Fixture") == before, "invalid import preserves scenes");
    }
    auto duplicate = chain; duplicate.append(compressor);
    check(!engine.setMicrophoneFilters(microphone, duplicate), "duplicate IDs rejected");
    check(!engine.setMicrophoneFilters("Color", chain), "filters restricted to microphones");
    check(saveGraph(chain, out + "/video-filters.json", true) && !engine.loadConfiguration(out + "/video-filters.json"), "video filter import rejected");
    expectedError = false;
    auto* original = obs_source_get_filter_by_name(native, "FeatherCast filter compressor");
    auto compSettings = compressor["settings"].toObject(); compSettings["ratio"] = 4.5; compressor["settings"] = compSettings; chain[0] = compressor;
    check(engine.setMicrophoneFilters(microphone, chain), "compressor setting edit");
    compressor["enabled"] = false; chain[0] = compressor;
    check(engine.setMicrophoneFilters(microphone, chain), "bypass edit");
    chain = {suppression, gate, compressor}; check(engine.setMicrophoneFilters(microphone, chain), "reorder edit");
    auto* retained = obs_source_get_filter_by_name(native, "FeatherCast filter compressor");
    check(retained == original && !obs_source_enabled(retained) && graph(native)[2].toObject()["name"] == "FeatherCast filter compressor", "identity and bypass survive reorder");
    obs_source_release(retained); obs_source_release(original); obs_source_release(native);
    check(engine.saveConfiguration(out + "/roundtrip.json") && engine.loadConfiguration(out + "/roundtrip.json") && engine.microphoneFilters(microphone) == chain, "values order bypass roundtrip");
    native = obs_get_source_by_name(microphone.toUtf8().constData());
    // Measure actual post-filter samples. A 6 dB gain before a gate crosses its
    // opening threshold; reversing the same filters leaves the gate closed.
    compressor = row("compressor", "compressor"); compSettings = compressor["settings"].toObject();
    compSettings["ratio"] = 1; compSettings["output_gain"] = 6; compressor["settings"] = compSettings;
    gate = row("noise_gate", "gate"); auto gateSettings = gate["settings"].toObject();
    gateSettings["open_threshold"] = -30; gateSettings["close_threshold"] = -31;
    gateSettings["attack_time"] = 1; gateSettings["hold_time"] = 0; gateSettings["release_time"] = 1; gate["settings"] = gateSettings;
    struct Capture { double rms{}; int frames{}; } capture;
    const auto callback = [](void* data, obs_source_t*, const audio_data* audio, bool) {
        auto& result = *static_cast<Capture*>(data); double sum = 0;
        auto* samples = reinterpret_cast<const float*>(audio->data[0]);
        for (uint32_t i = 0; i < audio->frames; ++i) sum += samples[i] * samples[i];
        result.rms = std::sqrt(sum / audio->frames); ++result.frames;
    };
    obs_source_add_audio_capture_callback(native, callback, &capture);
    uint64_t cursor = 0, epoch = os_gettime_ns();
    auto measure = [&](float amplitude) {
        float samples[480]; for (auto& sample : samples) sample = amplitude;
        capture.frames = 0;
        for (int i = 0; i < 150; ++i) {
            obs_source_audio audio{}; audio.data[0] = audio.data[1] = reinterpret_cast<uint8_t*>(samples);
            audio.frames = 480; audio.speakers = SPEAKERS_STEREO; audio.format = AUDIO_FORMAT_FLOAT_PLANAR; audio.samples_per_sec = 48000;
            audio.timestamp = epoch + cursor * 1000000000ULL / 48000; cursor += 480;
            obs_source_output_audio(native, &audio);
        }
        check(capture.frames > 0 && std::isfinite(capture.rms), "synthetic post-filter samples captured"); return capture.rms;
    };
    check(engine.setMicrophoneFilters(microphone, {compressor, gate}), "gain then gate");
    const auto gainThenGate = measure(.02f);
    check(engine.setMicrophoneFilters(microphone, {gate, compressor}), "gate then gain");
    const auto gateThenGain = measure(.02f);
    check(gainThenGate > .035 && gateThenGain < .001, "DSP ordering changes audible output");
    gate["enabled"] = false; check(engine.setMicrophoneFilters(microphone, {gate, compressor}), "bypass gate");
    check(measure(.02f) > .035, "native bypass restores audio");
    compressor["enabled"] = false; check(engine.setMicrophoneFilters(microphone, {gate, compressor}), "bypass compressor");
    check(std::abs(measure(.02f) - .02) < .001, "bypassing all filters preserves input");
    compressor = row("compressor", "compressor"); check(engine.setMicrophoneFilters(microphone, {compressor}), "default compression");
    check(measure(.5f) < .2, "compressor attenuates above threshold");
    check(engine.setMicrophoneFilters(microphone, {suppression}), "Speex suppression");
    check(std::isfinite(measure(.005f)), "Speex emits finite buffered audio");
    if (types["noise_suppression"]["methods"].toArray().contains("rnnoise")) {
        auto ns = suppression; ns["settings"] = QJsonObject{{"method", "rnnoise"}, {"suppress_level", -30}};
        check(engine.setMicrophoneFilters(microphone, {ns}), "RNNoise runtime method");
        check(std::isfinite(measure(.005f)), "RNNoise emits finite buffered audio");
    }
    obs_source_remove_audio_capture_callback(native, callback, &capture); obs_source_release(native);
    check(engine.setMicrophoneFilters(microphone, {row("compressor", "compressor"), row("noise_gate", "gate"), suppression}), "UI fixture");
    std::unique_ptr<QDialog> dialog(feathercast::ui::createMicrophoneFiltersDialog(engine, microphone)); dialog->show(); events();
    auto* list = dialog->findChild<QListWidget*>(); if (!list) return 1;
    list->setCurrentRow(2); events();
    auto* level = control<QDoubleSpinBox>(*dialog, "Suppression level");
    auto* slider = control<QSlider>(*dialog, "Suppression level slider");
    check(level && slider && level->value() == -30, "Speex default and slider");
    if (level && slider) { slider->setValue(-24); check(level->value() == -24 && engine.microphoneFilters(microphone)[2].toObject()["settings"].toObject()["suppress_level"] == -24, "slider persists level"); }
    auto* defaults = control<QPushButton>(*dialog, "Restore selected filter defaults");
    if (defaults) defaults->click(); events();
    check(engine.microphoneFilters(microphone)[2].toObject()["settings"].toObject()["suppress_level"] == -30, "defaults restore selected settings");
    list->item(2)->setCheckState(Qt::Unchecked);
    auto* up = control<QPushButton>(*dialog, "Move filter up"); if (up) up->click(); events();
    check(engine.microphoneFilters(microphone)[1].toObject()["type"] == "noise_suppression" && !engine.microphoneFilters(microphone)[1].toObject()["enabled"].toBool(), "UI reorder and bypass");
    auto* down = control<QPushButton>(*dialog, "Move filter down"); if (down) down->click(); events();
    check(dialog->grab().save(out + "/microphone-filters-dark.png"), "dark dialog screenshot");
    feathercast::ui::applyTheme(false); events(); check(dialog->grab().save(out + "/microphone-filters-light.png"), "light dialog screenshot");
    auto* remove = control<QPushButton>(*dialog, "Remove filter"); if (remove) remove->click(); events();
    check(engine.microphoneFilters(microphone).size() == 2, "UI remove");
    auto* add = control<QPushButton>(*dialog, "Add filter");
    if (add && add->menu()) for (auto* action : add->menu()->actions()) if (action->text() == "Noise Suppression") action->trigger();
    events(); check(engine.microphoneFilters(microphone).size() == 3, "UI explicit add");
    check(engine.removeSource("Fixture", microphone), "remove microphone while dialog exists"); events();
    check(!list->isEnabled() && add && !add->isEnabled(), "removed source disables dialog safely");
    dialog.reset(); engine.shutdown();
    std::cout << (failed ? "MICROPHONE FILTERS FAILED\n" : "MICROPHONE FILTERS PASSED\n"); return failed ? 1 : 0;
}
