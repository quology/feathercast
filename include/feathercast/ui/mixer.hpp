#pragma once
#include <QWidget>
#include <QHash>
namespace feathercast::engine { class Engine; }
namespace feathercast::ui {
class Mixer : public QWidget {
public:
  explicit Mixer(engine::Engine &engine, QWidget *parent = nullptr);
  void refresh();
private:
  engine::Engine &engine;
  QWidget *strips{};
  QHash<QString, QWidget *> rows;
  bool activeOnly = false;
};
}
