#include "feathercast/engine/engine.hpp"
#include "feathercast/ui/automation_panel.hpp"
#include "feathercast/ui/advanced_audio_dialog.hpp"
#include "feathercast/ui/microphone_filters_dialog.hpp"
#include "feathercast/ui/theme.hpp"
#include "feathercast/ui/mixer.hpp"
#include "feathercast/ui/source_editors.hpp"
#ifdef FEATHERCAST_TWITCH
#include "feathercast/ui/twitch_docks.hpp"
#endif
#include <QCommandLineParser>
#include <QJsonDocument>
#include <QSaveFile>
#include <QtWidgets>
#include <utility>
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
  bool program;
  explicit Preview(Engine *e, bool isProgram = false) : engine(e), program(isProgram) {
    setAttribute(Qt::WA_NativeWindow);
    setMinimumSize(320, 180);
    setStyleSheet("background:#09090b;");
    setAccessibleName("Live scene preview");
  }
  void resizeEvent(QResizeEvent *event) override {
    QWidget::resizeEvent(event);
    if (program) engine->resizeProgram(width(), height());
    else engine->resizePreview(width(), height());
  }
};
class Window : public QMainWindow {
  Engine engine;
  QString dir, config;
  Preview *preview{}, *program{};
  QStackedWidget *previewStack{};
  QWidget *programCard{};
  QPushButton *studioButton{};
  QLabel *statusMetrics{};
  QListWidget *scenes{};
  QTreeWidget *sources{};
  QLabel *stats{}, *delayStatus{};
  feathercast::ui::Mixer *mixer{};
  QPushButton *stream{}, *record{}, *replay{}, *save{};
  QSpinBox *delay{};
  QTimer timer, mixerTimer;
  feathercast::ui::AutomationPanel *automationPanel{};
  int saveTicks = 0;
  QProcess remuxProcess;
  QString remuxTemporary;
  bool refreshing = false, smoke = false, refreshPending = false;
  bool previewAttached = false, programAttached = false;
  QString pendingSourceSelection;
  void syncPreview() {
    if (!previewStack || !programCard) return;
    const bool enabled = engine.settings()["previewEnabled"].toBool();
    previewStack->setCurrentIndex(enabled ? 0 : 1);
    const bool studio = engine.studioMode();
    programCard->setVisible(studio);
    if (studioButton) studioButton->setText(studio ? "Exit Studio Mode" : "Studio Mode");
    if (isVisible()) {
      if (enabled != previewAttached) { engine.attachPreview(enabled ? preview->winId() : 0, preview->width(), preview->height()); previewAttached = enabled; }
      if (studio != programAttached) { engine.attachProgram(studio ? program->winId() : 0, program->width(), program->height()); programAttached = studio; }
    }
  }
  void resetWorkspaceLayout() {
    auto find = [this](const char *name) { return findChild<QDockWidget *>(name); };
    auto *scenesDock = find("Scenes"), *sourcesDock = find("Sources"), *audioDock = find("Audio meters"), *controlsDock = find("Controls"), *transitionDock = find("Scene Transitions");
    for (auto *d : {scenesDock, sourcesDock, audioDock, controlsDock, transitionDock}) if (d) { d->setFloating(false); addDockWidget(Qt::BottomDockWidgetArea, d); d->show(); }
    if (scenesDock && sourcesDock) splitDockWidget(scenesDock, sourcesDock, Qt::Horizontal);
    if (sourcesDock && audioDock) splitDockWidget(sourcesDock, audioDock, Qt::Horizontal);
    if (audioDock && controlsDock) splitDockWidget(audioDock, controlsDock, Qt::Horizontal);
    if (sourcesDock && transitionDock) { tabifyDockWidget(sourcesDock, transitionDock); sourcesDock->raise(); }
    for (const char *name : {"Stream delay", "Statistics", "Automation"}) if (auto *d = find(name)) d->hide();
    auto *twitchStats = find("Twitch stats"), *activity = find("Twitch activity feed"), *chat = find("Twitch chat"), *channel = find("Twitch channel");
    if (twitchStats && audioDock) { addDockWidget(Qt::BottomDockWidgetArea, twitchStats); splitDockWidget(audioDock, twitchStats, Qt::Horizontal); twitchStats->show(); }
    if (activity) { addDockWidget(Qt::RightDockWidgetArea, activity); activity->show(); }
    if (chat) { addDockWidget(Qt::RightDockWidgetArea, chat); if (activity) splitDockWidget(activity, chat, Qt::Horizontal); chat->show(); }
    if (channel && chat) { addDockWidget(Qt::RightDockWidgetArea, channel); tabifyDockWidget(chat, channel); channel->show(); chat->raise(); }
    if (scenesDock && sourcesDock && audioDock && controlsDock) resizeDocks({scenesDock,sourcesDock,audioDock,controlsDock}, {200,280,450,220}, Qt::Horizontal);
    if (audioDock) QTimer::singleShot(0, this, [this,audioDock] { resizeDocks({audioDock}, {height() < 900 ? 320 : 350}, Qt::Vertical); });
  }
  void saveConfig() {
    engine.saveConfiguration(config);
    QSettings s(dir + "/ui.ini", QSettings::IniFormat);
    s.setValue("geometry", saveGeometry());
    s.setValue("state", saveState());


  }
  QDockWidget *dock(const QString &title, QWidget *widget,
                    Qt::DockWidgetArea area) {
    auto d = new QDockWidget(title, this);
    d->setObjectName(title);
    auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumSize(0, 0); scroll->setWidget(widget); d->setWidget(scroll);
    if (auto *layout = widget->layout()) {
      layout->setContentsMargins(4, 4, 4, 4);
      layout->setSpacing(4);
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
  void scheduleSceneRefresh() {
    if (refreshPending) return;
    refreshPending = true;
    QTimer::singleShot(0, this, [this] { refreshPending = false; refreshScenes(); });
  }
  void refreshScenes() {
    refreshing = true;
    QString current = engine.currentScene();
    scenes->clear();
    scenes->addItems(engine.scenes());
    auto found = scenes->findItems(current, Qt::MatchExactly);
    if (!found.empty())
      scenes->setCurrentItem(found.first());
    const bool sourceFocus = sources->hasFocus();
    const QString selected = !pendingSourceSelection.isEmpty() ? std::exchange(pendingSourceSelection, QString()) : (sources->currentItem() ? sources->currentItem()->text(0) : QString());
    QSet<QString> expanded;
    QTreeWidgetItemIterator iterator(sources);
    while (*iterator) { if ((*iterator)->isExpanded()) expanded.insert((*iterator)->text(0)); ++iterator; }
    sources->clear();
    const auto rows = engine.sources(current);
    std::function<void(const QString &, QTreeWidgetItem *)> append = [&](const QString &parent, QTreeWidgetItem *container) {
      for (int n = rows.size() - 1; n >= 0; --n) {
        const auto o = rows[n].toObject();
        if (o["parent"].toString() != parent) continue;
        auto *item = container ? new QTreeWidgetItem(container) : new QTreeWidgetItem(sources);
        item->setText(0, o["name"].toString());
        const QHash<QString, QString> typeNames{{"desktop","Display Capture"},{"window","Window Capture"},{"game","Game Capture"},{"microphone","Microphone"},{"desktop_audio","Desktop Audio"},{"application_audio","Application Audio"},{"color","Color"},{"test_tone","Test Tone"},{"group","Group"}};
        item->setText(1, typeNames.value(o["type"].toString(), o["type"].toString()));
        item->setData(0, Qt::UserRole, o["type"].toString());
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, o["visible"].toBool(true) ? Qt::Checked : Qt::Unchecked);
        item->setCheckState(2, o["locked"].toBool() ? Qt::Checked : Qt::Unchecked);
        append(item->text(0), item);
        item->setExpanded(expanded.contains(item->text(0)));
        if (item->text(0) == selected) sources->setCurrentItem(item);
      }
    };
    append({}, nullptr);
    if (sourceFocus) sources->setFocus();
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
    auto *item = sources->currentItem(); if (!item) return;
    const auto name = item->text(0), scene = engine.currentScene(), type = item->data(0, Qt::UserRole).toString();
    if (type == "microphone") {
      std::unique_ptr<QDialog> dialog(feathercast::ui::createMicrophoneFiltersDialog(engine, name, this)); dialog->exec();
    } else if (type == "desktop" || type == "window" || type == "game" || type == "color" || type == "group") {
      feathercast::ui::editVideoFilters(engine, scene, name, this);
    }
    saveConfig();
  }
  void audioDialog() {
    std::unique_ptr<QDialog> dialog(feathercast::ui::createAdvancedAudioDialog(engine, this));
    dialog->exec();
    saveConfig();
  }
  void updateStats() {
    auto s = engine.statistics();
    auto elapsed = [](qint64 ms) { const qint64 seconds = ms / 1000; return QString("%1:%2:%3").arg(seconds / 3600, 2, 10, QChar('0')).arg((seconds / 60) % 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0')); };
    statusMetrics->setText(QString("Live %1   Rec %2   Replay %3   CPU %4%   %5 FPS")
      .arg(elapsed(s["streamDurationMs"].toInteger()), elapsed(s["recordingDurationMs"].toInteger()), elapsed(s["replayDurationMs"].toInteger()))
      .arg(s["cpuPercent"].toDouble(), 0, 'f', 1).arg(s["fps"].toDouble(), 0, 'f', 1));
    bool a = s["streaming"].toBool(), b = s["recording"].toBool(),
         c = s["replay"].toBool();
    stream->setText(a ? "Stop streaming" : "Start streaming");
    record->setText(b ? "Stop recording" : "Start recording");
    replay->setText(c ? "Stop replay" : "Start replay");
    replay->setEnabled(engine.settings()["replayEnabled"].toBool());
    save->setEnabled(c && engine.settings()["replayEnabled"].toBool());
    QString transport = !a ? "Stopped"
                        : s["relayConnected"].toBool()
                            ? "Connected"
                            : "Starting / reconnecting";
    stats->setText(
        QString("Stream: %1\nRecording: %2 · Replay: %3\nFPS: %4 · Render: %5 "
                "ms\nRender lag: %6 frames\nMemory: %7 MiB · GPU: "
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
    program = new Preview(&engine, true);
    auto *previewCard = new QWidget;
    previewCard->setObjectName("previewCard");
    auto *previewLayout = new QHBoxLayout(previewCard);
    previewLayout->setContentsMargins(6, 6, 6, 6);
    auto *left = new QWidget; auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(label("Preview", "section"));
    previewStack = new QStackedWidget;
    previewStack->addWidget(preview);
    auto *disabled = new QWidget; auto *disabledLayout = new QVBoxLayout(disabled);
    disabledLayout->addStretch(); auto *disabledText = new QLabel("Preview is currently disabled");
    disabledText->setAlignment(Qt::AlignCenter); disabledLayout->addWidget(disabledText);
    auto *enablePreview = new QPushButton("Enable Preview");
    disabledLayout->addWidget(enablePreview, 0, Qt::AlignCenter); disabledLayout->addStretch();
    connect(enablePreview, &QPushButton::clicked, this, [this] { engine.setPreviewEnabled(true); syncPreview(); });
    previewStack->addWidget(disabled); leftLayout->addWidget(previewStack, 1);
    previewLayout->addWidget(left, 1);
    programCard = new QWidget; auto *programLayout = new QVBoxLayout(programCard);
    programLayout->setContentsMargins(0, 0, 0, 0); programLayout->addWidget(label("Program", "section"));
    programLayout->addWidget(program, 1); previewLayout->addWidget(programCard, 1);
    setCentralWidget(previewCard);
    auto sceneBox = new QWidget;
    auto sl = new QVBoxLayout(sceneBox);
    scenes = new QListWidget;
    scenes->setAccessibleName("Scenes");
    scenes->setMinimumWidth(150);
    sl->addWidget(scenes);
    auto sr = new QHBoxLayout;
    sl->addLayout(sr);
    auto *addScene = button("+", sr, [this] {
      bool ok = false;
      auto n = QInputDialog::getText(this, "Add scene", "Name",
                                     QLineEdit::Normal, {}, &ok);
      if (ok)
        engine.addScene(n);
    });
    auto *removeScene = button("−", sr, [this] {
      if (scenes->currentItem())
        engine.removeScene(scenes->currentItem()->text());
    });
    button("✎", sr, [this] {
      if (!scenes->currentItem()) return;
      const auto old = scenes->currentItem()->text(); bool ok = false;
      const auto name = QInputDialog::getText(this, "Rename scene", "Name", QLineEdit::Normal, old, &ok);
      if (ok) engine.renameScene(old, name);
    });
    button("⧉", sr, [this] {
      if (!scenes->currentItem()) return;
      const auto old = scenes->currentItem()->text(); bool ok = false;
      const auto name = QInputDialog::getText(this, "Duplicate scene", "Name", QLineEdit::Normal, old + " copy", &ok);
      if (ok) engine.duplicateScene(old, name);
    });
    button("↑", sr, [this] { if (scenes->currentItem()) engine.moveScene(scenes->currentItem()->text(), qMax(0, scenes->currentRow() - 1)); });
    button("↓", sr, [this] { if (scenes->currentItem()) engine.moveScene(scenes->currentItem()->text(), qMin(scenes->count() - 1, scenes->currentRow() + 1)); });
    const QStringList sceneTooltips{"Add scene", "Remove scene", "Rename scene", "Duplicate scene", "Move scene up", "Move scene down"};
    sr->setSpacing(2); for (int i = 0; i < sr->count(); ++i) if (auto *w = qobject_cast<QPushButton *>(sr->itemAt(i)->widget())) { w->setFixedWidth(26); w->setStyleSheet("padding:2px; min-height:18px;"); w->setToolTip(sceneTooltips.value(i)); w->setAccessibleName(sceneTooltips.value(i)); }
    dock("Scenes", sceneBox, Qt::LeftDockWidgetArea);
    auto sourceBox = new QWidget;
    auto sol = new QVBoxLayout(sourceBox);
    sources = new QTreeWidget;
    sources->setHeaderLabels({"Source", "Type", "🔒"});
    sources->headerItem()->setToolTip(2, "Locked");
    sources->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    sources->header()->setSectionResizeMode(1, QHeaderView::Fixed); sources->setColumnWidth(1, 68);
    sources->header()->setSectionResizeMode(2, QHeaderView::Fixed); sources->setColumnWidth(2, 32);
    sources->setAccessibleName("Sources and visibility");
    sources->setMinimumWidth(150);
    sol->addWidget(sources);
    auto sor = new QHBoxLayout;
    sol->addLayout(sor);
    auto *addSourceButton = button("+", sor, [this] { addSource(); });
    auto *removeSource = button("−", sor, [this] {
      if (sources->currentItem())
        engine.removeSource(engine.currentScene(),
                            sources->currentItem()->text(0));
    });
    auto *sourceActions = new QHBoxLayout; sol->addLayout(sourceActions);
    button("+ Group", sourceActions, [this] {
      bool ok = false; const auto name = QInputDialog::getText(this, "Add group", "Name", QLineEdit::Normal, "Group", &ok);
      if (ok) engine.addGroup(engine.currentScene(), name);
    });
    button("✎", sourceActions, [this] {
      auto *item = sources->currentItem(); if (!item) return;
      const auto old = item->text(0), scene = engine.currentScene(); bool ok = false;
      const auto name = QInputDialog::getText(this, "Rename source", "Name", QLineEdit::Normal, old, &ok);
      if (ok) { pendingSourceSelection = name; if (!engine.renameSource(scene, old, name)) pendingSourceSelection.clear(); }
    });
    auto moveSelected = [this](int delta) {
      auto *item = sources->currentItem(); if (!item) return;
      const auto name = item->text(0), parent = item->parent() ? item->parent()->text(0) : QString();
      const auto rows = engine.sources(engine.currentScene());
      for (const auto &v : rows) { const auto row = v.toObject(); if (row["name"].toString() == name) {
        engine.moveSource(engine.currentScene(), name, parent, qMax(0, row["order"].toInt() + delta)); return;
      }}
    };
    button("↑", sourceActions, [moveSelected] { moveSelected(1); });
    button("↓", sourceActions, [moveSelected] { moveSelected(-1); });
    button("Move…", sourceActions, [this] {
      auto *item = sources->currentItem(); if (!item) return;
      const auto scene = engine.currentScene(), source = item->text(0);
      QStringList groups{"(root)"}; for (const auto &v : engine.sources(scene)) {
        const auto row = v.toObject(); if (row["type"].toString() == "group" && row["name"].toString() != source) groups << row["name"].toString();
      }
      bool ok = false; const auto group = QInputDialog::getItem(this, "Move source", "Group", groups, 0, false, &ok);
      if (ok) engine.moveSource(scene, source, group == "(root)" ? QString() : group, 1000);
    });
    auto *more = new QToolButton; more->setText("More…"); more->setPopupMode(QToolButton::InstantPopup); auto *sourceMenu = new QMenu(more);
    const QStringList actionNames{"Add group…", "Rename…", "Move up", "Move down", "Move to group…"};
    for (int i = 0; i < sourceActions->count(); ++i) if (auto *w = qobject_cast<QPushButton *>(sourceActions->itemAt(i)->widget())) { w->hide(); auto *action = sourceMenu->addAction(actionNames.value(i)); connect(action, &QAction::triggered, w, &QPushButton::click); connect(sourceMenu, &QMenu::aboutToShow, action, [action,w] { action->setEnabled(w->isEnabled()); }); }
    more->setMenu(sourceMenu); sor->addWidget(more);
    auto *editRow = new QHBoxLayout; sol->addLayout(editRow);
    auto *propertiesButton = button("Properties…", editRow, [this] { auto *item = sources->currentItem(); if (item) feathercast::ui::editSource(engine, engine.currentScene(), item->text(0), this); });
    auto* filters = button("Filters...", editRow, [this] { filtersDialog(); });
    filters->setToolTip("Edit filters for the selected source");
    filters->setEnabled(false);
    connect(sources, &QTreeWidget::currentItemChanged, this, [filters, propertiesButton, removeSource, sourceActions](QTreeWidgetItem* item) {
      bool locked = false; for (auto *node = item; node; node = node->parent()) locked |= node->checkState(2) == Qt::Checked;
      filters->setEnabled(item && !locked && QStringList{"microphone", "desktop", "window", "game", "color", "group"}.contains(item->data(0, Qt::UserRole).toString()));
      propertiesButton->setEnabled(item && !locked); removeSource->setEnabled(item && !locked);
      for (int i = 1; i < sourceActions->count(); ++i) if (auto *widget = sourceActions->itemAt(i)->widget()) widget->setEnabled(item && !locked);
      propertiesButton->setToolTip(locked ? "Unlock the source and its parent groups to edit it." : "Edit source properties and transform.");
    });
    propertiesButton->setEnabled(false); removeSource->setEnabled(false);
    sources->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(sources, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
      auto* item = sources->itemAt(pos);
      if (!item) return;
      sources->setCurrentItem(item);
      QMenu menu(this); menu.addAction("Filters...", this, [this] { filtersDialog(); });
      menu.exec(sources->viewport()->mapToGlobal(pos));
    });
    dock("Sources", sourceBox, Qt::LeftDockWidgetArea);
    connect(scenes, &QListWidget::currentTextChanged, this,
            [this](const QString &s) {
              if (!refreshing && !s.isEmpty()) {
                automationPanel->manualOverride();
                engine.selectScene(s);
                scheduleSceneRefresh();
              }
            });
    connect(sources, &QTreeWidget::itemChanged, this,
            [this](QTreeWidgetItem *i, int column) {
              if (refreshing) return;
              const auto scene = engine.currentScene(), name = i->text(0);
              if (column == 0) engine.setSourceVisible(scene, name, i->checkState(0) == Qt::Checked);
              if (column == 2) engine.setSourceLocked(scene, name, i->checkState(2) == Qt::Checked);
            });
    connect(&engine, &Engine::scenesChanged, this, [this] { scheduleSceneRefresh(); });
    auto controls = new QWidget;
    auto cl = new QVBoxLayout(controls);
    stream = button("Start streaming", cl, [this] {
      engine.statistics()["streaming"].toBool() ? engine.stopStreaming()
                                                : void(engine.startStreaming());
    });
    stream->setProperty("variant", "primary");
    record = button("Start recording", cl, [this] {
      engine.statistics()["recording"].toBool() ? engine.stopRecording()
                                                : void(engine.startRecording());
    });
    auto *replayRow = new QHBoxLayout; cl->addLayout(replayRow);
    replay = button("Replay", replayRow, [this] {
      engine.statistics()["replay"].toBool() ? engine.stopReplay()
                                             : void(engine.startReplay());
    });
    save = button("Save", replayRow, [this] { engine.saveReplay(); });
    studioButton = button("Studio Mode", cl, [this] { engine.setStudioMode(!engine.studioMode()); syncPreview(); });
    button("Take", cl, [this] { engine.takePreview(); });
    button("Settings", cl, [this] { settingsDialog(); });
    cl->addStretch();
    dock("Controls", controls, Qt::RightDockWidgetArea);
    auto *transitionBox = new QWidget; auto *transitionLayout = new QVBoxLayout(transitionBox);
    auto *transitionType = new QComboBox; transitionType->setObjectName("transitionType");
    for (const auto &v : engine.transitionTypes()) { auto o = v.toObject(); transitionType->addItem(o["name"].toString(), o["id"].toString()); }
    transitionType->setCurrentIndex(qMax(0, transitionType->findData(engine.settings()["transition"].toString())));
    auto *transitionDuration = new QSpinBox; transitionDuration->setObjectName("transitionDuration"); transitionDuration->setRange(50, 20000); transitionDuration->setSuffix(" ms");
    transitionDuration->setValue(engine.settings()["transitionDurationMs"].toInt(300));
    transitionLayout->addWidget(transitionType); transitionLayout->addWidget(transitionDuration);
    auto updateTransition = [this, transitionType, transitionDuration] { engine.setTransition(transitionType->currentData().toString(), transitionDuration->value()); };
    connect(transitionType, &QComboBox::currentIndexChanged, this, updateTransition);
    connect(transitionDuration, &QSpinBox::valueChanged, this, updateTransition);
    button("Take", transitionLayout, [this] { engine.takePreview(); });
    button("Cut", transitionLayout, [this] { engine.takePreview("cut"); });
    dock("Scene Transitions", transitionBox, Qt::BottomDockWidgetArea);
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
    mixer = new feathercast::ui::Mixer(engine, this);
    auto *audioDock = dock("Audio Mixer", mixer, Qt::BottomDockWidgetArea);
    connect(mixer->findChild<QPushButton *>("advancedAudio"), &QPushButton::clicked, this, [this] { audioDialog(); });
    audioDock->setObjectName("Audio meters"); // Preserve saved layouts from older versions.
    stats = new QLabel;
    stats->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    stats->setWordWrap(true);
    stats->setMargin(8);
    auto *statsDock = dock("Statistics", stats, Qt::BottomDockWidgetArea);
    automationPanel = new feathercast::ui::AutomationPanel(&engine, dir, this);
    auto *automationDock = dock("Automation", automationPanel, Qt::BottomDockWidgetArea);
    tabifyDockWidget(delayDock, statsDock);
    tabifyDockWidget(statsDock, automationDock);
    statsDock->raise();
    resizeDocks({findChildren<QDockWidget *>().first()}, {225}, Qt::Horizontal);
    resizeDocks({statsDock}, {190}, Qt::Vertical);
    statusMetrics = new QLabel; statusBar()->addPermanentWidget(statusMetrics);
    statusBar()->showMessage("Ready · Your workspace is saved automatically", 6000);
#ifdef FEATHERCAST_TWITCH
    feathercast::ui::installTwitchDocks(this, dir);
    if (auto *activity = findChild<QDockWidget *>("Twitch activity feed")) {
      if (auto *dismiss = activity->findChild<QPushButton *>("twitchActivityDismiss")) { dismiss->setText("Dismiss"); dismiss->setToolTip("Dismiss selected activity"); }
      if (auto *clear = activity->findChild<QPushButton *>("twitchActivityClear")) clear->setText("Clear");
      if (auto *filter = activity->findChild<QComboBox *>("twitchActivityFilter")) { filter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); filter->setMinimumContentsLength(8); }
      for (auto *area : activity->findChildren<QAbstractScrollArea *>()) area->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    }
    for (const char *name : {"Twitch channel", "Twitch chat", "Twitch activity feed", "Twitch stats"}) {
      if (auto *d = findChild<QDockWidget *>(name)) { auto *content = d->widget(); content->setParent(nullptr); auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setMinimumSize(0, 0); scroll->setWidget(content); d->setWidget(scroll); }
    }
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
    auto *filtersAction = edit->addAction("Source filters…", filters, &QPushButton::click);
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
            [this](bool on) { engine.setPreviewEnabled(on); syncPreview(); });
    connect(view, &QMenu::aboutToShow, this, [this, previewToggle] {
      const QSignalBlocker blocker(previewToggle);
      previewToggle->setChecked(engine.settings()["previewEnabled"].toBool());
    });
    view->addAction("Advanced Audio Properties…", this, [this] { audioDialog(); });
    view->addAction("Reset Workspace Layout", this, [this] { resetWorkspaceLayout(); });
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
    tools->addAction("Cancel remux", this, [this] { if (remuxProcess.state() != QProcess::NotRunning) remuxProcess.kill(); });
    auto *help = menuBar()->addMenu("&Help");
    help->addAction("About VELA", this, [this] {
      QMessageBox::about(this, "About VELA", "VELA " + QCoreApplication::applicationVersion() +
                         "\nVideo Encoding and Livestreaming Application");
    });
    refreshScenes();
    restoreGeometry(ui.value("geometry").toByteArray());
    if (ui.contains("state")) restoreState(ui.value("state").toByteArray());
    else resetWorkspaceLayout();
    connect(&timer, &QTimer::timeout, this, [this] {
      updateStats();
      if (++saveTicks % 100 == 0)
        saveConfig();
    });
    timer.start(300);
    connect(&mixerTimer, &QTimer::timeout, this, [this] { mixer->refresh(); });
    mixerTimer.start(100); mixer->refresh();
    updateStats();
    QTimer::singleShot(0, this, [this] {
      syncPreview();
    });
  }
  QJsonObject workspaceTest() {
    QJsonObject result;
    bool created = engine.addScene("Fixture scene") && engine.addGroup("Fixture scene", "Fixture group") &&
      engine.addSource("Fixture scene", "color", "Fixture color") && engine.addSource("Fixture scene", "test_tone", "Fixture tone");
    engine.moveSource("Fixture scene", "Fixture color", "Fixture group", 0);
    QCoreApplication::processEvents();
    auto matches = scenes->findItems("Fixture scene", Qt::MatchExactly); if (!matches.isEmpty()) scenes->setCurrentItem(matches.first());
    QCoreApplication::processEvents();
    auto groups = sources->findItems("Fixture group", Qt::MatchExactly | Qt::MatchRecursive, 0);
    auto colors = sources->findItems("Fixture color", Qt::MatchExactly | Qt::MatchRecursive, 0);
    result["hierarchy_and_selection"] = created && !groups.isEmpty() && !colors.isEmpty() && colors.first()->parent() == groups.first();
    if (!colors.isEmpty()) { colors.first()->setCheckState(0, Qt::Unchecked); }
    QCoreApplication::processEvents();
    bool hidden = false; for (auto v : engine.sources("Fixture scene")) if (v.toObject()["name"] == "Fixture color") hidden = !v.toObject()["visible"].toBool();
    colors = sources->findItems("Fixture color", Qt::MatchExactly | Qt::MatchRecursive, 0); if (!colors.isEmpty()) colors.first()->setCheckState(2, Qt::Checked);
    QCoreApplication::processEvents();
    bool locked = false; for (auto v : engine.sources("Fixture scene")) if (v.toObject()["name"] == "Fixture color") locked = v.toObject()["locked"].toBool();
    result["visibility_and_lock_controls"] = hidden && locked;
    engine.setSourceLocked("Fixture scene", "Fixture color", false);
    const auto oldProperties = engine.sourceProperties("Fixture scene", "Fixture color");
    bool propertyWorld = true, repeatedApply = true, editorRebased = false;
    QJsonArray coordinateSamples;
    QTimer::singleShot(50, this, [this, &propertyWorld, &repeatedApply, &editorRebased, &coordinateSamples] {
      auto *d = findChild<QDialog *>("sourcePropertiesDialog"); if (!d) { propertyWorld = false; return; }
      auto *width = d->findChild<QDoubleSpinBox *>("property_width"); auto *x = d->findChild<QDoubleSpinBox *>("transform_x"); auto *apply = d->findChild<QPushButton *>("applySource");
      if (!width || !x || !apply) { propertyWorld = false; d->reject(); return; }
      width->setValue(width->value() + 1); x->setValue(37);
      for (int click = 0; click < 4; ++click) {
        apply->click(); QEventLoop settle; QTimer::singleShot(150, &settle, &QEventLoop::quit); settle.exec();
        const auto world = engine.sourceWorldTransform("Fixture scene", "Fixture color");
        coordinateSamples.append(QJsonObject{{"apply", click + 1}, {"world", world}, {"widget_x", x->value()}});
        const bool placement = !world.isEmpty() && qAbs(world["dx"].toDouble() - 37) < .01;
        if (!click) propertyWorld = placement; else repeatedApply &= placement;
      }
      editorRebased = qAbs(x->value() - engine.sourceTransform("Fixture scene", "Fixture color")["x"].toDouble()) < .001;
      d->reject();
    });
    feathercast::ui::editSource(engine, "Fixture scene", "Fixture color", this);
    auto newProperties = engine.sourceProperties("Fixture scene", "Fixture color");
    result["properties_and_transform"] = newProperties["width"].toDouble() == oldProperties["width"].toDouble() + 1 && propertyWorld;
    result["repeated_apply_preserves_placement"] = repeatedApply;
    result["transform_editor_tracks_group_origin"] = editorRebased;
    result["coordinate_samples"] = coordinateSamples;
    bool filterEdited = false, filterRemoved = false;
    QTimer::singleShot(50, this, [this, &filterEdited, &filterRemoved] {
      auto *d = findChild<QDialog *>("videoFiltersDialog"); if (!d) return;
      auto *add = d->findChild<QPushButton *>("filterAdd"); auto *list = d->findChild<QListWidget *>();
      for (int n = 0; n < 2 && add; ++n) { QTimer::singleShot(0, d, [d,n] { auto *input = d->findChild<QInputDialog *>(); if (!input) return; if (auto *combo = input->findChild<QComboBox *>()) combo->setCurrentIndex(qMin(n, combo->count()-1)); input->accept(); }); add->click(); }
      if (list && list->count() > 1) { list->setCurrentRow(1); list->item(1)->setCheckState(Qt::Unchecked); if (auto *up = d->findChild<QPushButton *>("filterUp")) up->click(); if (auto *down = d->findChild<QPushButton *>("filterDown")) down->click(); }
      if (list && list->count() > 1) {
        list->setCurrentRow(0); auto before = engine.videoFilters("Fixture scene", "Fixture color")[0].toObject()["settings"].toObject();
        QTimer::singleShot(0, d, [d] { auto *edit = d->findChild<QDialog *>("videoFilterSettingsDialog"); if (!edit) return; for (auto *number : edit->findChildren<QDoubleSpinBox *>()) { double next = number->value() + number->singleStep(); if (next <= number->maximum()) { number->setValue(next); break; } } edit->accept(); });
        if (auto *edit = d->findChild<QPushButton *>("filterEdit")) edit->click();
        filterEdited = before != engine.videoFilters("Fixture scene", "Fixture color")[0].toObject()["settings"].toObject();
      }
      d->reject();
    });
    feathercast::ui::editVideoFilters(engine, "Fixture scene", "Fixture color", this);
    auto filters = engine.videoFilters("Fixture scene", "Fixture color");
    result["video_filter_add_bypass_reorder"] = filters.size() == 2 && filters[0].toObject()["id"] != filters[1].toObject()["id"] && !filters[1].toObject()["enabled"].toBool();
    result["video_filter_edit"] = filterEdited;
    QTimer::singleShot(50, this, [&filterRemoved, this] { auto *d = findChild<QDialog *>("videoFiltersDialog"); if (!d) return; auto *list = d->findChild<QListWidget *>(); if (list && list->count() > 0) { list->setCurrentRow(0); if (auto *remove = d->findChild<QPushButton *>("filterRemove")) remove->click(); } filterRemoved = engine.videoFilters("Fixture scene", "Fixture color").size() == 1; d->reject(); });
    feathercast::ui::editVideoFilters(engine, "Fixture scene", "Fixture color", this); result["video_filter_remove"] = filterRemoved;
    engine.setSourceLocked("Fixture scene", "Fixture color", true);
    mixer->refresh(); auto *strip = mixer->findChild<QWidget *>("mixer_Fixture tone");
    if (strip) { strip->findChild<QSlider *>("gain")->setValue(55); strip->findChild<QCheckBox *>("mute")->setChecked(true); auto *scope = strip->findChild<QComboBox *>("scope"); scope->setCurrentIndex(scope->findData("global")); }
    auto audio = engine.audioProperties("Fixture tone"); result["mixer_controls"] = strip && qAbs(audio["volume"].toDouble() - .55) < .001 && audio["muted"].toBool() && audio["scope"] == "global";
    auto *hide = strip ? strip->findChild<QPushButton *>("hide") : nullptr; if (hide) hide->click(); QCoreApplication::processEvents(); mixer->refresh();
    bool hiddenMixer = engine.audioProperties("Fixture tone")["mixer_hidden"].toBool();
    auto *showHidden = mixer->findChild<QPushButton *>("showHidden"); if (showHidden) showHidden->click();
    result["mixer_hide_show"] = hiddenMixer && !engine.audioProperties("Fixture tone")["mixer_hidden"].toBool();
    auto *duration = findChild<QSpinBox *>("transitionDuration"); auto *transition = findChild<QComboBox *>("transitionType");
    if (duration) duration->setValue(450); if (transition) transition->setCurrentIndex(transition->findData("fade"));
    result["transition_controls"] = duration && transition && engine.settings()["transitionDurationMs"].toInt() == 450 && engine.settings()["transition"] == "fade";
    auto previous = engine.programScene(); studioButton->click(); engine.selectScene("Scene"); const bool previewOnly = engine.programScene() == previous; engine.takePreview("cut"); result["studio_take"] = engine.studioMode() && previewOnly && engine.programScene() == "Scene";
    engine.setPreviewEnabled(false); syncPreview(); bool overlay = previewStack->currentIndex() == 1 && !previewAttached; engine.setPreviewEnabled(true); syncPreview(); result["preview_overlay"] = overlay && previewStack->currentIndex() == 0 && previewAttached;
    saveConfig(); QFile file(config); bool saved = file.open(QIODevice::ReadOnly); auto doc = QJsonDocument::fromJson(file.readAll()).object(); result["persistence"] = saved && doc["scenes"].isArray() && engine.settings()["studioMode"].toBool();
    return result;
  }
  QJsonObject workspaceReloadTest() {
    QJsonObject result; bool hasGroup = false, hidden = false, locked = false;
    for (auto v : engine.sources("Fixture scene")) { auto row = v.toObject(); if (row["name"] == "Fixture color") { hasGroup = row["parent"] == "Fixture group"; hidden = !row["visible"].toBool(); locked = row["locked"].toBool(); } }
    auto audio = engine.audioProperties("Fixture tone");
    result["saved_hierarchy"] = hasGroup && hidden && locked;
    result["saved_audio"] = qAbs(audio["volume"].toDouble() - .55) < .001 && audio["muted"].toBool() && audio["scope"] == "global";
    const auto world = engine.sourceWorldTransform("Fixture scene", "Fixture color"); result["saved_world_placement"] = !world.isEmpty() && qAbs(world["dx"].toDouble() - 37) < .01; result["world_transform"] = world;
    result["saved_studio_transition"] = engine.studioMode() && engine.settings()["transition"] == "fade" && engine.settings()["transitionDurationMs"].toInt() == 450;
    return result;
  }
  QJsonObject selfTest() {
    return automationPanel ? automationPanel->selfTest()
                           : QJsonObject{{"initialized", false}};
  }
  void closeEvent(QCloseEvent *e) override {
    timer.stop();
    mixerTimer.stop();
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
      engine.attachProgram(0, 0, 0);
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
  perf->addRow(new QLabel("CPU utilization is measured; GPU utilization is unavailable. "
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
    syncPreview();
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
  QCommandLineOption reloadTest("workspace-ui-reload-test", "Check workspace fixture after restart", "absoluteJSON");
  QCommandLineOption workspaceTest("workspace-ui-test", "Run native workspace controller fixture", "absoluteJSON");
  QCommandLineOption size("window-size", "Window size WIDTHxHEIGHT", "size");
  QCommandLineOption selftest("ui-self-test",
                              "Run integrated UI checks and save results",
                              "absoluteJSON");
  parser.addOptions({smoke, quit, config, screenshot, selftest, workspaceTest, reloadTest, size});
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
  Window window(dir, parser.isSet(smoke) || parser.isSet(selftest) || parser.isSet(workspaceTest) || parser.isSet(reloadTest));
  window.setProperty("workspaceFixture", parser.isSet(workspaceTest));
  if (parser.isSet(size)) { const auto parts = parser.value(size).split('x'); if (parts.size() == 2) window.resize(parts[0].toInt(), parts[1].toInt()); }
  if (parser.isSet(workspaceTest) || parser.isSet(reloadTest)) window.setAttribute(Qt::WA_DontShowOnScreen);
  window.show();
  if (parser.isSet(reloadTest)) QTimer::singleShot(1000, &window, [&] { auto results = window.workspaceReloadTest(); for (auto value : results) if (value.isBool() && !value.toBool()) testExit = 1; QSaveFile f(parser.value(reloadTest)); if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(results).toJson()); if (!f.commit()) testExit = 2; } else testExit = 2; });
  if (parser.isSet(workspaceTest)) QTimer::singleShot(1000, &window, [&] {
    auto results = window.workspaceTest(); for (auto value : results) if (value.isBool() && !value.toBool()) testExit = 1;
    QSaveFile f(parser.value(workspaceTest)); if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(results).toJson()); if (!f.commit()) testExit = 2; } else testExit = 2;
  });
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
