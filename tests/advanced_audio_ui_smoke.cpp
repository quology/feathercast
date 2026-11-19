#include "feathercast/engine/engine.hpp"
#include "feathercast/ui/advanced_audio_dialog.hpp"
#include "feathercast/ui/theme.hpp"

#include <QtWidgets>
#include <iostream>

namespace {
template<typename T>
T* control(QWidget& dialog, const QString& name) {
    for (auto* widget : dialog.findChildren<T*>())
        if (widget->accessibleName() == name) return widget;
    return nullptr;
}
void waitForEvents(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (app.arguments().size() != 3) return 2;
    app.setStyle("Fusion");
    feathercast::ui::applyTheme(true);
    const QString out = QFileInfo(app.arguments()[2]).absoluteFilePath();
    QDir().mkpath(out);
    feathercast::engine::Engine engine;
    auto settings = engine.defaultSettings();
    settings["width"] = 640; settings["height"] = 360; settings["fps"] = 30;
    settings["encoder"] = "obs_x264"; settings["outputDirectory"] = out;
    if (!engine.initialize(app.arguments()[1], settings)) return 1;
    if (!engine.addScene("Live") || !engine.addScene("Break") ||
        !engine.addSource("Live", "test_tone", "Studio microphone", {{"audio", QJsonObject{{"muted", true}}}}) ||
        !engine.addSource("Live", "test_tone", "Game audio", {{"audio", QJsonObject{{"tracks", 5}, {"muted", true}}}}) ||
        !engine.addSource("Break", "test_tone", "Intermission music", {{"audio", QJsonObject{{"muted", true}}}})) return 1;
    bool failed = false;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << "FAILED " << message << '\n'; failed = true; }
    };
    const QString name = "Studio microphone";
    std::unique_ptr<QDialog> dialog(feathercast::ui::createAdvancedAudioDialog(engine));
    dialog->show(); waitForEvents(700);
    auto* table = dialog->findChild<QTableWidget*>();
    auto* gain = control<QDoubleSpinBox>(*dialog, name + " volume");
    auto* mono = control<QCheckBox>(*dialog, name + " mono");
    auto* balance = control<QSlider>(*dialog, name + " balance");
    auto* sync = control<QSpinBox>(*dialog, name + " sync offset");
    auto* monitor = control<QComboBox>(*dialog, name + " audio monitoring");
    auto* mute = control<QCheckBox>(*dialog, name + " mute");
    QCheckBox *percent = nullptr, *activeOnly = nullptr;
    for (auto* check : dialog->findChildren<QCheckBox*>()) {
        if (check->text() == "Volume in %") percent = check;
        if (check->text() == "Active Sources Only") activeOnly = check;
    }
    if (!table || !gain || !mono || !balance || !sync || !monitor || !mute || !percent || !activeOnly) return 1;
    check(table->columnCount() == 8 && table->rowCount() == 3, "complete table across scenes");
    auto visible = [&] {
        int count = 0; for (int i = 0; i < table->rowCount(); ++i) if (!table->isRowHidden(i)) ++count;
        return count;
    };
    check(visible() == 2, "active-only excludes inactive scene");
    activeOnly->click();
    check(visible() == 3 && table->item(0, 1)->text() == "Inactive", "inactive status available");
    check(gain->value() == 0, "unity is zero dB");
    percent->click();
    check(gain->value() == 100, "unity is 100 percent");
    gain->setValue(72.3);
    const auto preciseGain = engine.audioProperties(name)["volume"].toDouble();
    percent->click(); percent->click(); percent->click();
    check(preciseGain == engine.audioProperties(name)["volume"].toDouble(), "unit switch leaves engine gain untouched");
    gain->setValue(gain->minimum());
    check(engine.audioProperties(name)["volume"] == 0 && gain->text().contains(QStringLiteral("∞")), "zero gain has distinct negative-infinity display");
    gain->setValue(-3);
    mono->click(); balance->setValue(62); sync->setValue(-75);
    mute->click(); check(!engine.audioProperties(name)["muted"].toBool(), "mute toggle updates engine");
    mute->click(); // Monitoring tests always use muted synthetic audio.
    check(monitor->count() == 3, "three monitoring modes");
    if (engine.audioMonitoringAvailable()) {
        monitor->setCurrentIndex(1);
        check(engine.audioProperties(name)["monitoring"] == "monitor_only", "monitor-only control updates engine");
        monitor->setCurrentIndex(2);
        check(engine.audioProperties(name)["monitoring"] == "monitor_and_output", "monitor and output control");
        monitor->setCurrentIndex(0);
    } else check(!monitor->isEnabled(), "unavailable device disables monitoring");
    for (int t = 1; t <= 6; ++t) {
        auto* checkBox = control<QCheckBox>(*dialog, name + " track " + QString::number(t));
        if (!checkBox) return 1;
        checkBox->click();
        check(bool(engine.audioProperties(name)["tracks"].toInt() & (1 << (t - 1))) == (t != 1), "track toggles independently");
    }
    auto properties = engine.audioProperties(name);
    check(properties["mono"] == true && properties["balance"] == .62 && properties["sync_offset_ms"] == -75 &&
          properties["tracks"] == 62, "controls write source audio settings");
    sync->setFocus(); waitForEvents(700);
    check(sync->hasFocus() && sync == control<QSpinBox>(*dialog, name + " sync offset"), "status polling preserves editor and focus");
    check(dialog->grab().save(out + "/advanced-audio.png"), "production widget screenshot");
    check(engine.startRecording(out + "/ui-recording.mkv"), "start recording with dialog open");
    auto* track = control<QCheckBox>(*dialog, name + " track 1");
    check(track && !track->isEnabled() && gain->isEnabled() && sync->isEnabled(), "only routing locked while recording");
    engine.stopRecording(); waitForEvents(800);
    check(track->isEnabled(), "routing unlocked after recording stops");
    activeOnly->click();
    engine.selectScene("Break"); waitForEvents(700);
    check(visible() == 1, "filter follows active scene");
    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Close)->click();
    check(!dialog->isVisible(), "close button dismisses dialog");
    dialog.reset(); engine.shutdown();
    std::cout << (failed ? "ADVANCED AUDIO UI FAILED\n" : "ADVANCED AUDIO UI PASSED\n");
    return failed ? 1 : 0;
}
