#include "feathercast/ui/mixer.hpp"
#include "feathercast/engine/engine.hpp"
#include <QtWidgets>
#include <cmath>
namespace feathercast::ui {
Mixer::Mixer(engine::Engine &e, QWidget *parent) : QWidget(parent), engine(e) {
  auto *outer = new QVBoxLayout(this); outer->setContentsMargins(3,3,3,3);
  auto *scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  strips = new QWidget; auto *layout = new QHBoxLayout(strips); layout->setAlignment(Qt::AlignLeft); layout->setContentsMargins(0,0,0,0);
  scroll->setWidget(strips); outer->addWidget(scroll, 1);
  auto *options = new QHBoxLayout; outer->addLayout(options);
  auto *active = new QCheckBox("Active only"); options->addWidget(active);
  connect(active, &QCheckBox::toggled, this, [this](bool on) { activeOnly = on; refresh(); });
  auto *advanced = new QPushButton("Advanced…"); advanced->setObjectName("advancedAudio"); options->addWidget(advanced);
  auto *show = new QPushButton("Show hidden sources"); show->setObjectName("showHidden"); options->addWidget(show);
  connect(show, &QPushButton::clicked, this, [this] { for (const auto &v : engine.audioSources()) { auto row = v.toObject(); if (row["properties"].toObject()["mixer_hidden"].toBool()) engine.setAudioProperties(row["name"].toString(), {{"mixer_hidden", false}}); } refresh(); });
}
void Mixer::refresh() {
  auto *layout = qobject_cast<QHBoxLayout *>(strips->layout()); QSet<QString> seen;
  const auto peaks = engine.statistics()["audioPeaksDb"].toObject();
  for (const auto &value : engine.audioSources()) {
    const auto row = value.toObject(), props = row["properties"].toObject(); const auto name = row["name"].toString();
    const bool show = !props["mixer_hidden"].toBool() && (!activeOnly || row["active"].toBool());
    seen.insert(name); QWidget *strip = rows.value(name);
    if (!strip) {
      strip = new QWidget; strip->setObjectName("mixer_" + name); strip->setMinimumWidth(110); strip->setMaximumWidth(150); auto *v = new QVBoxLayout(strip); v->setContentsMargins(3,3,3,3);
      auto *title = new QLabel(name); title->setWordWrap(true); v->addWidget(title);
      auto *scope = new QComboBox; scope->setObjectName("scope"); scope->addItem("Scene", "scene"); scope->addItem("Global", "global"); v->addWidget(scope);
      auto *level = new QProgressBar; level->setObjectName("level"); level->setRange(-60, 0); level->setFormat("%v dBFS"); v->addWidget(level);
      auto *gain = new QSlider(Qt::Horizontal); gain->setObjectName("gain"); gain->setRange(0,200); gain->setAccessibleName(name + " volume"); v->addWidget(gain);
      auto *db = new QLabel; db->setObjectName("db"); v->addWidget(db);
      auto *mute = new QCheckBox("Mute"); mute->setObjectName("mute"); v->addWidget(mute);
      auto *monitor = new QComboBox; monitor->setObjectName("monitor"); monitor->addItem("Monitor off", "off"); monitor->addItem("Monitor only", "monitor_only"); monitor->addItem("Monitor + output", "monitor_and_output"); v->addWidget(monitor);
      auto *hide = new QPushButton("Hide"); hide->setObjectName("hide"); v->addWidget(hide);
      connect(gain, &QSlider::valueChanged, this, [this,name](int n) { engine.setAudioProperties(name, {{"volume", n / 100.0}}); });
      connect(mute, &QCheckBox::toggled, this, [this,name](bool on) { engine.setAudioProperties(name, {{"muted", on}}); });
      connect(scope, &QComboBox::currentIndexChanged, this, [this,name,scope] { if (!engine.setAudioProperties(name, {{"scope", scope->currentData().toString()}})) refresh(); });
      connect(monitor, &QComboBox::currentIndexChanged, this, [this,name,monitor] { if (!engine.setAudioProperties(name, {{"monitoring", monitor->currentData().toString()}})) refresh(); });
      connect(hide, &QPushButton::clicked, this, [this,name] { engine.setAudioProperties(name, {{"mixer_hidden", true}}); QTimer::singleShot(0, this, [this] { refresh(); }); });
      layout->addWidget(strip); rows.insert(name, strip);
    }
    strip->setVisible(show);
    auto *gain = strip->findChild<QSlider *>("gain"); auto *mute = strip->findChild<QCheckBox *>("mute"); auto *scope = strip->findChild<QComboBox *>("scope"); auto *monitor = strip->findChild<QComboBox *>("monitor");
    const QSignalBlocker b1(gain), b2(mute), b3(scope), b4(monitor);
    if (!gain->isSliderDown()) gain->setValue(qRound(props["volume"].toDouble(1) * 100)); mute->setChecked(props["muted"].toBool());
    scope->setCurrentIndex(qMax(0, scope->findData(props["scope"].toString("scene")))); monitor->setCurrentIndex(qMax(0, monitor->findData(props["monitoring"].toString("off"))));
    const auto volume = props["volume"].toDouble(1); strip->findChild<QLabel *>("db")->setText(volume > 0 ? QString::number(20*std::log10(volume), 'f', 1) + " dB" : "−∞ dB");
    auto *meter = strip->findChild<QProgressBar *>("level"); const double peak = peaks.value(name).toDouble(-100);
    meter->setValue(qBound(-60, qRound(peak), 0)); meter->setFormat(peak < -60 ? "Below −60 dBFS" : QString::number(peak, 'f', 1) + " dBFS"); meter->setToolTip("Measured source peak, aggregated across channels");
    if (auto *model = qobject_cast<QStandardItemModel *>(monitor->model())) for (int i = 1; i < monitor->count(); ++i) if (model->item(i)) model->item(i)->setEnabled(engine.audioMonitoringAvailable());
    // The engine only supports global scope for audio-only sources and display companions.
    QString type; for (const auto &v : engine.sources(row["scene"].toString())) if (v.toObject()["name"].toString() == name) type = v.toObject()["type"].toString();
    auto *model = qobject_cast<QStandardItemModel *>(scope->model()); if (model && model->item(1)) model->item(1)->setEnabled(type != "window" && type != "game");
    scope->setToolTip(type == "window" || type == "game" ? "Use Application Audio for global audio." : "Global audio remains active across scenes.");
  }
  for (const auto &name : rows.keys()) if (!seen.contains(name)) { delete rows.take(name); }
}
}
