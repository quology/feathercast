#pragma once
#include <QDialog>
#include <QString>
namespace feathercast::engine { class Engine; }
namespace feathercast::ui {
// Engine must outlive this dialog. Changes take effect immediately.
QDialog* createMicrophoneFiltersDialog(engine::Engine& engine, const QString& source, QWidget* parent = nullptr);
}
