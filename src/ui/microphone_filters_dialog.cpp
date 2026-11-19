#include "feathercast/ui/microphone_filters_dialog.hpp"
#include "feathercast/engine/engine.hpp"
#include <QtWidgets>

namespace feathercast::ui {
namespace {
class FiltersDialog final : public QDialog {
    engine::Engine& engine;
    QString source;
    QJsonArray chain;
    QMap<QString, QJsonObject> specs;
    QListWidget* list;
    QVBoxLayout* editorLayout;
    QWidget* editor{};
    QLabel* notice;
    QPushButton *add, *remove, *up, *down, *defaults;
    bool loading{};

    bool available(const QJsonObject& row) const {
        const auto spec = specs.value(row["type"].toString());
        return spec["available"].toBool() && (row["type"] != "noise_suppression" ||
            spec["methods"].toArray().contains(row["settings"].toObject()["method"]));
    }
    bool write(QJsonArray updated, bool rebuild = false, int selection = -1) {
        if (!engine.setMicrophoneFilters(source, updated)) { reload(list->currentRow()); return false; }
        chain = engine.microphoneFilters(source);
        if (rebuild) reload(selection < 0 ? list->currentRow() : selection);
        return true;
    }
    void setting(const QString& key, const QJsonValue& value) {
        const int i = list->currentRow(); if (i < 0 || i >= chain.size()) return;
        auto updated = chain; auto row = updated[i].toObject(); auto settings = row["settings"].toObject();
        settings[key] = value; row["settings"] = settings; updated[i] = row;
        write(updated, key == "method");
    }
    void showSettings() {
        if (editor) { editorLayout->removeWidget(editor); editor->hide(); editor->deleteLater(); }
        editor = new QWidget;
        editorLayout->addWidget(editor);
        auto* form = new QFormLayout(editor);
        form->setContentsMargins(14, 4, 4, 4);
        form->setSpacing(12);
        const int i = list->currentRow(); const bool selected = i >= 0 && i < chain.size();
        const bool microphone = engine.isMicrophone(source);
        remove->setEnabled(microphone && selected);
        up->setEnabled(microphone && selected && i > 0);
        down->setEnabled(microphone && selected && i + 1 < chain.size());
        defaults->setEnabled(microphone && selected && specs.value(chain[i].toObject()["type"].toString())["available"].toBool());
        if (!selected) {
            auto* empty = new QLabel("Add a filter to shape this microphone’s sound.\nFilters process from top to bottom.");
            empty->setWordWrap(true); form->addRow(empty); return;
        }
        const auto row = chain[i].toObject(), spec = specs.value(row["type"].toString()), settings = row["settings"].toObject();
        auto* heading = new QLabel(row["name"].toString()); heading->setProperty("role", "section"); form->addRow(heading);
        if (!available(row)) {
            auto* warning = new QLabel("Unavailable in this OBS runtime. Saved settings are retained; this filter is bypassed until its module or method is available.");
            warning->setWordWrap(true); form->addRow(warning);
        }
        if (row["type"] == "noise_suppression") {
            auto* method = new QComboBox; method->setAccessibleName("Suppression method");
            for (auto v : spec["methods"].toArray()) method->addItem(v == "speex" ? "Speex (lower CPU usage)" : "RNNoise (higher quality)", v.toString());
            if (method->findData(settings["method"].toString()) < 0) method->addItem(settings["method"].toString() + " (unavailable)", settings["method"].toString());
            method->setCurrentIndex(method->findData(settings["method"].toString()));
            method->setEnabled(microphone && spec["available"].toBool());
            form->addRow("Method", method);
            connect(method, &QComboBox::currentIndexChanged, this, [this, method] { setting("method", method->currentData().toString()); });
        }
        for (auto v : spec["fields"].toArray()) {
            const auto field = v.toObject(); const auto key = field["key"].toString();
            if (key == "suppress_level" && settings["method"] != "speex") continue;
            auto* spin = new QDoubleSpinBox;
            spin->setObjectName(key); spin->setAccessibleName(field["label"].toString());
            spin->setDecimals(field["integer"].toBool() ? 0 : 1);
            spin->setRange(field["min"].toDouble(), field["max"].toDouble());
            spin->setSingleStep(field["integer"].toBool() ? 1 : .1);
            spin->setSuffix(field["suffix"].toString());
            spin->setKeyboardTracking(false);
            spin->setValue(settings[key].toDouble()); spin->setMinimumWidth(130);
            spin->setEnabled(microphone && available(row));
            if (key == "suppress_level") {
                auto* holder = new QWidget; auto* line = new QHBoxLayout(holder); line->setContentsMargins(0, 0, 0, 0);
                auto* slider = new QSlider(Qt::Horizontal); slider->setAccessibleName("Suppression level slider");
                slider->setRange(-60, 0); slider->setValue(int(spin->value())); slider->setTracking(false);
                slider->setEnabled(spin->isEnabled()); line->addWidget(slider, 1); line->addWidget(spin);
                connect(slider, &QSlider::valueChanged, spin, [spin](int value) { spin->setValue(value); });
                connect(spin, &QDoubleSpinBox::valueChanged, slider, [slider](double value) { QSignalBlocker block(slider); slider->setValue(int(value)); });
                form->addRow(field["label"].toString(), holder);
            } else form->addRow(field["label"].toString(), spin);
            connect(spin, &QDoubleSpinBox::valueChanged, this, [this, key](double value) { setting(key, value); });
        }
        if (row["type"] == "noise_gate") {
            auto* help = new QLabel("Close threshold must be at or below open threshold."); help->setWordWrap(true); form->addRow(help);
        }
        editor->setEnabled(microphone);
    }
    void reload(int selection) {
        loading = true; chain = engine.microphoneFilters(source); list->clear();
        for (auto v : chain) {
            const auto row = v.toObject();
            auto* item = new QListWidgetItem(row["name"].toString() + (available(row) ? "" : " (unavailable)"), list);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(row["enabled"].toBool() ? Qt::Checked : Qt::Unchecked);
            item->setToolTip("Checked: enabled. Unchecked: bypassed. Processing order is top to bottom.");
        }
        list->setCurrentRow(chain.isEmpty() ? -1 : qBound(0, selection, int(chain.size() - 1)));
        const bool microphone = engine.isMicrophone(source);
        list->setEnabled(microphone); add->setEnabled(microphone && chain.size() < 32);
        bool any = false; for (const auto& spec : specs) any |= spec["available"].toBool();
        add->setEnabled(add->isEnabled() && any);
        notice->setText(!microphone ? "This microphone is no longer available." : !any ?
            "Audio filters are unavailable. Install the bundled obs-filters module to add filters." :
            "Changes apply immediately. Uncheck a filter to bypass it. Filters process from top to bottom.");
        loading = false; showSettings();
    }
    void addFilter(const QString& type) {
        const auto spec = specs.value(type); if (!spec["available"].toBool()) return;
        QString name = spec["name"].toString();
        auto used = [&](const QString& n) { for (auto v : chain) if (v.toObject()["name"] == n) return true; return false; };
        for (int i = 2; used(name); ++i) name = spec["name"].toString() + " " + QString::number(i);
        auto updated = chain;
        updated.append(QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"name", name},
                                   {"type", type}, {"enabled", true}, {"settings", spec["defaults"]}});
        write(updated, true, int(updated.size() - 1));
    }
    void move(int delta) {
        const int i = list->currentRow(), target = i + delta;
        if (i < 0 || target < 0 || target >= chain.size()) return;
        auto updated = chain; const auto row = updated.takeAt(i); updated.insert(target, row);
        write(updated, true, target);
    }
public:
    FiltersDialog(engine::Engine& e, QString name, QWidget* parent) : QDialog(parent), engine(e), source(std::move(name)) {
        setWindowTitle("Filters for ‘" + source + "’"); setSizeGripEnabled(true); resize(760, 420);
        for (auto value : engine.microphoneFilterTypes()) { auto spec = value.toObject(); specs.insert(spec["type"].toString(), spec); }
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(16, 16, 16, 16); layout->setSpacing(12);
        auto* body = new QHBoxLayout;
        auto* left = new QVBoxLayout;
        auto* title = new QLabel("Audio Filters"); title->setProperty("role", "section"); left->addWidget(title);
        list = new QListWidget; list->setAccessibleName("Microphone audio filters"); list->setMinimumWidth(220); left->addWidget(list, 1);
        auto* controls = new QHBoxLayout;
        auto make = [&](const QString& text, const QString& accessible) {
            auto* b = new QPushButton(text); b->setAccessibleName(accessible); b->setToolTip(accessible); controls->addWidget(b); return b;
        };
        add = make("+", "Add filter"); remove = make("−", "Remove filter"); up = make("↑", "Move filter up"); down = make("↓", "Move filter down");
        auto* menu = new QMenu(add);
        for (auto value : engine.microphoneFilterTypes()) {
            const auto spec = value.toObject(); auto* action = menu->addAction(spec["name"].toString()); action->setEnabled(spec["available"].toBool());
            connect(action, &QAction::triggered, this, [this, type = spec["type"].toString()] { addFilter(type); });
        }
        add->setMenu(menu); left->addLayout(controls); body->addLayout(left, 2);
        auto* scroll = new QScrollArea; scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        auto* holder = new QWidget; editorLayout = new QVBoxLayout(holder); editorLayout->setContentsMargins(0, 0, 0, 0); editorLayout->setAlignment(Qt::AlignTop);
        scroll->setWidget(holder); body->addWidget(scroll, 3); layout->addLayout(body, 1);
        notice = new QLabel; notice->setWordWrap(true); layout->addWidget(notice);
        auto* footer = new QDialogButtonBox(QDialogButtonBox::RestoreDefaults | QDialogButtonBox::Close);
        defaults = footer->button(QDialogButtonBox::RestoreDefaults); defaults->setText("Defaults"); defaults->setAccessibleName("Restore selected filter defaults");
        layout->addWidget(footer);
        connect(footer, &QDialogButtonBox::rejected, this, &QDialog::accept);
        connect(defaults, &QPushButton::clicked, this, [this] {
            const int i = list->currentRow(); if (i < 0) return;
            auto updated = chain; auto row = updated[i].toObject(); row["settings"] = specs.value(row["type"].toString())["defaults"]; updated[i] = row;
            write(updated, true, i);
        });
        connect(remove, &QPushButton::clicked, this, [this] { const int i = list->currentRow(); if (i >= 0) { auto updated = chain; updated.removeAt(i); write(updated, true, qMax(0, i - 1)); } });
        connect(up, &QPushButton::clicked, this, [this] { move(-1); });
        connect(down, &QPushButton::clicked, this, [this] { move(1); });
        connect(list, &QListWidget::currentRowChanged, this, [this] { if (!loading) showSettings(); });
        connect(list, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
            if (loading) return;
            const int i = list->row(item); auto updated = chain; auto row = updated[i].toObject(); row["enabled"] = item->checkState() == Qt::Checked; updated[i] = row; write(updated);
        });
        connect(&engine, &engine::Engine::scenesChanged, this, [this] { reload(list->currentRow()); });
        reload(0);
    }
};
}
QDialog* createMicrophoneFiltersDialog(engine::Engine& engine, const QString& source, QWidget* parent) {
    return new FiltersDialog(engine, source, parent);
}
}
