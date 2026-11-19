#include "feathercast/engine/engine.hpp"
#include "feathercast/ui/source_editors.hpp"
#include <obs.h>
#include <graphics/matrix4.h>
#include <QApplication>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QEventLoop>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <iostream>
#include <cmath>
using feathercast::engine::Engine;
static void waitMs(int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); }
// Independent native measurement: child draw matrix followed by every ancestor
// draw matrix, matching OBS scene render's matrix stack. Root lock prevents a
// group rebasing halfway through this snapshot.
struct WorldSearch { QString name; matrix4 parent; QJsonObject* result; };
static void walkWorld(obs_scene_t* scene, WorldSearch& state) {
    obs_scene_enum_items(scene, [](obs_scene_t*, obs_sceneitem_t* item, void* data) {
        auto& state = *static_cast<WorldSearch*>(data);
        matrix4 local{}, combined{}; obs_sceneitem_get_draw_transform(item, &local); matrix4_mul(&combined, &local, &state.parent);
        if (state.name == QString::fromUtf8(obs_source_get_name(obs_sceneitem_get_source(item)))) {
            *state.result = {{"m11", combined.x.x}, {"m12", combined.x.y}, {"m21", combined.y.x}, {"m22", combined.y.y}, {"dx", combined.t.x}, {"dy", combined.t.y}}; return false;
        }
        if (obs_sceneitem_is_group(item)) { WorldSearch child{state.name, combined, state.result}; walkWorld(obs_sceneitem_group_get_scene(item), child); }
        return state.result->isEmpty();
    }, &state);
}
static QJsonObject world(const QString& sceneName, const QString& name) {
    auto* source = obs_get_source_by_name(sceneName.toUtf8().constData());
    auto* scene = obs_scene_from_source(source); QJsonObject result;
    matrix4 identity{}; matrix4_identity(&identity); WorldSearch state{name, identity, &result};
    obs_scene_atomic_update(scene, [](void* data, obs_scene_t* scene) { walkWorld(scene, *static_cast<WorldSearch*>(data)); }, &state);
    obs_source_release(source); return result;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv); if (app.arguments().size() != 3) return 2;
    const auto out = QFileInfo(app.arguments()[2]).absoluteFilePath(); QDir().mkpath(out);
    Engine e; auto config = Engine::defaultSettings(); config["width"] = 1280; config["height"] = 720; config["fps"] = 60;
    if (!e.initialize(app.arguments()[1], config)) return 2;
    e.addScene("Coordinates"); e.addGroup("Coordinates", "Group"); e.addSource("Coordinates", "color", "Color"); e.moveSource("Coordinates", "Color", "Group", 0); waitMs(200);
    QWidget parent; parent.setAttribute(Qt::WA_DontShowOnScreen); parent.setProperty("workspaceFixture", true);
    QJsonArray samples; bool pendingEditPreserved = false;
    QTimer::singleShot(20, &parent, [&] {
        auto* dialog = parent.findChild<QDialog*>("sourcePropertiesDialog"); if (!dialog) return;
        auto* width = dialog->findChild<QDoubleSpinBox*>("property_width"); auto* x = dialog->findChild<QDoubleSpinBox*>("transform_x"); auto* apply = dialog->findChild<QPushButton*>("applySource");
        if (!width || !x || !apply) { dialog->reject(); return; }
        width->setValue(1281); x->setValue(37);
        for (int i = 0; i < 4; ++i) {
            apply->click(); waitMs(150);
            samples.append(QJsonObject{{"apply", i+1}, {"widget_x", x->value()}, {"child", e.sourceTransform("Coordinates", "Color")}, {"group", e.sourceTransform("Coordinates", "Group")}, {"world", world("Coordinates", "Color")}});
        }
        x->setValue(9); waitMs(150); pendingEditPreserved = x->value() == 9;
        dialog->reject();
    });
    feathercast::ui::editSource(e, "Coordinates", "Color", &parent);
    const auto close = [](const QJsonObject& a, const QJsonObject& b) {
        if (a.isEmpty() || a.keys() != b.keys()) return false;
        for (auto it = a.begin(); it != a.end(); ++it) if (qAbs(it.value().toDouble() - b[it.key()].toDouble()) > .01) return false;
        return true;
    };
    bool stable = samples.size() == 4;
    for (auto sample : samples) stable &= qAbs(sample.toObject()["world"].toObject()["dx"].toDouble() - 37) < .01;
    QJsonObject report{{"repeated_apply_preserves_world_x37", stable}, {"pending_editor_change_preserved", pendingEditPreserved}, {"samples", samples}};
    report["native_matches_engine_world"] = close(world("Coordinates", "Color"), e.sourceWorldTransform("Coordinates", "Color"));
    bool snapshots = true;
    for (int edit = 0; edit < 12; ++edit) {
        waitMs(35);
        const auto parentX = e.sourceTransform("Coordinates", "Group")["x"].toDouble();
        const double localX = edit + 1;
        e.setSourceProperties("Coordinates", "Color", {{"width", 1280 + edit}});
        e.setSourceTransform("Coordinates", "Color", {{"x", localX}});
        // Exercise saves on both sides of native rebasing, while render continues.
        for (int sample = 0; sample < 12; ++sample) {
            const auto path = out + "/snapshot.json"; snapshots &= e.saveConfiguration(path);
            QFile saved(path); saved.open(QIODevice::ReadOnly); const auto doc = QJsonDocument::fromJson(saved.readAll()).object();
            double parent = 0, child = 0;
            for (auto row : doc["scenes"].toArray()[0].toObject()["sources"].toArray()) {
                const auto entry = row.toObject(); if (entry["name"] == "Group") parent = entry["transform"].toObject()["x"].toDouble();
                if (entry["name"] == "Color") child = entry["transform"].toObject()["x"].toDouble();
            }
            snapshots &= qAbs(parent + child - parentX - localX) < .01;
        }
    }
    report["atomic_parent_child_snapshots"] = snapshots;
    bool pendingRoundtrip = true; QJsonArray pendingSamples;
    for (int edit = 0; edit < 6; ++edit) {
        waitMs(40); const double expectedX = e.sourceTransform("Coordinates", "Group")["x"].toDouble() + 11;
        e.setSourceProperties("Coordinates", "Color", {{"width", 1300 + edit}});
        e.setSourceTransform("Coordinates", "Color", {{"x", 11}});
        pendingRoundtrip &= e.saveConfiguration(out + "/pending.json");
        QFile pending(out + "/pending.json"); pending.open(QIODevice::ReadOnly); const auto savedPending = QJsonDocument::fromJson(pending.readAll()).object(); pending.close();
        pendingRoundtrip &= e.loadConfiguration(out + "/pending.json");
        waitMs(100); const auto actual = world("Coordinates", "Color"); pendingRoundtrip &= qAbs(actual["dx"].toDouble() - expectedX) < .01;
        pendingSamples.append(QJsonObject{{"expected_x", expectedX}, {"actual", actual}, {"saved", savedPending}, {"restored", e.sources("Coordinates")}});
    }
    report["pending_edit_reload_world_placement"] = pendingRoundtrip; report["pending_samples"] = pendingSamples;
    waitMs(100);
    e.addGroup("Coordinates", "Outer"); e.moveSource("Coordinates", "Group", "Outer", 0); waitMs(100);
    e.setSourceTransform("Coordinates", "Outer", {{"x", 210}, {"y", 95}, {"rotation", 17}, {"scaleX", 1.25}, {"scaleY", .8}}); waitMs(150);
    const auto expectedWorld = world("Coordinates", "Color"); report["nested_native_world"] = expectedWorld;
    report["nested_matrix_matches_engine"] = close(expectedWorld, e.sourceWorldTransform("Coordinates", "Color"));
    bool duplicated = e.duplicateScene("Coordinates", "Copy"); e.selectScene("Copy"); e.takePreview("cut"); waitMs(150);
    report["duplicate_world_placement"] = duplicated && close(expectedWorld, world("Copy", "Copy / Color"));
    e.selectScene("Coordinates"); e.takePreview("cut"); e.removeScene("Copy"); waitMs(100);
    bool roundtrip = e.saveConfiguration(out + "/engine.json") && e.loadConfiguration(out + "/engine.json"); waitMs(150);
    report["reload_world_placement"] = roundtrip && close(expectedWorld, world("Coordinates", "Color"));
    report["reloaded_native_world"] = world("Coordinates", "Color");
    waitMs(550); const double cpu = e.statistics()["cpuPercent"].toDouble(); bool finiteCachedCpu = std::isfinite(cpu);
    for (int i = 0; i < 100; ++i) { const double sample = e.statistics()["cpuPercent"].toDouble(); finiteCachedCpu &= std::isfinite(sample) && sample == cpu; }
    report["finite_cached_cpu_burst"] = finiteCachedCpu;
    bool passed = true; for (auto value : report) if (value.isBool() && !value.toBool()) passed = false;
    QFile file(out + "/coordinates.json"); file.open(QIODevice::WriteOnly); file.write(QJsonDocument(report).toJson()); file.close();
    e.shutdown();
    std::cerr << (passed ? "COORDINATES PASSED\n" : "COORDINATES FAILED\n"); return passed ? 0 : 1;
}
