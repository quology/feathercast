#pragma once
#include <QDialog>

namespace feathercast::engine { class Engine; }
namespace feathercast::ui {
// The dialog owns its widgets. The engine must outlive the dialog.
QDialog* createAdvancedAudioDialog(engine::Engine& engine, QWidget* parent = nullptr);
}
