#pragma once
#include <QString>
class QMainWindow;
namespace feathercast::ui {
void installTwitchDocks(QMainWindow *window, const QString &profileDirectory);
}
