#pragma once
#include <QString>
class QWidget;
namespace feathercast::engine { class Engine; }
namespace feathercast::ui {
void editSource(engine::Engine &, const QString &scene, const QString &name, QWidget *parent);
void editVideoFilters(engine::Engine &, const QString &scene, const QString &name, QWidget *parent);
}
