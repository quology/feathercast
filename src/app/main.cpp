#include "feathercast/engine/engine.hpp"
#include "feathercast/ui/automation_panel.hpp"
#include "feathercast/ui/advanced_audio_dialog.hpp"
#include "feathercast/ui/microphone_filters_dialog.hpp"
#include "feathercast/ui/theme.hpp"
#ifdef FEATHERCAST_TWITCH
#include "feathercast/ui/twitch_docks.hpp"
#endif
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QSaveFile>
#include <QtWidgets>
using feathercast::engine::Engine;
using feathercast::ui::applyTheme;
static QLabel *label(const QString &text, const char *role) {
  auto *result = new QLabel(text);
  result->setProperty("role", role);
  return result;
}
static void section(QBoxLayout *layout, const QString &title) {
  layout->addWidget(label(title, "section"));
}

class Preview : public QWidget {
public:
  Engine *engine;
  explicit Preview(Engine *e) : engine(e) {
    setAttribute(Qt::WA_NativeWindow);
    setMinimumSize(320, 180);
    setStyleSheet("background:#09090b;");
    setAccessibleName("Live scene preview");
  }
  void resizeEvent(QResizeEvent *event) override {
    QWidget::resizeEvent(event);
    engine->resizePreview(width(), height());
  }
};
class Window : public QMainWindow {
  Engine engine;
  QString dir, config;
  Preview *preview{};
  QListWidget *scenes{}, *sources{};
  QLabel *stats{}, *delayStatus{}, *meters{};
  QPushButton *stream{}, *record{}, *replay{}, *save{};
  QSpinBox *delay{};
  QTimer timer;
  feathercast::ui::AutomationPanel *automationPanel{};
  int saveTicks = 0;
  QProcess remuxProcess;
  QString remuxTemporary;
  bool refreshing = false, smoke = false;
  QHash<QString, bool> visibility;
  void saveConfig() {
    engine.saveConfiguration(config);
    QSettings s(dir + "/ui.ini", QSettings::IniFormat);
    s.setValue("geometry", saveGeometry());
    s.setValue("state", saveState());
    for (auto it = visibility.cbegin(); it != visibility.cend(); ++it)
      s.setValue("visibility/" + QString::fromLatin1(it.key().toUtf8().toBase64(
                                     QByteArray::Base64UrlEncoding)),
                 it.value());

  }
  QDockWidget *dock(const QString &title, QWidget *widget,
                    Qt::DockWidgetArea area) {
    auto d = new QDockWidget(title, this);
    d->setObjectName(title);
    d->setWidget(widget);
    if (auto *layout = widget->layout()) {
      layout->setContentsMargins(12, 10, 12, 12);
      layout->setSpacing(8);
    }
    addDockWidget(area, d);
    return d;
  }
  QPushButton *button(const QString &text, QBoxLayout *layout,
                      const std::function<void()> &fn) {
    auto b = new QPushButton(text);
    layout->addWidget(b);
    connect(b, &QPushButton::clicked, this, fn);
    return b;
  }
  void refreshScenes() {
    refreshing = true;
    QString current = engine.currentScene();
    scenes->clear();
    scenes->addItems(engine.scenes());
    auto found = scenes->findItems(current, Qt::MatchExactly);
    if (!found.empty())
      scenes->setCurrentItem(found.first());
    sources->clear();
    for (auto v : engine.sources(current)) {
      auto o = v.toObject();
      auto item = new QListWidgetItem(o["name"].toString(), sources);
      item->setData(Qt::UserRole, o["type"].toString());
      item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
      auto id = current + "/" + item->text();
      item->setCheckState(visibility.value(id, true) ? Qt::Checked
                                                     : Qt::Unchecked);
      engine.setSourceVisible(current, item->text(),
                              item->checkState() == Qt::Checked);
    }
    refreshing = false;
  }
  void addSource() {
    if (engine.currentScene().isEmpty())
      return;
    QDialog d(this);
    d.setWindowTitle("Add source");
    QFormLayout form(&d);
    d.setMinimumWidth(520);
    QComboBox type, option, audioApplication;
    QLineEdit name;
    QCheckBox captureAudio("Capture audio from this application only");
    QCheckBox separateTrack("Also save a separate recording track");
    QLabel audioNote;
    audioNote.setWordWrap(true);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    const QList<QPair<QString, QString>> types = {
        {"Display capture", "desktop"},     {"Window capture", "window"},
        {"Game capture", "game"},           {"Microphone", "microphone"},
        {"Desktop audio", "desktop_audio"}, {"Application audio", "application_audio"}, {"Color", "color"},
        {"Test tone", "test_tone"}};
    for (auto t : types)
      type.addItem(t.first, t.second);
    form.addRow("Type", &type);
    form.addRow("Name", &name);
    form.addRow("Device / target", &option);
    form.addRow(&captureAudio);
    form.addRow("Audio application", &audioApplication);
    form.addRow(&separateTrack);
    form.addRow(&audioNote);
    form.addRow(&buttons);
    auto updateAudio = [&] {
      const auto kind = type.currentData().toString();
      const bool visual = kind == "desktop" || kind == "window" || kind == "game";
      const bool available = engine.sourceAudioAvailable(kind);
      captureAudio.setVisible(visual);
      captureAudio.setEnabled(available);
      const bool audioEnabled = available && (!visual || captureAudio.isChecked());
      form.setRowVisible(&audioApplication, kind == "desktop" && audioEnabled);
      separateTrack.setVisible(visual || available);
      separateTrack.setEnabled(audioEnabled);
      if (!audioEnabled) separateTrack.setChecked(false);
      if (visual && !available)
        audioNote.setText("Application audio capture is unavailable in this Windows / capture runtime.");
      else if (audioEnabled)
        audioNote.setText(kind == "desktop"
          ? "A display has no single audio source. Choose an application: its audio joins the normal mix. A separate recording track contains only this application's audio. Mute Desktop audio to avoid duplicates."
          : "Audio joins the normal mix. A separate recording track contains only this source (up to five sources). Application capture includes the app, not an individual tab. Mute Desktop audio to avoid duplicates.");
      else
        audioNote.setText(visual ? "Enable audio to include the selected application's sound." : "");
      bool valid = !name.text().trimmed().isEmpty();
      if (kind == "application_audio" || (audioEnabled && (kind == "window" || kind == "game")))
        valid = valid && !option.currentData().toMap().value("value").toString().isEmpty();
      if (kind == "desktop" && audioEnabled)
        valid = valid && !audioApplication.currentData().toMap().value("value").toString().isEmpty();
      if (kind == "application_audio") valid = valid && available;
      buttons.button(QDialogButtonBox::Ok)->setEnabled(valid);
    };
    auto update = [&] {
      option.clear();
      for (auto v : engine.sourceOptions(type.currentData().toString())) {
        auto o = v.toObject();
        option.addItem(o["name"].toString(), o.toVariantMap());
      }
      audioApplication.clear();
      if (type.currentData().toString() == "desktop") {
        audioApplication.addItem("Select an application…");
        for (auto v : engine.sourceOptions("application_audio")) {
          auto o = v.toObject();
          if (!o["value"].toString().isEmpty()) audioApplication.addItem(o["name"].toString(), o.toVariantMap());
        }
      }
      captureAudio.setChecked(false);
      separateTrack.setChecked(false);
      name.setText(type.currentText());
      updateAudio();
    };
    connect(&type, &QComboBox::currentIndexChanged, &d, update);
    connect(&option, &QComboBox::currentIndexChanged, &d, updateAudio);
    connect(&audioApplication, &QComboBox::currentIndexChanged, &d, updateAudio);
    connect(&captureAudio, &QCheckBox::toggled, &d, updateAudio);
    connect(&name, &QLineEdit::textChanged, &d, updateAudio);
    update();
    connect(&buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    connect(&buttons, &QDialogButtonBox::accepted, &d, [&] {
      QJsonObject p;
      auto o = option.currentData().toMap();
      if (!o.isEmpty())
        p[o["property"].toString()] = QJsonValue::fromVariant(o["value"]);
      const auto kind = type.currentData().toString();
      if ((kind == "desktop" || kind == "window" || kind == "game") && captureAudio.isChecked() && captureAudio.isEnabled()) {
        p["capture_audio"] = true;
        if (kind == "desktop") p["audio_window"] = QJsonValue::fromVariant(audioApplication.currentData().toMap()["value"]);
      }
      if (separateTrack.isChecked() && separateTrack.isEnabled()) p["separate_audio_track"] = true;
      if (engine.addSource(engine.currentScene(), kind, name.text(), p)) d.accept();
    });
    d.exec();
  }
  void filtersDialog() {
    auto* item = sources->currentItem();
    if (!item || item->data(Qt::UserRole).toString() != "microphone") return;
    std::unique_ptr<QDialog> dialog(feathercast::ui::createMicrophoneFiltersDialog(engine, item->text(), this));
    dialog->exec();
    saveConfig();
  }
  void audioDialog() {
    std::unique_ptr<QDialog> dialog(feathercast::ui::createAdvancedAudioDialog(engine, this));
    dialog->exec();
    saveConfig();
  }
  void updateStats() {
    auto s = engine.statistics();
    QStringList levels;
    auto peaks = s["audioPeaksDb"].toObject();
    for (auto it = peaks.begin(); it != peaks.end(); ++it)
      levels << QString("%1: %2 dBFS")
                    .arg(it.key())
                    .arg(it.value().toDouble(), 0, 'f', 1);
    meters->setText(
        levels.isEmpty()
            ? "Add a microphone or desktop audio source to see live levels."
            : levels.join("\n"));
    bool a = s["streaming"].toBool(), b = s["recording"].toBool(),
         c = s["replay"].toBool();
    stream->setText(a ? "Stop streaming" : "Start streaming");
    record->setText(b ? "Stop recording" : "Start recording");
    replay->setText(c ? "Stop replay buffer" : "Start replay buffer");
    replay->setEnabled(engine.settings()["replayEnabled"].toBool());
    save->setEnabled(c && engine.settings()["replayEnabled"].toBool());
    QString transport = !a ? "Stopped"
                        : s["relayConnected"].toBool()
                            ? "Connected"
                            : "Starting / reconnecting";
    stats->setText(
        QString("Stream: %1\nRecording: %2 · Replay: %3\nFPS: %4 · Render: %5 "
                "ms\nRender lag: %6 frames\nMemory: %7 MiB · CPU/GPU: "
                "unavailable\nStream: %8 bytes · Recording: %9 bytes")
            .arg(transport, b ? "On" : "Off", c ? "On" : "Off")
            .arg(s["fps"].toDouble(), 0, 'f', 1)
            .arg(s["renderMs"].toDouble(), 0, 'f', 1)
            .arg(s["renderSkippedFrames"].toDouble(), 0, 'f', 0)
            .arg(s["memoryMiB"].toDouble(), 0, 'f', 1)
            .arg(s["streamBytes"].toDouble(), 0, 'f', 0)
            .arg(s["recordingBytes"].toDouble(), 0, 'f', 0));
    delayStatus->setText(
        QString("Target %1 s · Effective %2 s\nBuffer %3 MiB · Dropped/cut "
                "packets %4")
            .arg(s["targetDelayMs"].toDouble() / 1000, 0, 'f', 1)
            .arg(s["effectiveDelayMs"].toDouble() / 1000, 0, 'f', 1)
            .arg(s["delayBytes"].toDouble() / 1048576, 0, 'f', 1)
            .arg(s["delayDroppedPackets"].toDouble(), 0, 'f', 0));
  }
  void remuxDialog() {
    if (remuxProcess.state() != QProcess::NotRunning) {
      statusBar()->showMessage("A remux is already running", 5000);
      return;
    }
    auto input = QFileDialog::getOpenFileName(
        this, "Choose completed MKV recording",
        engine.settings()["outputDirectory"].toString(),
        "Matroska recordings (*.mkv)");
    if (input.isEmpty())
      return;
    auto output = QFileDialog::getSaveFileName(
        this, "Save remuxed MP4",
        QFileInfo(input).absolutePath() + "/" +
            QFileInfo(input).completeBaseName() + ".mp4",
        "MP4 (*.mp4)");
    if (output.isEmpty())
      return;
    if (QFileInfo::exists(output)) {
      QMessageBox::warning(
          this, "Remux",
          "Choose a new output filename. Existing files are preserved.");
      return;
    }
    if (engine.statistics()["recording"].toBool()) {
      QMessageBox::warning(this, "Remux",
                           "Stop recording before remuxing completed files.");
      return;
    }
    remuxTemporary =
        output + "." + QUuid::createUuid().toString(QUuid::Id128) + ".partial";
    remuxProcess.start(QCoreApplication::applicationDirPath() +
                           "/feathercast_remux.exe",
                       {QFileInfo(input).absoluteFilePath(),
                        QFileInfo(output).absoluteFilePath(), remuxTemporary});
    statusBar()->showMessage("Remuxing MKV to MP4… Original MKV is preserved.");
  }
  void settingsDialog();

public:
  Window(QString path, bool isSmoke) : dir(std::move(path)), smoke(isSmoke) {
    QDir().mkpath(dir);
    config = dir + "/engine.json";
    setWindowTitle(QGuiApplication::applicationDisplayName() + " " +
                   QCoreApplication::applicationVersion());
    resize(1280, 800);
    auto *header = addToolBar("Workspace");
    header->setObjectName("WorkspaceHeader");
    header->setMovable(false);
    header->setFloatable(false);
    auto *brandIcon = new QLabel;
    brandIcon->setPixmap(QApplication::windowIcon().pixmap(24));
    header->addWidget(brandIcon);
    header->addWidget(label(QGuiApplication::applicationDisplayName(), "heading"));
    header->addWidget(label("Video Encoding and Livestreaming Application", "muted"));
    auto *headerSpace = new QWidget;
    headerSpace->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    header->addWidget(headerSpace);
    auto *settingsButton = new QPushButton("Settings");
    settingsButton->setToolTip("Configure streaming, output and appearance");
    header->addWidget(settingsButton);
    connect(settingsButton, &QPushButton::clicked, this, [this] { settingsDialog(); });
    setDockOptions(AllowNestedDocks | AllowTabbedDocks | AnimatedDocks);
    connect(&remuxProcess, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus state) {
              QFile::remove(remuxTemporary);
              statusBar()->showMessage(
                  code == 0 && state == QProcess::NormalExit
                      ? "MP4 remux completed; original MKV preserved"
                      : "Remux failed or cancelled; original MKV preserved",
                  10000);
            });
    connect(&remuxProcess, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError) {
              QFile::remove(remuxTemporary);
              statusBar()->showMessage(
                  "Remux helper failed to start or exited unexpectedly", 10000);
            });
    connect(&engine, &Engine::errorOccurred, this, [this](const QString &m) {
      statusBar()->showMessage(m, 12000);
      if (!smoke)
        QMessageBox::warning(this, QGuiApplication::applicationDisplayName(), m);
    });
    connect(&engine, &Engine::replaySaved, this, [this](const QString &p) {
      statusBar()->showMessage("Replay saved: " + p, 10000);
    });
    if (!engine.initialize()) {
      QTimer::singleShot(0, this, &QWidget::close);
      return;
    }
    QSettings ui(dir + "/ui.ini", QSettings::IniFormat);
    if (QFileInfo::exists(config)) {
      QJsonObject legacy;
      ui.beginGroup("audio");
      for (const auto& key : ui.childGroups()) {
        const auto name = QString::fromUtf8(QByteArray::fromBase64(key.toLatin1(), QByteArray::Base64UrlEncoding));
        const double gain = qBound(0, ui.value(key + "/volume", 100).toInt(), 200) / 100.0;
        legacy[name] = QJsonObject{{"volume", gain}, {"muted", ui.value(key + "/muted", false).toBool()}};
      }
      ui.endGroup();
      if (engine.loadConfiguration(config, legacy) && !legacy.isEmpty() && engine.saveConfiguration(config))
        ui.remove("audio");
    }
    if (engine.scenes().isEmpty()) {
      engine.addScene("Scene");
      engine.addSource("Scene", "color", "Background");
    }
    preview = new Preview(&engine);
    auto *previewCard = new QWidget;
    previewCard->setObjectName("previewCard");
    auto *previewLayout = new QVBoxLayout(previewCard);
    previewLayout->setContentsMargins(12, 10, 12, 12);
    previewLayout->setSpacing(10);
    auto *previewHeading = new QHBoxLayout;
    previewHeading->addWidget(label("Preview", "section"));
    previewHeading->addStretch();
    previewHeading->addWidget(label("LIVE CANVAS", "badge"));
    previewLayout->addLayout(previewHeading);
    previewLayout->addWidget(preview, 1);
    setCentralWidget(previewCard);
    auto sceneBox = new QWidget;
    auto sl = new QVBoxLayout(sceneBox);
    scenes = new QListWidget;
    scenes->setAccessibleName("Scenes");
    scenes->setMinimumWidth(150);
    sl->addWidget(scenes);
    auto sr = new QHBoxLayout;
    sl->addLayout(sr);
    auto *addScene = button("Add scene", sr, [this] {
      bool ok = false;
      auto n = QInputDialog::getText(this, "Add scene", "Name",
                                     QLineEdit::Normal, {}, &ok);
      if (ok)
        engine.addScene(n);
    });
    auto *removeScene = button("Remove", sr, [this] {
      if (scenes->currentItem())
        engine.removeScene(scenes->currentItem()->text());
    });
    dock("Scenes", sceneBox, Qt::LeftDockWidgetArea);
    auto sourceBox = new QWidget;
    auto sol = new QVBoxLayout(sourceBox);
    sources = new QListWidget;
    sources->setAccessibleName("Sources and visibility");
    sources->setMinimumWidth(150);
    sol->addWidget(sources);
    auto sor = new QHBoxLayout;
    sol->addLayout(sor);
    auto *addSourceButton = button("Add source", sor, [this] { addSource(); });
    auto *removeSource = button("Remove", sor, [this] {
      if (sources->currentItem())
        engine.removeSource(engine.currentScene(),
                            sources->currentItem()->text());
    });
    auto* filters = button("Filters...", sol, [this] { filtersDialog(); });
    filters->setToolTip("Add compressor, noise gate and noise suppression to the selected microphone");
    filters->setEnabled(false);
    connect(sources, &QListWidget::currentItemChanged, this, [filters](QListWidgetItem* item) {
      filters->setEnabled(item && item->data(Qt::UserRole).toString() == "microphone");
    });
    sources->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(sources, &QListWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
      auto* item = sources->itemAt(pos);
      if (!item || item->data(Qt::UserRole).toString() != "microphone") return;
      sources->setCurrentItem(item);
      QMenu menu(this); menu.addAction("Filters...", this, [this] { filtersDialog(); });
      menu.exec(sources->viewport()->mapToGlobal(pos));
    });
    auto *sourceAudio = button("Advanced Audio Properties…", sol, [this] { audioDialog(); });
    sourceAudio->setToolTip("Adjust audio and track assignments for sources across all scenes");
    dock("Sources", sourceBox, Qt::LeftDockWidgetArea);
    connect(scenes, &QListWidget::currentTextChanged, this,
            [this](const QString &s) {
              if (!refreshing && !s.isEmpty()) {
                automationPanel->manualOverride();
                engine.selectScene(s);
              }
            });
    connect(sources, &QListWidget::itemChanged, this,
            [this](QListWidgetItem *i) {
              if (!refreshing) {
                visibility[engine.currentScene() + "/" + i->text()] =
                    i->checkState() == Qt::Checked;
                engine.setSourceVisible(engine.currentScene(), i->text(),
                                        i->checkState() == Qt::Checked);
              }
            });
    connect(&engine, &Engine::scenesChanged, this, [this] { refreshScenes(); });
    auto controls = new QWidget;
    auto cl = new QVBoxLayout(controls);
    section(cl, "Broadcast");
    stream = button("Start streaming", cl, [this] {
      engine.statistics()["streaming"].toBool() ? engine.stopStreaming()
                                                : void(engine.startStreaming());
    });
    stream->setProperty("variant", "primary");
    record = button("Start recording", cl, [this] {
      engine.statistics()["recording"].toBool() ? engine.stopRecording()
                                                : void(engine.startRecording());
    });
    section(cl, "Replay buffer");
    replay = button("Start replay buffer", cl, [this] {
      engine.statistics()["replay"].toBool() ? engine.stopReplay()
                                             : void(engine.startReplay());
    });
    save = button("Save replay", cl, [this] { engine.saveReplay(); });
    section(cl, "Recording tools");
    button("Remux MKV to MP4…", cl, [this] { remuxDialog(); });
    button("Cancel remux", cl, [this] {
      if (remuxProcess.state() != QProcess::NotRunning)
        remuxProcess.kill();
    });
    cl->addStretch();
    dock("Controls", controls, Qt::RightDockWidgetArea);
    auto dw = new QWidget;
    auto dl = new QVBoxLayout(dw);
    auto presets = new QComboBox;
    for (int n : {0, 15, 30, 60, 90, 120})
      presets->addItem(QString::number(n) + " s", n);
    presets->addItem("Custom", -1);
    dl->addWidget(label("Delay preset", "muted"));
    presets->setAccessibleName("Stream delay preset");
    dl->addWidget(presets);
    delay = new QSpinBox;
    delay->setAccessibleName("Target stream delay in seconds");
    delay->setRange(0, 300);
    delay->setSuffix(" s");
    delay->setValue(engine.settings()["delayMs"].toInt() / 1000);
    dl->addWidget(delay);
    button("Apply delay", dl,
           [this] { engine.setDelayMs(delay->value() * 1000); });
    connect(presets, &QComboBox::currentIndexChanged, this, [this, presets] {
      if (presets->currentData().toInt() >= 0)
        delay->setValue(presets->currentData().toInt());
    });
    delayStatus = new QLabel;
    delayStatus->setWordWrap(true);
    dl->addWidget(delayStatus);
    auto warning = new QLabel("Increasing delay holds output. Decreasing cuts "
                              "content. Holds may disconnect the server.");
    warning->setWordWrap(true);
    warning->setProperty("role", "muted");
    dl->addWidget(warning);
    auto *delayDock = dock("Stream delay", dw, Qt::BottomDockWidgetArea);
    meters = new QLabel;
    meters->setMargin(8);
    meters->setWordWrap(true);
    auto *audioDock = dock("Audio meters", meters, Qt::BottomDockWidgetArea);
    stats = new QLabel;
    stats->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    stats->setWordWrap(true);
    stats->setMargin(8);
    auto *statsDock = dock("Statistics", stats, Qt::BottomDockWidgetArea);
    automationPanel = new feathercast::ui::AutomationPanel(&engine, dir, this);
    auto *automationDock = dock("Automation", automationPanel, Qt::BottomDockWidgetArea);
    tabifyDockWidget(delayDock, audioDock);
    tabifyDockWidget(audioDock, statsDock);
    tabifyDockWidget(statsDock, automationDock);
    statsDock->raise();
    resizeDocks({findChildren<QDockWidget *>().first()}, {225}, Qt::Horizontal);
    resizeDocks({statsDock}, {190}, Qt::Vertical);
    statusBar()->showMessage("Ready · Your workspace is saved automatically", 6000);
