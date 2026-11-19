#include "feathercast/ui/source_editors.hpp"
#include "feathercast/engine/engine.hpp"
#include <QtWidgets>
#include <functional>
namespace feathercast::ui {
namespace {
// OBS stores colors as ABGR; Qt's QRgb is ARGB.
QColor fromObs(quint32 v) { return QColor(v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >> 24) & 255); }
quint32 toObs(const QColor &c) { return quint32(c.red()) | (quint32(c.green()) << 8) | (quint32(c.blue()) << 16) | (quint32(c.alpha()) << 24); }
using Getter = std::function<QJsonValue()>;
QMap<QString, Getter> fields(QFormLayout *form, const QJsonArray &schema, const QJsonObject &values) {
  QMap<QString, Getter> getters;
  for (const auto &v : schema) {
    auto field = v.toObject(); auto key = field["key"].toString(), type = field["type"].toString(); auto current = values.value(key); QWidget *widget = nullptr;
    if (type == "bool") { auto *w = new QCheckBox; w->setChecked(current.toBool()); widget = w; getters[key] = [w] { return QJsonValue(w->isChecked()); }; }
    else if (type == "int" || type == "float") { auto *w = new QDoubleSpinBox; w->setDecimals(type == "int" ? 0 : 3); w->setRange(field["min"].toDouble(), field["max"].toDouble()); w->setSingleStep(qMax(0.001, field["step"].toDouble(1))); w->setValue(current.toDouble()); widget = w; getters[key] = [w] { return QJsonValue(w->value()); }; }
    else if (type == "list") { auto *w = new QComboBox; int selected = -1; for (const auto &option : field["options"].toArray()) { auto o = option.toObject(); w->addItem(o["label"].toString(), o["value"].toVariant()); if (o["value"] == current) selected = w->count()-1; }
      if (selected < 0) { w->insertItem(0, "Current value unavailable", current.toVariant()); selected = 0; } w->setCurrentIndex(selected); widget = w; getters[key] = [w] { return QJsonValue::fromVariant(w->currentData()); }; }
    else if (type == "color") { auto *w = new QPushButton("Choose color…"); w->setProperty("obsColor", current.toVariant()); QObject::connect(w, &QPushButton::clicked, w, [w] { auto c = QColorDialog::getColor(fromObs(w->property("obsColor").toULongLong()), w, "Color", QColorDialog::ShowAlphaChannel); if (c.isValid()) w->setProperty("obsColor", QVariant::fromValue(toObs(c))); }); widget = w; getters[key] = [w] { return QJsonValue(double(w->property("obsColor").toULongLong())); }; }
    if (widget) { widget->setObjectName("property_" + key); form->addRow(field["label"].toString(), widget); }
  }
  return getters;
}
QJsonObject patch(const QMap<QString, Getter> &getters, const QJsonObject &old) { QJsonObject out; for (auto it = getters.begin(); it != getters.end(); ++it) { auto value = it.value()(); if (value != old.value(it.key())) out[it.key()] = value; } return out; }
}
void editSource(engine::Engine &engine, const QString &scene, const QString &name, QWidget *parent) {
  QDialog d(parent); if (parent && parent->property("workspaceFixture").toBool()) d.setAttribute(Qt::WA_DontShowOnScreen); d.setObjectName("sourcePropertiesDialog"); d.setWindowTitle("Properties — " + name); d.resize(520, 600); auto *outer = new QVBoxLayout(&d);
  auto *tabs = new QTabWidget; outer->addWidget(tabs); auto *propsScroll = new QScrollArea; propsScroll->setWidgetResizable(true); tabs->addTab(propsScroll, "Source");
  auto *transform = new QWidget; auto *tf = new QFormLayout(transform); tabs->addTab(transform, "Transform");
  auto *coordinateNote = new QLabel("Position is relative to the parent group. Group bounds can move its origin while keeping the source in the same place on the canvas."); coordinateNote->setWordWrap(true); tf->addRow(coordinateNote);
  auto transformValues = engine.sourceTransform(scene, name); QMap<QString,QDoubleSpinBox *> numbers;
  const QList<QPair<QString,QString>> transforms = {{"x","Position X"},{"y","Position Y"},{"scaleX","Scale X"},{"scaleY","Scale Y"},{"rotation","Rotation (degrees)"},{"boundsWidth","Bounds width"},{"boundsHeight","Bounds height"},{"cropLeft","Crop left"},{"cropTop","Crop top"},{"cropRight","Crop right"},{"cropBottom","Crop bottom"}};
  for (const auto &entry : transforms) { auto *w = new QDoubleSpinBox; const bool crop = entry.first.startsWith("crop"); const bool bounds = entry.first.startsWith("bounds"); w->setDecimals(crop ? 0 : 3); w->setRange(crop || bounds ? 0 : -100000, crop ? 32768 : 100000); w->setValue(transformValues[entry.first].toDouble()); w->setObjectName("transform_" + entry.first); tf->addRow(entry.second, w); numbers[entry.first] = w; }
  auto *bounds = new QComboBox; bounds->addItem("None",0); bounds->addItem("Stretch",1); bounds->addItem("Scale to fit",2); bounds->addItem("Scale to fill",3); bounds->addItem("Scale to width",4); bounds->addItem("Scale to height",5); bounds->addItem("Maximum size",6); bounds->setCurrentIndex(qMax(0,bounds->findData(transformValues["boundsType"].toInt()))); tf->addRow("Bounds mode", bounds);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); auto *apply = buttons->addButton("Apply", QDialogButtonBox::ApplyRole); apply->setObjectName("applySource"); outer->addWidget(buttons); QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  QJsonObject values; QMap<QString, Getter> getters;
  auto rebuild = [&] { values = engine.sourceProperties(scene,name); auto *page = new QWidget; auto *form = new QFormLayout(page); getters = fields(form, engine.sourcePropertySchema(scene,name), values); propsScroll->setWidget(page); };
  rebuild();
  // Compare edits with what the widgets actually displayed, not unrounded native
  // floats. OBS may move a group's origin on a later video frame. Refresh clean
  // controls together with their baseline, retaining fields the user is editing.
  QJsonObject displayedTransform;
  auto refreshTransform = [&](bool force) {
    const auto native = engine.sourceTransform(scene, name);
    for (auto it = numbers.begin(); it != numbers.end(); ++it) {
      auto *w = it.value();
      if (force || (!w->hasFocus() && w->value() == displayedTransform.value(it.key()).toDouble())) {
        const QSignalBlocker blocker(w); w->setValue(native.value(it.key()).toDouble()); displayedTransform[it.key()] = w->value();
      }
    }
    if (force || (!bounds->hasFocus() && bounds->currentData().toInt() == displayedTransform.value("boundsType").toInt())) {
      const QSignalBlocker blocker(bounds); bounds->setCurrentIndex(qMax(0, bounds->findData(native.value("boundsType").toInt()))); displayedTransform["boundsType"] = bounds->currentData().toInt();
    }
  };
  refreshTransform(true);
  QTimer transformTimer(&d); transformTimer.setInterval(100);
  QObject::connect(&transformTimer, &QTimer::timeout, &d, [&] { refreshTransform(false); }); transformTimer.start();
  QObject::connect(apply, &QPushButton::clicked, &d, [&] {
    auto changes = patch(getters, values); if (!changes.isEmpty() && !engine.setSourceProperties(scene,name,changes)) return;
    QJsonObject t; for (auto it = numbers.begin(); it != numbers.end(); ++it) if (it.value()->value() != displayedTransform.value(it.key()).toDouble()) t[it.key()] = it.value()->value();
    if (bounds->currentData().toInt() != displayedTransform.value("boundsType").toInt()) t["boundsType"] = bounds->currentData().toInt();
    if (!t.isEmpty() && !engine.setSourceTransform(scene,name,t)) return;
    refreshTransform(true); rebuild();
  }); d.exec();
}
void editVideoFilters(engine::Engine &engine, const QString &scene, const QString &name, QWidget *parent) {
  QDialog d(parent); if (parent && parent->property("workspaceFixture").toBool()) d.setAttribute(Qt::WA_DontShowOnScreen); d.setObjectName("videoFiltersDialog"); d.setWindowTitle("Video filters — " + name); d.resize(620,480); auto *outer = new QVBoxLayout(&d); auto *list = new QListWidget; outer->addWidget(list);
  auto chain = engine.videoFilters(scene,name), catalog = engine.videoFilterTypes(); bool updating = false;
  auto refresh = [&] { updating = true; int selected = list->currentRow(); list->clear(); for (const auto &v : chain) { auto o = v.toObject(); auto *item = new QListWidgetItem(o["name"].toString(),list); item->setFlags(item->flags() | Qt::ItemIsUserCheckable); item->setCheckState(o["enabled"].toBool() ? Qt::Checked : Qt::Unchecked); } list->setCurrentRow(qMin(selected,list->count()-1)); updating = false; };
  auto commit = [&] { if (!engine.setVideoFilters(scene,name,chain)) chain = engine.videoFilters(scene,name); refresh(); };
  QObject::connect(list, &QListWidget::itemChanged, &d, [&](QListWidgetItem *item) { if (updating) return; int i = list->row(item); auto row = chain[i].toObject(); row["enabled"] = item->checkState() == Qt::Checked; chain[i] = row; commit(); });
  auto *buttons = new QHBoxLayout; outer->addLayout(buttons);
  auto addButton = [&](const QString &text, auto action) { auto *b = new QPushButton(text); b->setObjectName("filter" + text); buttons->addWidget(b); QObject::connect(b, &QPushButton::clicked, &d, action); };
  addButton("Add", [&] { QStringList names; for (auto v : catalog) names << v.toObject()["name"].toString(); QInputDialog choose(&d); if (parent && parent->property("workspaceFixture").toBool()) choose.setAttribute(Qt::WA_DontShowOnScreen); choose.setWindowTitle("Add filter"); choose.setLabelText("Type"); choose.setComboBoxItems(names); choose.setComboBoxEditable(false); if (choose.exec() != QDialog::Accepted) return; auto choice = choose.textValue(); int i = names.indexOf(choice); if (i < 0) return; auto spec = catalog[i].toObject(); chain.append(QJsonObject{{"id",QUuid::createUuid().toString(QUuid::Id128)},{"name",choice},{"type",spec["type"]},{"enabled",true},{"settings",spec["defaults"]}}); commit(); });
  addButton("Remove", [&] { int i = list->currentRow(); if (i >= 0) { chain.removeAt(i); commit(); } });
  addButton("Up", [&] { int i = list->currentRow(); if (i > 0) { QJsonValue a = chain.at(i-1); chain[i-1] = chain[i]; chain[i] = a; commit(); list->setCurrentRow(i-1); } });
  addButton("Down", [&] { int i = list->currentRow(); if (i >= 0 && i+1 < chain.size()) { QJsonValue a = chain.at(i+1); chain[i+1] = chain[i]; chain[i] = a; commit(); list->setCurrentRow(i+1); } });
  addButton("Edit", [&] { int i = list->currentRow(); if (i < 0) return; auto row = chain[i].toObject(); QJsonObject spec; for (auto v : catalog) if (v.toObject()["type"] == row["type"]) spec = v.toObject(); QDialog edit(&d); if (parent && parent->property("workspaceFixture").toBool()) edit.setAttribute(Qt::WA_DontShowOnScreen); edit.setObjectName("videoFilterSettingsDialog"); edit.setWindowTitle(row["name"].toString()); auto *layout = new QVBoxLayout(&edit); auto *form = new QFormLayout; layout->addLayout(form); auto old = row["settings"].toObject(); auto getters = fields(form,spec["fields"].toArray(),old); auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); layout->addWidget(box); QObject::connect(box,&QDialogButtonBox::accepted,&edit,&QDialog::accept); QObject::connect(box,&QDialogButtonBox::rejected,&edit,&QDialog::reject); if (edit.exec() != QDialog::Accepted) return; auto changed = patch(getters,old); for (auto it = changed.begin(); it != changed.end(); ++it) old[it.key()] = it.value(); row["settings"] = old; chain[i] = row; commit(); });
  auto *close = new QDialogButtonBox(QDialogButtonBox::Close); outer->addWidget(close); QObject::connect(close,&QDialogButtonBox::rejected,&d,&QDialog::reject); refresh(); d.exec();
}
}
