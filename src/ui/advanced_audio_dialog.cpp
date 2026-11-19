#include "feathercast/ui/advanced_audio_dialog.hpp"
#include "feathercast/engine/engine.hpp"

#include <QtWidgets>
#include <array>
#include <cmath>

namespace feathercast::ui {
namespace {
using engine::Engine;
class AudioDialog final : public QDialog {
    struct Row {
        QDoubleSpinBox* volume{};
        QCheckBox* mute{};
        QCheckBox* mono{};
        QSlider* balance{};
        QSpinBox* sync{};
        QComboBox* monitoring{};
        std::array<QCheckBox*, 6> tracks{};
        double gain{};
    };
    Engine& engine;
    QTableWidget* table;
    QCheckBox* percent;
    QCheckBox* activeOnly;
    QLabel* notice;
    QLabel* empty;
    QStringList names;
    QList<Row> rows;
    bool hasDevice;

    static QHBoxLayout* cell(QTableWidget* table, int row, int column) {
        auto* holder = new QWidget;
        auto* layout = new QHBoxLayout(holder);
        layout->setContentsMargins(6, 2, 6, 2);
        layout->setSpacing(5);
        table->setCellWidget(row, column, holder);
        return layout;
    }
    void showGain(Row& row) {
        QSignalBlocker block(row.volume);
        const bool asPercent = percent->isChecked();
        row.volume->setDecimals(asPercent ? 1 : 2);
        row.volume->setRange(asPercent ? 0 : -96.1, asPercent ? 200 : 6.02);
        row.volume->setSingleStep(asPercent ? 1 : 0.1);
        row.volume->setSuffix(asPercent ? " %" : " dB");
        row.volume->setSpecialValueText(asPercent ? QString() : QStringLiteral("−∞ dB"));
        row.volume->setValue(asPercent ? row.gain * 100 :
            (row.gain == 0 ? -96.1 : std::max(-96.0, 20.0 * std::log10(row.gain))));
    }
    void write(const QString& name, const char* key, const QJsonValue& value) {
        if (!engine.setAudioProperties(name, {{key, value}})) loadRows(engine.audioSources());
    }
    void loadRows(const QJsonArray& sources) {
        table->setRowCount(0);
        names.clear(); rows.clear();
        for (auto value : sources) {
            const auto entry = value.toObject(), audio = entry["properties"].toObject();
            const auto name = entry["name"].toString();
            const int i = table->rowCount();
            table->insertRow(i);
            auto* title = new QTableWidgetItem(name);
            title->setToolTip(name + " · " + entry["scene"].toString());
            table->setItem(i, 0, title);
            table->setItem(i, 1, new QTableWidgetItem);
            Row row;
            auto* gainCell = cell(table, i, 2);
            row.volume = new QDoubleSpinBox;
            row.volume->setAccessibleName(name + " volume");
            row.volume->setKeyboardTracking(false);
            row.volume->setMinimumWidth(94);
            row.gain = audio["volume"].toDouble();
            showGain(row);
            gainCell->addWidget(row.volume);
            row.mute = new QCheckBox("Mute");
            row.mute->setChecked(audio["muted"].toBool());
            row.mute->setAccessibleName(name + " mute");
            gainCell->addWidget(row.mute);

            row.mono = new QCheckBox;
            row.mono->setAccessibleName(name + " mono");
            row.mono->setChecked(audio["mono"].toBool());
            cell(table, i, 3)->addWidget(row.mono, 0, Qt::AlignCenter);

            auto* panCell = cell(table, i, 4);
            panCell->addWidget(new QLabel("L"));
            row.balance = new QSlider(Qt::Horizontal);
            row.balance->setRange(0, 100);
            row.balance->setValue(qRound(audio["balance"].toDouble() * 100));
            row.balance->setMinimumWidth(70);
            row.balance->setAccessibleName(name + " balance");
            row.balance->setAccessibleDescription("Left at 0, center at 50, right at 100");
            panCell->addWidget(row.balance);
            panCell->addWidget(new QLabel("R"));

            row.sync = new QSpinBox;
            row.sync->setRange(-950, 20000);
            row.sync->setSuffix(" ms");
            row.sync->setKeyboardTracking(false);
            row.sync->setAccessibleName(name + " sync offset");
            row.sync->setValue(audio["sync_offset_ms"].toInt());
            cell(table, i, 5)->addWidget(row.sync);
            row.monitoring = new QComboBox;
            row.monitoring->setAccessibleName(name + " audio monitoring");
            row.monitoring->addItem("Monitor Off", "off");
            row.monitoring->addItem("Monitor Only (mute output)", "monitor_only");
            row.monitoring->addItem("Monitor and Output", "monitor_and_output");
            row.monitoring->setCurrentIndex(row.monitoring->findData(audio["monitoring"].toString()));
            row.monitoring->setToolTip(hasDevice ? "Uses the default Windows playback device" : "No Windows playback device available");
            if (!hasDevice) {
                auto* model = qobject_cast<QStandardItemModel*>(row.monitoring->model());
                model->item(1)->setEnabled(false);
                model->item(2)->setEnabled(false);
                row.monitoring->setEnabled(row.monitoring->currentIndex() != 0);
            }
            cell(table, i, 6)->addWidget(row.monitoring);
            auto* tracksCell = cell(table, i, 7);
            for (int t = 0; t < 6; ++t) {
                const int bit = 1 << t;
                auto* check = new QCheckBox(QString::number(t + 1));
                row.tracks[t] = check;
                check->setAccessibleName(name + " track " + QString::number(t + 1));
                check->setToolTip(t == 0 ? "Track 1 · stream, replay and recording" : QString("Recording track %1").arg(t + 1));
                check->setChecked((audio["tracks"].toInt() & bit) != 0);
                tracksCell->addWidget(check);
                connect(check, &QCheckBox::toggled, this, [this, name, bit](bool on) {
                    const int mask = engine.audioProperties(name)["tracks"].toInt();
                    write(name, "tracks", on ? mask | bit : mask & ~bit);
                });
            }
            names.append(name); rows.append(row);
            connect(row.volume, &QDoubleSpinBox::valueChanged, this, [this, i, name](double v) {
                const double gain = percent->isChecked() ? v / 100 :
                    (v <= -96.1 ? 0.0 : (v >= 6.02 ? 2.0 : std::pow(10.0, v / 20)));
                rows[i].gain = gain;
                write(name, "volume", gain);
            });
            connect(row.mute, &QCheckBox::toggled, this, [this, name](bool on) { write(name, "muted", on); });
            connect(row.mono, &QCheckBox::toggled, this, [this, name](bool on) { write(name, "mono", on); });
            connect(row.balance, &QSlider::valueChanged, this, [this, name](int v) { write(name, "balance", v / 100.0); });
            connect(row.sync, &QSpinBox::valueChanged, this, [this, name](int v) { write(name, "sync_offset_ms", v); });
            connect(row.monitoring, &QComboBox::currentIndexChanged, this, [this, name, combo = row.monitoring] {
                write(name, "monitoring", combo->currentData().toString());
            });
        }
        table->resizeColumnsToContents();
        table->setColumnWidth(0, qBound(130, table->columnWidth(0), 240));
    }
    void refresh() {
        const auto sources = engine.audioSources();
        QStringList current;
        for (auto value : sources) current << value.toObject()["name"].toString();
        if (current != names) loadRows(sources);
        const bool recording = engine.recordingActive();
        int shown = 0;
        for (int i = 0; i < rows.size(); ++i) {
            const bool active = sources[i].toObject()["active"].toBool();
            table->item(i, 1)->setText(active ? "Active" : "Inactive");
            const bool hidden = activeOnly->isChecked() && !active;
            table->setRowHidden(i, hidden);
            if (!hidden) ++shown;
            for (auto* check : rows[i].tracks) check->setEnabled(!recording);
        }
        empty->setText(sources.isEmpty() ? "Add an audio source to adjust its properties." : "No active audio sources. Clear Active Sources Only to see all sources.");
        empty->setVisible(!shown);
        QStringList messages;
        if (recording) messages << "Stop recording to change track assignments.";
        if (!hasDevice) messages << "Audio monitoring requires a Windows playback device.";
        notice->setText(messages.join("  "));
        notice->setVisible(!messages.isEmpty());
    }
public:
    AudioDialog(Engine& e, QWidget* parent) : QDialog(parent), engine(e), hasDevice(e.audioMonitoringAvailable()) {
        setWindowTitle("Advanced Audio Properties");
        setSizeGripEnabled(true);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(14, 14, 14, 14);
        layout->setSpacing(10);
        table = new QTableWidget(0, 8);
        table->setAccessibleName("Source audio properties");
        table->setHorizontalHeaderLabels({"Name", "Status", "Volume", "Mono", "Balance", "Sync Offset", "Audio Monitoring", "Tracks"});
        table->verticalHeader()->hide();
        table->verticalHeader()->setDefaultSectionSize(42);
        table->setTabKeyNavigation(false);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->setShowGrid(false);
        table->setAlternatingRowColors(true);
        table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        table->horizontalHeader()->setStretchLastSection(true);
        layout->addWidget(table, 1);
        empty = new QLabel;
        layout->addWidget(empty);
        notice = new QLabel;
        notice->setWordWrap(true);
        layout->addWidget(notice);
        auto* footer = new QHBoxLayout;
        activeOnly = new QCheckBox("Active Sources Only");
        activeOnly->setChecked(true);
        percent = new QCheckBox("Volume in %");
        footer->addWidget(activeOnly);
        footer->addWidget(percent);
        footer->addStretch();
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close);
        footer->addWidget(close);
        layout->addLayout(footer);
        connect(close, &QDialogButtonBox::rejected, this, &QDialog::accept);
        connect(activeOnly, &QCheckBox::toggled, this, [this] { refresh(); });
        connect(percent, &QCheckBox::toggled, this, [this] { for (auto& row : rows) showGain(row); });
        connect(&engine, &Engine::stateChanged, this, [this] { refresh(); });
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { refresh(); });
        timer->start(500);
        refresh();
        // Allow enough room for all controls at the current DPI, with native
        // scrollbars when a smaller desktop cannot fit the full table.
        const auto available = screen()->availableGeometry().size();
        resize(qMin(available.width() - 32, qMax(1080, table->horizontalHeader()->length() + 60)),
               qMin(available.height() - 60, 380));
    }
};
}
QDialog* createAdvancedAudioDialog(engine::Engine& engine, QWidget* parent) {
    return new AudioDialog(engine, parent);
}
}
