#pragma once
#include <QObject>
#include <QDateTime>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <functional>

class QWebSocket;
namespace feathercast::twitch {
// Endpoints are injectable for deterministic local protocol tests, never user settings.
struct Endpoints {
  QUrl oauth{"https://id.twitch.tv/oauth2/"};
  QUrl helix{"https://api.twitch.tv/helix/"};
  QUrl events{"wss://eventsub.wss.twitch.tv/ws"};
};
QStringList requiredScopes();
QStringList activityScopes();
bool validMetadata(const QString &title, const QStringList &tags);
bool validReconnectUrl(const QUrl &url);
QByteArray protectTokens(const QByteArray &plain);
QByteArray unprotectTokens(const QByteArray &cipher);

class TwitchService final : public QObject {
  Q_OBJECT
public:
  explicit TwitchService(QString storageDirectory, QObject *parent = nullptr,
                         Endpoints endpoints = {});
  ~TwitchService() override;
  bool connected() const { return online_; }
  QString clientId() const { return client_; }
  void restore();
  void connectAccount(const QString &publicClientId);
  void disconnectAccount();
  void searchCategories(const QString &query);
  void updateChannel(const QString &title, const QString &categoryId,
                     const QStringList &tags);
  void sendChat(const QString &message);
  void pollStatus();
signals:
  void activityReceived(const QJsonObject &activity);
  void activityStateChanged(const QString &type, const QString &state);
  void statisticsReceived(const QJsonObject &statistics);
  void goalsReceived(const QJsonArray &goals);
  void connectionChanged(bool connected, const QString &account);
  void statusMessage(const QString &message);
  void deviceAuthorization(const QUrl &url, const QString &code);
  void channelReceived(const QJsonObject &channel);
  void streamReceived(const QJsonObject &stream);
  void streamUnavailable();
  void categoriesReceived(const QJsonArray &categories);
  void chatReceived(const QString &name, const QString &text);
  void chatStateChanged(const QString &state);
  void chatSendFinished(bool sent);
  void channelUpdateFinished(bool saved);
private:
  using Callback = std::function<void(int, QJsonObject)>;
  struct Work { QByteArray method; QString path; QJsonObject body; Callback done; bool retried{}; bool optional{}; };
  QString path_, client_, access_, refresh_, user_, login_, device_;
  QLockFile sessionLock_;
  Endpoints endpoints_;
  QNetworkAccessManager network_;
  QSet<QNetworkReply *> replies_;
  QTimer deviceTimer_, validateTimer_, statusTimer_, watchdog_, reconnectTimer_;
  QDateTime deviceExpiry_;
  int deviceInterval_{5000}, generation_{}, socketBackoff_{1000};
  qint64 authRetryAfter_{};
  bool online_{}, refreshing_{}, validating_{}, devicePending_{};
  QQueue<Work> refreshQueue_;
  QHash<QString, qint64> blockedUntil_;
  QPointer<QWebSocket> socket_, replacement_;
  QQueue<QString> eventIds_;
  QSet<QString> seenEvents_;
  QSet<QString> scopes_, pendingStats_, revokedTypes_;
  quint64 goalRevision_{};
  QJsonObject statistics_;
  QHash<QString, QJsonObject> goals_;
  void pollStatistics();
  void publishGoals();
  void subscribeActivity(const QString &session, QWebSocket *socket);
  void receiveActivity(const QString &type, const QJsonObject &event, const QJsonObject &metadata);
  void raw(const QByteArray &method, const QUrl &url, const QByteArray &body,
           const QByteArray &contentType, bool auth, Callback done);
  void oauth(const QString &endpoint, const QMap<QString, QString> &form, Callback done);
  void api(Work work);
  void refreshToken();
  void validate(bool afterRefresh = false);
  bool acceptTokens(const QJsonObject &object);
  bool saveTokens();
  void endSession(bool erase);
  void pollDevice();
  void openSocket(const QUrl &url, bool transferred = false);
  void handleEvent(QWebSocket *socket, bool transferred, const QString &message);
  void scheduleReconnect();
};
} // namespace feathercast::twitch