#ifdef FEATHERCAST_TWITCH
    feathercast::ui::installTwitchDocks(this, dir);
#endif
    auto *file = menuBar()->addMenu("&File");
    file->addAction("Save configuration", this, [this] {
      saveConfig();
    });
    file->addAction("Open configuration folder", this, [this] {
      QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    file->addAction("Open output folder", this, [this] {
      QDesktopServices::openUrl(QUrl::fromLocalFile(engine.settings()["outputDirectory"].toString()));
    });
    file->addSeparator();
    file->addAction("Settings…", this, [this] { settingsDialog(); });
    file->addSeparator();
    file->addAction("Exit", this, &QWidget::close);
    auto *edit = menuBar()->addMenu("&Edit");
    edit->addAction("Add scene…", addScene, &QPushButton::click);
    auto *removeSceneAction = edit->addAction("Remove selected scene", removeScene, &QPushButton::click);
    edit->addSeparator();
    edit->addAction("Add source…", addSourceButton, &QPushButton::click);
    auto *removeSourceAction = edit->addAction("Remove selected source", removeSource, &QPushButton::click);
    auto *filtersAction = edit->addAction("Microphone filters…", filters, &QPushButton::click);
    connect(edit, &QMenu::aboutToShow, this, [this, removeSceneAction, removeSourceAction, filtersAction, filters] {
      removeSceneAction->setEnabled(scenes->currentItem());
      removeSourceAction->setEnabled(sources->currentItem());
      filtersAction->setEnabled(filters->isEnabled());
    });
    auto *view = menuBar()->addMenu("&View");
    auto previewToggle = view->addAction("Enable preview drawing");
    previewToggle->setCheckable(true);
    previewToggle->setChecked(engine.settings()["previewEnabled"].toBool());
    connect(previewToggle, &QAction::toggled, this,
            [this](bool on) { engine.setPreviewEnabled(on); });
    connect(view, &QMenu::aboutToShow, this, [this, previewToggle] {
      const QSignalBlocker blocker(previewToggle);
      previewToggle->setChecked(engine.settings()["previewEnabled"].toBool());
    });
    view->addAction("Advanced Audio Properties…", this, [this] { audioDialog(); });
    auto *docks = menuBar()->addMenu("&Docks");
    for (auto d : findChildren<QDockWidget *>())
      docks->addAction(d->toggleViewAction());
    auto *profile = menuBar()->addMenu("&Profile");
    profile->addAction("Default (single profile)")->setEnabled(false);
    auto *collections = menuBar()->addMenu("&Scene Collection");
    collections->addAction("Single scene collection")->setEnabled(false);
    auto *tools = menuBar()->addMenu("&Tools");
    tools->addAction("Global hotkeys…", this,
                     [this] { automationPanel->editHotkeys(this); });
    tools->addAction("Remux MKV to MP4…", this, [this] { remuxDialog(); });
    auto *help = menuBar()->addMenu("&Help");
    help->addAction("About VELA", this, [this] {
      QMessageBox::about(this, "About VELA", "VELA " + QCoreApplication::applicationVersion() +
                         "\nVideo Encoding and Livestreaming Application");
    });
    ui.beginGroup("visibility");
    for (const auto &k : ui.childKeys())
      visibility[QString::fromUtf8(QByteArray::fromBase64(
          k.toLatin1(), QByteArray::Base64UrlEncoding))] = ui.value(k).toBool();
    ui.endGroup();
    refreshScenes();
    restoreGeometry(ui.value("geometry").toByteArray());
    restoreState(ui.value("state").toByteArray());
    connect(&timer, &QTimer::timeout, this, [this] {
      updateStats();
      if (++saveTicks % 30 == 0)
        saveConfig();
    });
    timer.start(1000);
    updateStats();
    QTimer::singleShot(0, this, [this] {
      if (preview)
        engine.attachPreview(preview->winId(), preview->width(),
                             preview->height());
    });
  }
  QJsonObject selfTest() {
    return automationPanel ? automationPanel->selfTest()
                           : QJsonObject{{"initialized", false}};
  }
  void closeEvent(QCloseEvent *e) override {
    timer.stop();
    if (remuxProcess.state() != QProcess::NotRunning) {
      remuxProcess.kill();
      remuxProcess.waitForFinished(3000);
      QFile::remove(remuxTemporary);
    }
    if (automationPanel) {
      automationPanel->save();
      delete automationPanel;
      automationPanel = nullptr;
    }
    if (engine.initialized()) {
      saveConfig();
      engine.attachPreview(0, 0, 0);
      engine.shutdown();
    }
    QMainWindow::closeEvent(e);
  }
};
void Window::settingsDialog() {
  QDialog d(this);
  d.setWindowTitle("Settings");
  d.resize(900, 620);
  auto outer = new QVBoxLayout(&d);
  outer->setContentsMargins(20, 18, 20, 18);
  outer->setSpacing(14);
  outer->addWidget(label("Settings", "heading"));
  outer->addWidget(label("Configure your broadcast and workspace.", "muted"));
  auto *body = new QHBoxLayout;
  body->setSpacing(16);
  auto *navigation = new QListWidget;
  navigation->setAccessibleName("Settings categories");
  navigation->setFixedWidth(180);
  auto *tabs = new QStackedWidget;
  body->addWidget(navigation);
  body->addWidget(tabs, 1);
  outer->addLayout(body, 1);
  connect(navigation, &QListWidget::currentRowChanged, tabs,
          &QStackedWidget::setCurrentIndex);
  auto s = engine.settings();
  auto page = [&](QString name) {
    auto w = new QWidget;
    auto f = new QFormLayout(w);
    f->setContentsMargins(16, 14, 16, 14);
    f->setSpacing(12);
    f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    f->addRow(label(name, "section"));
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(w);
    tabs->addWidget(scroll);
    navigation->addItem(name);
    return f;
  };
  auto number = [&](QFormLayout *f, QString label, QString key, int min,
                    int max) {
    auto b = new QSpinBox;
    b->setRange(min, max);
    b->setValue(s[key].toInt());
    f->addRow(label, b);
    return b;
  };
  auto general = page("General");
  auto folder = new QLineEdit(s["outputDirectory"].toString());
  auto browse = new QPushButton("Browse…");
  auto row = new QWidget;
  auto rl = new QHBoxLayout(row);
  rl->setContentsMargins(0, 0, 0, 0);
  rl->addWidget(folder);
  rl->addWidget(browse);
  general->addRow("Output folder", row);
  connect(browse, &QPushButton::clicked, &d, [&] {
    auto p =
        QFileDialog::getExistingDirectory(&d, "Output folder", folder->text());
    if (!p.isEmpty())
      folder->setText(QDir(p).absolutePath());
  });
  auto previewOn = new QCheckBox("Enable preview drawing");
  previewOn->setChecked(s["previewEnabled"].toBool());
  general->addRow(previewOn);
  auto replayOn = new QCheckBox("Enable replay buffer module");
  replayOn->setChecked(s["replayEnabled"].toBool());
  general->addRow(replayOn);
  auto pluginsOn = new QCheckBox("Enable trusted plugins module");
  pluginsOn->setChecked(s["pluginsEnabled"].toBool());
  general->addRow(pluginsOn);
  auto theme = new QComboBox;
  theme->addItems({"Dark", "Light"});
  QSettings themeSettings(dir + "/ui.ini", QSettings::IniFormat);
  theme->setCurrentIndex(themeSettings.value("theme", 0).toInt());
  general->addRow("Theme", theme);
  general->addRow(new QLabel(
      "Layout and configuration are saved every 30 seconds and on exit."));
  auto st = page("Stream");
  auto service = new QComboBox;
  service->addItems({"Custom", "Twitch", "YouTube"});
  auto endpoint = new QLineEdit(s["streamEndpoint"].toString());
  auto key = new QLineEdit;
  key->setEchoMode(QLineEdit::Password);
  key->setPlaceholderText(engine.hasStreamKey()
                              ? "Stored securely; leave blank to keep"
                              : "Stream key");
  st->addRow("Service", service);
  st->addRow("Base endpoint", endpoint);
  st->addRow("Stream key", key);
  st->addRow(new QLabel("Key is protected separately with Windows DPAPI."));
  connect(service, &QComboBox::currentIndexChanged, &d, [=](int i) {
    if (i == 1)
      endpoint->setText("rtmp://live.twitch.tv/app");
    if (i == 2)
      endpoint->setText("rtmp://a.rtmp.youtube.com/live2");
  });
  auto out = page("Output");
  auto encoder = new QComboBox;
  encoder->addItem("Automatic", "auto");
  encoder->addItem("Software x264", "obs_x264");
  encoder->addItem("NVIDIA NVENC", "obs_nvenc_h264_tex");
  encoder->setCurrentIndex(qMax(0, encoder->findData(s["encoder"].toString())));
  out->addRow("Encoder", encoder);
  auto vb =
      number(out, "Video bitrate (kbps)", "videoBitrateKbps", 100, 100000);
  auto ab = number(out, "Audio bitrate (kbps)", "audioBitrateKbps", 32, 320);
  auto kf = number(out, "Keyframe interval (s)", "keyframeSeconds", 1, 10);
  auto au = page("Audio");
  au->addRow(
      new QLabel("Open Advanced Audio Properties from Sources or the View menu to adjust gain, monitoring and tracks. "
                 "Live source peak levels appear in the Audio meters dock."));
  auto vid = page("Video");
  auto w = number(vid, "Width", "width", 320, 7680);
  auto h = number(vid, "Height", "height", 180, 4320);
  auto fps = number(vid, "FPS", "fps", 1, 240);
  auto hot = page("Hotkeys");
  auto hotkeys = new QPushButton("Configure global hotkeys…");
  hot->addRow(hotkeys);
  connect(hotkeys, &QPushButton::clicked, &d,
          [&] { automationPanel->editHotkeys(&d); });
  auto rep = page("Replay buffer");
  auto seconds = number(rep, "Duration (seconds)", "replaySeconds", 1, 300);
  auto durationPresets = new QComboBox;
  for (int n : {15, 30, 60, 120, 300})
    durationPresets->addItem(QString::number(n) + " seconds", n);
  rep->addRow("Duration presets", durationPresets);
  connect(durationPresets, &QComboBox::currentIndexChanged, &d,
          [=] { seconds->setValue(durationPresets->currentData().toInt()); });
  auto rm = number(rep, "Memory cap (MiB)", "replayMemoryMb", 16, 2048);
  auto del = page("Stream delay");
  auto dm = number(del, "Memory cap (MiB)", "delayMemoryMb", 16, 2048);
  del->addRow(
      new QLabel("Use the main window to change delay from 0 to 300 seconds."));
  auto automation = page("Scene automation");
  auto note = new QLabel(
      "Use the Automation dock to edit rules, OR groups, AND conditions, "
      "ordered actions, priority, debounce and cooldown. Manual scene "
      "selection pauses automation for 30 seconds.");
  note->setWordWrap(true);
  automation->addRow(note);
  auto plugins = page("Plugins");
  auto list = new QListWidget;
  auto reload = [&] {
    list->clear();
    for (auto v : engine.pluginActions()) {
      auto o = v.toObject();
      auto i = new QListWidgetItem(
          o["plugin"].toString() + " — " + o["name"].toString(), list);
      i->setData(Qt::UserRole, o["id"].toString());
    }
  };
  reload();
  plugins->addRow(list);
  auto load = new QPushButton("Load trusted plugin DLL…");
  plugins->addRow(load);
  connect(load, &QPushButton::clicked, &d, [&] {
    auto p = QFileDialog::getOpenFileName(&d, "Load plugin", QDir::homePath(),
                                          "DLL files (*.dll)");
    if (!p.isEmpty() && engine.loadPlugin(QFileInfo(p).absoluteFilePath()))
      reload();
  });
  auto invoke = new QPushButton("Invoke selected action…");
  plugins->addRow(invoke);
  connect(invoke, &QPushButton::clicked, &d, [&] {
    if (!list->currentItem())
      return;
    bool ok;
    auto args = QInputDialog::getText(&d, "Plugin action", "JSON arguments",
                                      QLineEdit::Normal, "{}", &ok);
    if (ok)
      engine.invokePluginAction(
          list->currentItem()->data(Qt::UserRole).toString(), args);
  });
  auto advanced = page("Advanced");
  advanced->addRow(new QLabel("Plugins run trusted code in this process and "
                              "are loaded only when selected."));
  auto perf = page("Performance");
  perf->addRow(new QLabel("CPU and GPU utilization readings are unavailable. "
                          "Audio peak meters use real libobs callbacks."));
  auto box =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  outer->addWidget(box);
  connect(box, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  connect(box, &QDialogButtonBox::accepted, &d, [&] {
    if (!QFileInfo(folder->text()).isAbsolute()) {
      QMessageBox::warning(&d, "Settings",
                           "Choose an absolute output directory.");
      return;
    }
    s["outputDirectory"] = QDir::cleanPath(folder->text());
    s["streamEndpoint"] = endpoint->text().trimmed();
    s["encoder"] = encoder->currentData().toString();
    s["videoBitrateKbps"] = vb->value();
    s["audioBitrateKbps"] = ab->value();
    s["keyframeSeconds"] = kf->value();
    s["width"] = w->value();
    s["height"] = h->value();
    s["fps"] = fps->value();
    s["replaySeconds"] = seconds->value();
    s["replayMemoryMb"] = rm->value();
    s["delayMemoryMb"] = dm->value();
    s["previewEnabled"] = previewOn->isChecked();
    s["replayEnabled"] = replayOn->isChecked();
    s["pluginsEnabled"] = pluginsOn->isChecked();
    if (!engine.applySettings(s))
      return;
    if (!key->text().isEmpty() && !engine.storeStreamKey(key->text()))
      return;
    engine.setPreviewEnabled(previewOn->isChecked());
    themeSettings.setValue("theme", theme->currentIndex());
    applyTheme(theme->currentIndex() == 0);
    d.accept();
  });
  navigation->setCurrentRow(0);
  for (auto *note : d.findChildren<QLabel *>())
    note->setWordWrap(true);
  d.exec();
}
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  // These identifiers are the existing on-disk profile and DPAPI vault paths.
  QCoreApplication::setOrganizationName("FeatherCast");
  QCoreApplication::setApplicationName("FeatherCast");
  app.setApplicationDisplayName("VELA");
  app.setApplicationVersion(VELA_VERSION);
  app.setWindowIcon(QIcon(":/vela/vela.ico"));
  QCommandLineParser parser;
  parser.setApplicationDescription("VELA - Video Encoding and Livestreaming Application");
  parser.addHelpOption();
  QCommandLineOption smoke("smoke-ui", "Run UI smoke mode");
  QCommandLineOption quit("quit-after", "Quit after seconds", "seconds");
  QCommandLineOption config("config-directory",
                            "Absolute configuration directory", "path");
  QCommandLineOption screenshot("screenshot",
                                "Save a real widget screenshot after startup",
                                "absolutePNG");
  QCommandLineOption selftest("ui-self-test",
                              "Run integrated UI checks and save results",
                              "absoluteJSON");
  parser.addOptions({smoke, quit, config, screenshot, selftest});
  parser.process(app);
  auto dir = parser.value(config);
  if (dir.isEmpty())
    dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (!QFileInfo(dir).isAbsolute())
    return 2;
  dir = QDir::cleanPath(dir);
  if (parser.isSet(config)) {
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setApplicationName("FeatherCast-Test-" +
                                         QString::number(qHash(dir)));
  }
  app.setStyle("Fusion");
  QSettings themeSettings(dir + "/ui.ini", QSettings::IniFormat);
  applyTheme(themeSettings.value("theme", 0).toInt() != 1);
  int testExit = 0;
  Window window(dir, parser.isSet(smoke) || parser.isSet(selftest));
  window.show();
  if (parser.isSet(screenshot))
    QTimer::singleShot(2000, &window, [&] {
      auto path = parser.value(screenshot);
      if (QFileInfo(path).isAbsolute())
        if (!window.grab().save(path))
          testExit = 2;
    });
  if (parser.isSet(selftest))
    QTimer::singleShot(2500, &window, [&] {
      auto path = parser.value(selftest);
      if (QFileInfo(path).isAbsolute()) {
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
          auto results = window.selfTest();
          for (auto value : results)
            if (value.isBool() && !value.toBool())
              testExit = 1;
          file.write(QJsonDocument(results).toJson());
          file.commit();
        }
      }
    });
  if (parser.isSet(quit))
    QTimer::singleShot(qMax(0, parser.value(quit).toInt()) * 1000, &window,
                       &QWidget::close);
  int result = app.exec();
  return result ? result : testExit;
}
