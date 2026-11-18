#include "feathercast/ui/automation_panel.hpp"
#include <QSaveFile>
#include <cmath>
#include <windows.h>
namespace feathercast::ui {
using namespace core;
namespace {
const QStringList conditions = {"Always",
                                "Process running (basename lowercase)",
                                "Foreground process (basename lowercase)",
                                "Window title contains",
                                "Streaming",
                                "Recording",
                                "Replay running",
                                "Audio peak above dB (loudest source)",
                                "Elapsed at least ms",
                                "Hotkey action ID",
                                "External flag (integration input)"};
const QStringList actionNames = {"Switch scene",
                                 "Start streaming",
                                 "Stop streaming",
                                 "Start recording",
                                 "Stop recording",
                                 "Start replay",
                                 "Stop replay",
                                 "Save replay",
                                 "Set delay ms",
                                 "Plugin action ID",
                                 "Log"};
QPushButton *button(QBoxLayout *l, const QString &text, QObject *owner,
                    std::function<void()> fn) {
  auto b = new QPushButton(text);
  l->addWidget(b);
  QObject::connect(b, &QPushButton::clicked, owner, fn);
  return b;
}
QComboBox *combo(const QStringList &names, int index) {
  auto c = new QComboBox;
  c->addItems(names);
  c->setCurrentIndex(index);
  return c;
}
} // namespace
AutomationPanel::AutomationPanel(engine::Engine *e, QString directory,
                                 QWidget *parent)
    : QWidget(parent), engine(e), path(directory + "/automation.json") {
  auto l = new QVBoxLayout(this);
  enabled = new QCheckBox("Enable automation");
  l->addWidget(enabled);
  fallback = new QComboBox;
  fallback->addItem("No fallback", "");
  for (auto s : e->scenes())
    fallback->addItem(s, s);
  auto f = new QFormLayout;
  l->addLayout(f);
  f->addRow("Fallback scene", fallback);
  rulesList = new QListWidget;
  l->addWidget(rulesList);
  auto row = new QHBoxLayout;
  l->addLayout(row);
  button(row, "Add", this, [this] {
    Rule r;
    r.id = "Rule " + std::to_string(rules.size() + 1);
    r.any_of = {{{ConditionType::Always}}};
    r.actions = {
        {ActionType::SwitchScene, engine->currentScene().toStdString()}};
    rules.push_back(r);
    if (!editRule(int(rules.size() - 1))) {
      rules.pop_back();
      refresh();
    }
  });
  button(row, "Edit", this, [this] { editRule(rulesList->currentRow()); });
  button(row, "Remove", this, [this] {
    int i = rulesList->currentRow();
    if (i >= 0) {
      rules.erase(rules.begin() + i);
      refresh();
      save();
    }
  });
  auto io = new QHBoxLayout;
  l->addLayout(io);
  button(io, "Import…", this, [this] {
    auto p = QFileDialog::getOpenFileName(this, "Import automation", {},
                                          "JSON (*.json)");
    if (p.isEmpty())
      return;
    QFile file(p);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) {
      appendLog("Import failed: unreadable or oversized document");
      return;
    }
    QJsonParseError pe;
    auto doc = QJsonDocument::fromJson(file.readAll(), &pe);
    bool on;
    QString scene, error;
    std::vector<Rule> next;
    if (pe.error != QJsonParseError::NoError ||
        !decodeRules(doc.object(), next, on, scene, error)) {
      appendLog("Import rejected: " + error);
      return;
    }
    rules = std::move(next);
    enabled->setChecked(on);
    if (fallback->findData(scene) < 0)
      fallback->addItem(scene, scene);
    fallback->setCurrentIndex(qMax(0, fallback->findData(scene)));
    refresh();
    save();
  });
  button(io, "Export…", this, [this] {
    auto p = QFileDialog::getSaveFileName(this, "Export automation", {},
                                          "JSON (*.json)");
    if (p.isEmpty())
      return;
    QSaveFile file(p);
    if (file.open(QIODevice::WriteOnly)) {
      file.write(QJsonDocument(encodeRules(rules, enabled->isChecked(),
                                           fallback->currentData().toString()))
                     .toJson());
      if (!file.commit())
        appendLog("Export failed");
    }
  });
  button(io, "Test selected", this, [this] {
    int i = rulesList->currentRow();
    if (i >= 0)
      appendLog(QString::fromStdString(rules[i].id) +
                (AutomationEngine::matches(rules[i], snapshot())
                     ? ": matches current snapshot"
                     : ": does not match"));
  });
  button(l, "Pause automation for 30 seconds", this,
         [this] { manualOverride(); });
  log = new QListWidget;
  log->setMaximumHeight(100);
  l->addWidget(log);
  QFile file(path);
  if (file.open(QIODevice::ReadOnly)) {
    bool on;
    QString scene, error;
    if (decodeRules(QJsonDocument::fromJson(file.readAll()).object(), rules, on,
                    scene, error)) {
      enabled->setChecked(on);
      if (fallback->findData(scene) < 0)
        fallback->addItem(scene, scene);
      fallback->setCurrentIndex(qMax(0, fallback->findData(scene)));
    } else
      appendLog("Saved automation rejected: " + error);
  }
  if (rules.empty()) {
    for (auto pair : {std::pair{"VALORANT", 100}, std::pair{"CS2", 90},
                      std::pair{"Discord", 40}, std::pair{"Desktop", 0}}) {
      Rule r;
      r.id = pair.first;
      r.priority = pair.second;
      r.enabled = false;
      r.any_of = {{{ConditionType::ProcessRunning,
                    QString(pair.first).toLower().toStdString() + ".exe"}}};
      r.actions = {{ActionType::SwitchScene, pair.first}};
      if (r.id == "VALORANT")
        r.any_of = {
            {{ConditionType::ProcessRunning, "valorant-win64-shipping.exe"}}};
      if (r.id == "Desktop")
        r.any_of = {{{ConditionType::Always}}};
      rules.push_back(r);
    }
  }
  QSettings settings(directory + "/hotkeys.ini", QSettings::IniFormat);
  for (auto id :
       {"toggle_stream", "toggle_record", "toggle_replay", "save_replay",
        "delay_0", "delay_30", "delay_90", "manual_scene"})
    bindings.append({id, QKeySequence(settings.value(id).toString()), false});
  qApp->installNativeEventFilter(this);
  registerBindings();
  refresh();
  clock.start();
  connect(&timer, &QTimer::timeout, this, [this] { tick(); });
  timer.start(500);
  connect(enabled, &QCheckBox::toggled, this, [this] {
    evaluator.reset_runtime();
    save();
  });
  connect(fallback, &QComboBox::currentIndexChanged, this, [this] { save(); });
  connect(engine, &engine::Engine::scenesChanged, this, [this] {
    auto current = fallback->currentData().toString();
    QSignalBlocker blocker(fallback);
    fallback->clear();
    fallback->addItem("No fallback", "");
    for (auto s : engine->scenes())
      fallback->addItem(s, s);
    fallback->setCurrentIndex(qMax(0, fallback->findData(current)));
  });
}
AutomationPanel::~AutomationPanel() {
  timer.stop();
  qApp->removeNativeEventFilter(this);
  for (int i = 0; i < bindings.size(); ++i)
    if (bindings[i].registered)
      UnregisterHotKey(nullptr, 0x6000 + i);
}
void AutomationPanel::appendLog(const QString &text) {
  log->addItem(QTime::currentTime().toString() + " " + text);
  while (log->count() > 100)
    delete log->takeItem(0);
  log->scrollToBottom();
}
void AutomationPanel::refresh() {
  rulesList->clear();
  for (const auto &r : rules)
    rulesList->addItem(
        QString("%1  %2  [priority %3]")
            .arg(r.enabled ? "✓" : "○", QString::fromStdString(r.id))
            .arg(r.priority));
  std::string error;
  evaluator.set_rules(rules, &error);
  evaluator.set_fallback_scene(
      fallback->currentData().toString().toStdString());
}
void AutomationPanel::save() {
  evaluator.set_fallback_scene(
      fallback->currentData().toString().toStdString());
  QSaveFile file(path);
  if (file.open(QIODevice::WriteOnly)) {
    file.write(QJsonDocument(encodeRules(rules, enabled->isChecked(),
                                         fallback->currentData().toString()))
                   .toJson());
    if (!file.commit())
      appendLog("Configuration save failed");
  }
}
void AutomationPanel::manualOverride() {
  evaluator.set_manual_override_until(clock.elapsed() + 30000);
  appendLog("Manual override: automation paused for 30 seconds");
}
AutomationSnapshot AutomationPanel::snapshot() {
  auto s = engine->automationSnapshot();
  s.elapsed_ms = clock.elapsed();
  for (auto it = pulses.begin(); it != pulses.end();) {
    if (it.value() < clock.elapsed())
      it = pulses.erase(it);
    else {
      s.hotkeys.insert(it.key().toStdString());
      ++it;
    }
  }
  return s;
}
void AutomationPanel::tick() {
  if (!enabled->isChecked())
    return;
  auto decision = evaluator.evaluate(snapshot(), clock.elapsed());
  for (const auto &a : decision.actions) {
    bool ok = dispatch(a);
    appendLog(QString::fromStdString(decision.rule_id) +
              (ok ? ": action dispatched" : ": action failed"));
  }
}
bool AutomationPanel::dispatch(const Action &a) {
  auto arg = QString::fromStdString(a.argument);
  switch (a.type) {
  case ActionType::SwitchScene:
    return engine->selectScene(arg);
  case ActionType::StartStreaming:
    return engine->startStreaming();
  case ActionType::StopStreaming:
    engine->stopStreaming();
    return true;
  case ActionType::StartRecording:
    return engine->startRecording();
  case ActionType::StopRecording:
    engine->stopRecording();
    return true;
  case ActionType::StartReplay:
    return engine->startReplay();
  case ActionType::StopReplay:
    engine->stopReplay();
    return true;
  case ActionType::SaveReplay:
    return engine->saveReplay();
  case ActionType::SetDelayMs:
    return engine->setDelayMs(a.value);
  case ActionType::PluginAction:
    return engine->invokePluginAction(arg);
  case ActionType::Log:
    appendLog(arg.contains("://") ? "[URL suppressed]" : arg);
    return true;
  }
  return false;
}
bool AutomationPanel::editRule(int index) {
  if (index < 0 || index >= int(rules.size()))
    return false;
  auto original = rules[index];
  QDialog dialog(this);
  dialog.setWindowTitle("Automation rule — OR groups / AND conditions");
  dialog.resize(900, 720);
  auto layout = new QVBoxLayout(&dialog);
  auto form = new QFormLayout;
  layout->addLayout(form);
  auto id = new QLineEdit(QString::fromStdString(original.id));
  form->addRow("Unique ID", id);
  auto priority = new QSpinBox;
  priority->setRange(-100000, 100000);
  priority->setValue(original.priority);
  form->addRow("Priority (higher wins; ID breaks ties)", priority);
  auto on = new QCheckBox("Enabled");
  on->setChecked(original.enabled);
  form->addRow(on);
  auto debounce = new QSpinBox;
  debounce->setRange(0, 86400000);
  debounce->setValue(original.debounce_ms);
  form->addRow("Debounce (ms)", debounce);
  auto cooldown = new QSpinBox;
  cooldown->setRange(0, 86400000);
  cooldown->setValue(original.cooldown_ms);
  form->addRow("Cooldown (ms)", cooldown);
  auto repeat =
      new QCheckBox("Repeat while winning (requires positive cooldown)");
  repeat->setChecked(original.repeat_while_matching);
  form->addRow(repeat);
  auto groups = new QTabWidget;
  layout->addWidget(groups);
  auto addGroup = [&](const ConditionGroup &group) {
    auto w = new QWidget;
    auto l = new QVBoxLayout(w);
    auto table = new QTableWidget(0, 4);
    table->setObjectName("conditions");
    table->setHorizontalHeaderLabels(
        {"Type", "Text / process / hotkey ID", "Number", "Negate"});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    l->addWidget(table);
    auto add = [=](Condition c) {
      int row = table->rowCount();
      table->insertRow(row);
      table->setCellWidget(row, 0, combo(conditions, int(c.type)));
      table->setCellWidget(row, 1,
                           new QLineEdit(QString::fromStdString(c.text)));
      auto n = new QDoubleSpinBox;
      n->setRange(-100000000, 100000000);
      n->setValue(c.number);
      table->setCellWidget(row, 2, n);
      auto neg = new QCheckBox;
      neg->setChecked(c.negate);
      table->setCellWidget(row, 3, neg);
    };
    for (auto c : group)
      add(c);
    auto row = new QHBoxLayout;
    l->addLayout(row);
    button(row, "Add AND condition", &dialog, [=] { add({}); });
    button(row, "Remove condition", &dialog,
           [=] { table->removeRow(table->currentRow()); });
    groups->addTab(w, QString("OR group %1").arg(groups->count() + 1));
  };
  for (auto g : original.any_of)
    addGroup(g);
  auto gr = new QHBoxLayout;
  layout->addLayout(gr);
  button(gr, "Add OR group", &dialog, [&] { addGroup({}); });
  button(gr, "Remove group", &dialog, [&] {
    int i = groups->currentIndex();
    if (i >= 0)
      delete groups->widget(i);
  });
  auto table = new QTableWidget(0, 3);
  table->setHorizontalHeaderLabels(
      {"Action", "Scene / plugin ID / log text", "Value (delay ms)"});
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  layout->addWidget(table);
  auto addAction = [=](Action a) {
    int r = table->rowCount();
    table->insertRow(r);
    table->setCellWidget(r, 0, combo(actionNames, int(a.type)));
    table->setCellWidget(r, 1,
                         new QLineEdit(QString::fromStdString(a.argument)));
    auto n = new QSpinBox;
    n->setRange(0, 300000);
    n->setValue(a.value);
    table->setCellWidget(r, 2, n);
  };
  for (auto a : original.actions)
    addAction(a);
  auto ar = new QHBoxLayout;
  layout->addLayout(ar);
  button(ar, "Add action", &dialog, [=] { addAction({}); });
  button(ar, "Remove action", &dialog,
         [=] { table->removeRow(table->currentRow()); });
  button(ar, "Move action up", &dialog, [=] {
    int r = table->currentRow();
    if (r < 1)
      return;
    for (int c = 0; c < 3; ++c) {
      if (c == 0) {
        auto a = qobject_cast<QComboBox *>(table->cellWidget(r, c));
        auto b = qobject_cast<QComboBox *>(table->cellWidget(r - 1, c));
        int v = a->currentIndex();
        a->setCurrentIndex(b->currentIndex());
        b->setCurrentIndex(v);
      } else if (c == 1) {
        auto a = qobject_cast<QLineEdit *>(table->cellWidget(r, c));
        auto b = qobject_cast<QLineEdit *>(table->cellWidget(r - 1, c));
        auto v = a->text();
        a->setText(b->text());
        b->setText(v);
      } else {
        auto a = qobject_cast<QSpinBox *>(table->cellWidget(r, c));
        auto b = qobject_cast<QSpinBox *>(table->cellWidget(r - 1, c));
        int v = a->value();
        a->setValue(b->value());
        b->setValue(v);
      }
    }
    table->setCurrentCell(r - 1, 0);
  });
  auto box =
      new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  layout->addWidget(box);
  connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(box, &QDialogButtonBox::accepted, &dialog, [&] {
    Rule r;
    r.id = id->text().trimmed().toStdString();
    r.priority = priority->value();
    r.enabled = on->isChecked();
    r.debounce_ms = debounce->value();
    r.cooldown_ms = cooldown->value();
    r.repeat_while_matching = repeat->isChecked();
    for (int g = 0; g < groups->count(); ++g) {
      ConditionGroup cs;
      auto t = groups->widget(g)->findChild<QTableWidget *>("conditions");
      for (int row = 0; row < t->rowCount(); ++row)
        cs.push_back(
            {ConditionType(qobject_cast<QComboBox *>(t->cellWidget(row, 0))
                               ->currentIndex()),
             qobject_cast<QLineEdit *>(t->cellWidget(row, 1))
                 ->text()
                 .toStdString(),
             qobject_cast<QDoubleSpinBox *>(t->cellWidget(row, 2))->value(),
             qobject_cast<QCheckBox *>(t->cellWidget(row, 3))->isChecked()});
      r.any_of.push_back(cs);
    }
    for (int row = 0; row < table->rowCount(); ++row)
      r.actions.push_back(
          {ActionType(qobject_cast<QComboBox *>(table->cellWidget(row, 0))
                          ->currentIndex()),
           qobject_cast<QLineEdit *>(table->cellWidget(row, 1))
               ->text()
               .toStdString(),
           qobject_cast<QSpinBox *>(table->cellWidget(row, 2))->value()});
    auto next = rules;
    next[index] = r;
    bool onValue;
    QString scene, error;
    std::vector<Rule> validated;
    if (!decodeRules(encodeRules(next, true, {}), validated, onValue, scene,
                     error)) {
      QMessageBox::warning(&dialog, "Invalid rule", error);
      return;
    }
    rules = std::move(next);
    refresh();
    save();
    dialog.accept();
  });
  bool accepted = dialog.exec() == QDialog::Accepted;
  refresh();
  return accepted;
}
void AutomationPanel::registerBindings() {
  for (int i = 0; i < bindings.size(); ++i) {
    auto &b = bindings[i];
    if (b.registered)
      UnregisterHotKey(nullptr, 0x6000 + i);
    b.registered = false;
    if (b.sequence.isEmpty())
      continue;
    auto key = b.sequence[0];
    auto mods = key.keyboardModifiers();
    UINT flags = MOD_NOREPEAT;
    if (mods & Qt::ControlModifier)
      flags |= MOD_CONTROL;
    if (mods & Qt::AltModifier)
      flags |= MOD_ALT;
    if (mods & Qt::ShiftModifier)
      flags |= MOD_SHIFT;
    if (mods & Qt::MetaModifier)
      flags |= MOD_WIN;
    int k = int(key.key());
    UINT vk = 0;
    if (k >= Qt::Key_F1 && k <= Qt::Key_F24)
      vk = VK_F1 + k - Qt::Key_F1;
    else if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9'))
      vk = k;
    else if (k == Qt::Key_Space)
      vk = VK_SPACE;
    if (vk)
      b.registered = RegisterHotKey(nullptr, 0x6000 + i, flags, vk);
    if (!b.registered)
      appendLog("Hotkey conflict or unsupported key: " + b.action);
  }
}
void AutomationPanel::editHotkeys(QWidget *parent) {
  QDialog d(parent);
  d.setWindowTitle("Global hotkeys");
  auto f = new QFormLayout(&d);
  f->addRow(new QLabel(
      "Use Ctrl/Alt/Shift/Win with letters, digits, Space or F1–F24.\nIDs are "
      "also available to automation hotkey conditions (2-second pulse)."));
  QList<QKeySequenceEdit *> edits;
  for (const auto &b : bindings) {
    auto edit = new QKeySequenceEdit(b.sequence);
    edit->setMaximumSequenceLength(1);
    edits << edit;
    f->addRow(b.action, edit);
  }
  auto box =
      new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
  f->addRow(box);
  connect(box, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  connect(box, &QDialogButtonBox::accepted, &d, [&] {
    QSettings settings(QFileInfo(path).absolutePath() + "/hotkeys.ini",
                       QSettings::IniFormat);
    for (int i = 0; i < bindings.size(); ++i) {
      bindings[i].sequence = edits[i]->keySequence();
      settings.setValue(bindings[i].action, bindings[i].sequence.toString());
    }
    registerBindings();
    QStringList failures;
    for (const auto &b : bindings)
      if (!b.sequence.isEmpty() && !b.registered)
        failures << b.action;
    if (!failures.isEmpty())
      QMessageBox::warning(&d, "Hotkeys not registered", failures.join("\n"));
    else
      d.accept();
  });
  d.exec();
}
bool AutomationPanel::nativeEventFilter(const QByteArray &, void *message,
                                        qintptr *result) {
  auto msg = static_cast<MSG *>(message);
  if (msg->message != WM_HOTKEY)
    return false;
  int i = int(msg->wParam) - 0x6000;
  if (i < 0 || i >= bindings.size())
    return false;
  auto id = bindings[i].action;
  pulses[id] = clock.elapsed() + 2000;
  auto s = engine->statistics();
  if (id == "toggle_stream")
    s["streaming"].toBool() ? engine->stopStreaming()
                            : void(engine->startStreaming());
  else if (id == "toggle_record")
    s["recording"].toBool() ? engine->stopRecording()
                            : void(engine->startRecording());
  else if (id == "toggle_replay")
    s["replay"].toBool() ? engine->stopReplay() : void(engine->startReplay());
  else if (id == "save_replay")
    engine->saveReplay();
  else if (id.startsWith("delay_"))
    engine->setDelayMs(id.mid(6).toInt() * 1000);
  else if (id == "manual_scene") {
    manualOverride();
    bool ok;
    auto scene = QInputDialog::getItem(this, "Manual scene", "Scene",
                                       engine->scenes(), 0, false, &ok);
    if (ok)
      engine->selectScene(scene);
  }
  if (result)
    *result = 0;
  return true;
}
QJsonObject AutomationPanel::selfTest() {
  auto before = encodeRules(rules, enabled->isChecked(),
                            fallback->currentData().toString());
  std::vector<Rule> copy;
  bool on;
  QString scene, error;
  bool roundtrip = decodeRules(before, copy, on, scene, error) &&
                   encodeRules(copy, on, scene) == before;
  auto invalid = before;
  invalid["version"] = 99;
  bool rejected = !decodeRules(invalid, copy, on, scene, error) &&
                  encodeRules(copy, on, scene) == before;
  auto current = engine->currentScene();
  auto oldDelay = engine->settings()["delayMs"].toInt();
  bool sceneAction = dispatch({ActionType::SwitchScene, current.toStdString()});
  bool delayAction = dispatch({ActionType::SetDelayMs, {}, 15000}) &&
                     engine->settings()["delayMs"].toInt() == 15000;
  Rule probe;
  probe.id = "self-test";
  probe.debounce_ms = 0;
  probe.any_of = {{{ConditionType::Hotkey, "self_test_pulse"}}};
  probe.actions = {{ActionType::SetDelayMs, {}, 30000},
                   {ActionType::Log, "Self-test ordered action"}};
  auto priorRules = rules;
  bool priorEnabled = enabled->isChecked();
  rules = {probe};
  refresh();
  enabled->setChecked(true);
  pulses["self_test_pulse"] = clock.elapsed() + 2000;
  tick();
  bool controller = engine->settings()["delayMs"].toInt() == 30000;
  engine->setDelayMs(45000);
  tick();
  bool noRepeat = engine->settings()["delayMs"].toInt() == 45000;
  manualOverride();
  engine->setDelayMs(oldDelay);
  tick();
  bool overrideHeld = engine->settings()["delayMs"].toInt() == oldDelay;
  evaluator.clear_manual_override();
  rules = priorRules;
  refresh();
  enabled->setChecked(false);
  pulses.clear();
  bool registered =
      RegisterHotKey(nullptr, 0x7000,
                     MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F24);
  bool conflict =
      registered &&
      !RegisterHotKey(nullptr, 0x7001,
                      MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F24);
  if (registered)
    UnregisterHotKey(nullptr, 0x7000);
  UnregisterHotKey(nullptr, 0x7001);
  // Exercise Qt's native WM_HOTKEY event path without injecting keyboard input
  // into other applications. Registration/conflicts above use the real OS API.
  bindings.append({"delay_30", {}, false});
  const auto nativeId = 0x6000 + bindings.size() - 1;
  PostThreadMessageW(GetCurrentThreadId(), WM_HOTKEY, nativeId, 0);
  QEventLoop nativeLoop;
  QTimer::singleShot(100, &nativeLoop, &QEventLoop::quit);
  nativeLoop.exec();
  bool nativeDispatch = engine->settings()["delayMs"].toInt() == 30000 &&
                        snapshot().hotkeys.contains("delay_30");
  bindings.removeLast();
  pulses.clear();
  engine->setDelayMs(oldDelay);
  QString toneName = "FeatherCast UI meter test";
  bool tone = engine->addSource(current, "test_tone", toneName);
  QEventLoop loop;
  QTimer::singleShot(700, &loop, &QEventLoop::quit);
  loop.exec();
  double peak =
      engine->statistics()["audioPeaksDb"].toObject()[toneName].toDouble(-100);
  bool meter = tone && peak > -60 && peak < 0 &&
               engine->automationSnapshot().audio_peak_db > -60;
  engine->removeSource(current, toneName);
  QTimer::singleShot(200, this, [this] {
    for (auto *widget : QApplication::topLevelWidgets())
      if (auto *dialog = qobject_cast<QDialog *>(widget);
          dialog && dialog->windowTitle().startsWith("Automation rule")) {
        dialog->grab().save(QFileInfo(path).absolutePath() +
                            "/macro-editor.png");
        dialog->reject();
      }
  });
  if (!rules.empty())
    editRule(0);
  // Invalid imported scene graphs must not destroy the live scene graph.
  QTemporaryFile bad(QFileInfo(path).absolutePath() + "/invalid-XXXXXX.json");
  bad.open();
  QJsonArray tooMany;
  for (int i = 0; i < 129; ++i)
    tooMany.append(
        QJsonObject{{"name", QString::number(i)}, {"sources", QJsonArray{}}});
  bad.write(QJsonDocument(QJsonObject{{"version", 1},
                                      {"settings", engine->settings()},
                                      {"scenes", tooMany},
                                      {"selectedScene", "0"}})
                .toJson());
  bad.flush();
  auto scenesBefore = engine->scenes();
  bool enginePreserved = !engine->loadConfiguration(bad.fileName()) &&
                         engine->scenes() == scenesBefore &&
                         engine->currentScene() == current;
  enabled->setChecked(priorEnabled);
  save();
  return {{"nativeHotkeyEventDispatch", nativeDispatch},
          {"roundtrip", roundtrip},
          {"invalidPreservesRules", rejected},
          {"sceneAction", sceneAction},
          {"delayAction", delayAction},
          {"controllerOrderedActions", controller},
          {"controllerDoesNotRepeat", noRepeat},
          {"manualOverride", overrideHeld},
          {"globalHotkeyRegistered", registered},
          {"globalHotkeyConflictDetected", conflict},
          {"realAudioMeter", meter},
          {"testTonePeakDb", peak},
          {"invalidEngineConfigPreservesScenes", enginePreserved}};
}
} // namespace feathercast::ui
