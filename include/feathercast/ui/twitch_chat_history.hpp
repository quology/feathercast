#pragma once

#include <QJsonArray>
#include <QTextEdit>
#include <memory>

class QNetworkAccessManager;

namespace feathercast::ui {
class TwitchChatHistory final : public QTextEdit {
  Q_OBJECT
public:
  explicit TwitchChatHistory(QWidget *parent = nullptr, QNetworkAccessManager *network = nullptr);
  ~TwitchChatHistory() override;
  void setChannel(const QString &twitchUserId);
  void setAddOns(bool betterTtv, bool frankerFaceZ);
  void appendMessage(const QString &name, const QString &text, const QJsonArray &fragments);
  void clear();

protected:
  QMimeData *createMimeDataFromSelection() const override;
  void showEvent(QShowEvent *event) override;
  void hideEvent(QHideEvent *event) override;

private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace feathercast::ui
