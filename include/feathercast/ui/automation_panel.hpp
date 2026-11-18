#pragma once
#include "feathercast/engine/engine.hpp"
#include <QAbstractNativeEventFilter>
#include <QtWidgets>
namespace feathercast::ui {
QJsonObject encodeRules(const std::vector<core::Rule> &rules, bool enabled,
                        const QString &fallback);
bool decodeRules(const QJsonObject &, std::vector<core::Rule> &rules,
                 bool &enabled, QString &fallback, QString &error);
class AutomationPanel final : public QWidget,
                              public QAbstractNativeEventFilter {
public:
  AutomationPanel(engine::Engine *, QString directory,
                  QWidget *parent = nullptr);
  ~AutomationPanel() override;
  void save();
  void manualOverride();
  void editHotkeys(QWidget *parent);
  bool nativeEventFilter(const QByteArray &, void *, qintptr *) override;
  QJsonObject selfTest();

private:
  engine::Engine *engine;
  QString path;
  core::AutomationEngine evaluator;
  QElapsedTimer clock;
  QTimer timer;
  QListWidget *rulesList, *log;
  QCheckBox *enabled;
  QComboBox *fallback;
  std::vector<core::Rule> rules;
  struct Binding {
    QString action;
    QKeySequence sequence;
    bool registered{};
  };
  QList<Binding> bindings;
  QHash<QString, qint64> pulses;
  void refresh();
  bool editRule(int);
  void tick();
  bool dispatch(const core::Action &);
  void appendLog(const QString &);
  void registerBindings();
  core::AutomationSnapshot snapshot();
};
} // namespace feathercast::ui
