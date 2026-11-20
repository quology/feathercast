#include "feathercast/twitch/twitch_service.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrlQuery>
#include <QWebSocket>
#ifdef Q_OS_WIN
#include <windows.h>
#include <wincrypt.h>
#endif
#include <algorithm>

namespace feathercast::twitch {
QStringList requiredScopes() { return {"channel:manage:broadcast", "user:read:chat", "user:write:chat"}; }
QStringList authorizationScopes() {
  auto scopes = requiredScopes();
  scopes.append({"moderator:read:followers", "channel:read:subscriptions", "bits:read"});
  return scopes;
}
namespace {
QString activityScope(const QString &type) {
  if (type == "channel.follow") return "moderator:read:followers";
  if (type == "channel.cheer") return "bits:read";
  if (type.startsWith("channel.subscri")) return "channel:read:subscriptions";
  return {};
}
QStringList activityTypes() {
  return {"channel.follow", "channel.subscribe", "channel.subscription.message",
          "channel.subscription.gift", "channel.cheer", "channel.raid"};
}
}
bool TwitchService::activityAuthorized() const {
  for (const auto &type : activityTypes()) {
    const auto scope = activityScope(type);
    if (!scope.isEmpty() && !scopes_.contains(scope)) return false;
  }
  return online_;
}
bool validMetadata(const QString &title, const QStringList &tags) {
  if (title.trimmed().isEmpty() || title.toUcs4().size() > 140 || tags.size() > 10) return false;
  QSet<QString> unique;
  for (const auto &tag : tags) {
    if (tag.isEmpty() || tag.toUcs4().size() > 25 || unique.contains(tag.toCaseFolded())) return false;
    for (const auto c : tag.toUcs4()) if (!QChar::isLetterOrNumber(c)) return false;
    unique.insert(tag.toCaseFolded());
  }
  return true;
}
bool validReconnectUrl(const QUrl &url) {
  return url.isValid() && url.scheme() == "wss" &&
      (url.host() == "eventsub.wss.twitch.tv" || url.host().endsWith(".eventsub.wss.twitch.tv")) &&
      url.userInfo().isEmpty() && (url.port(-1) == -1 || url.port() == 443);
}
QByteArray protectTokens(const QByteArray &plain) {
#ifdef Q_OS_WIN
  DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE *>(const_cast<char *>(plain.constData()))}, out{};
  if (!CryptProtectData(&in, L"FeatherCast Twitch", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
  QByteArray result(reinterpret_cast<char *>(out.pbData), out.cbData);
  SecureZeroMemory(out.pbData, out.cbData); LocalFree(out.pbData); return result;
#else
  Q_UNUSED(plain); return {};
#endif
}
QByteArray unprotectTokens(const QByteArray &cipher) {
#ifdef Q_OS_WIN
  DATA_BLOB in{DWORD(cipher.size()), reinterpret_cast<BYTE *>(const_cast<char *>(cipher.constData()))}, out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
  QByteArray result(reinterpret_cast<char *>(out.pbData), out.cbData);
  SecureZeroMemory(out.pbData, out.cbData); LocalFree(out.pbData); return result;
#else
  Q_UNUSED(cipher); return {};
#endif
}
TwitchService::TwitchService(QString directory, QObject *parent, Endpoints endpoints)
    : QObject(parent), path_(std::move(directory) + "/twitch.tokens"), sessionLock_(path_ + ".lock"), endpoints_(std::move(endpoints)) {
  // A connected profile can hold this lock for the entire app session.
  sessionLock_.setStaleLockTime(0);
  network_.setTransferTimeout(15000);
  deviceTimer_.setSingleShot(true);
  watchdog_.setSingleShot(true);
  reconnectTimer_.setSingleShot(true);
  connect(&deviceTimer_, &QTimer::timeout, this, &TwitchService::pollDevice);
  connect(&validateTimer_, &QTimer::timeout, this, [this] { validate(); });
  connect(&statusTimer_, &QTimer::timeout, this, &TwitchService::pollStatus);
  connect(&watchdog_, &QTimer::timeout, this, [this] {
    if (socket_) socket_->abort();
    if (replacement_) replacement_->abort();
    scheduleReconnect();
  });
  connect(&reconnectTimer_, &QTimer::timeout, this, [this] { openSocket(endpoints_.events); });
}
TwitchService::~TwitchService() {
  disconnect(this, nullptr, nullptr, nullptr);
  endSession(false);
}
void TwitchService::raw(const QByteArray &method, const QUrl &url, const QByteArray &body,
                        const QByteArray &type, bool auth, Callback done) {
  if (replies_.size() >= 32) { done(0, {}); return; }
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  if (!type.isEmpty()) request.setHeader(QNetworkRequest::ContentTypeHeader, type);
  if (auth) {
    request.setRawHeader("Authorization", "Bearer " + access_.toUtf8());
    request.setRawHeader("Client-Id", client_.toUtf8());
  }
  auto *reply = network_.sendCustomRequest(request, method, body);
  replies_.insert(reply);
  const auto epoch = generation_;
  // Hard deadline as transferTimeout alone resets on received bytes.
  QTimer::singleShot(20000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
  connect(reply, &QIODevice::readyRead, reply, [reply] {
    if (reply->bytesAvailable() > 1024 * 1024) reply->abort();
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply, epoch, auth, url, done = std::move(done)] {
    replies_.remove(reply); reply->deleteLater();
    if (epoch != generation_) return;
    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError && status < 400) status = 0;
    if (url.path().startsWith(endpoints_.oauth.path()) && status == 429) {
      const auto retry = reply->rawHeader("Retry-After").toLongLong();
      authRetryAfter_ = QDateTime::currentSecsSinceEpoch() + std::clamp<qint64>(retry > 0 ? retry : 60, 1, 3600);
    }
    if (auth) {
      const auto bucket = url.path().endsWith("/chat/messages") ? QString("chat") : QString("helix");
      if (status == 429 || reply->rawHeader("Ratelimit-Remaining") == "0") {
        const auto now = QDateTime::currentSecsSinceEpoch();
        auto until = reply->rawHeader("Ratelimit-Reset").toLongLong();
        until = std::clamp(until, now + 1, now + 3600);
        if (status == 429 && reply->rawHeader("Ratelimit-Reset").isEmpty()) until = now + 60;
        blockedUntil_[bucket] = until;
      }
    }
    const auto bytes = reply->readAll();
    QJsonParseError error;
    auto document = QJsonDocument::fromJson(bytes, &error);
    if (status >= 200 && status < 300 && status != 204 &&
        (error.error != QJsonParseError::NoError || !document.isObject())) status = 0;
    done(status, document.object());
  });
}
void TwitchService::oauth(const QString &endpoint, const QMap<QString, QString> &form, Callback done) {
  QByteArray query;
  for (auto it = form.cbegin(); it != form.cend(); ++it) {
    if (!query.isEmpty()) query += '&';
    query += QUrl::toPercentEncoding(it.key()) + '=' + QUrl::toPercentEncoding(it.value());
  }
  raw("POST", endpoints_.oauth.resolved(QUrl(endpoint)), query,
      "application/x-www-form-urlencoded", false, std::move(done));
}
void TwitchService::endSession(bool erase) {
  ++generation_; ++eventGeneration_; online_ = false; refreshing_ = false; validating_ = false; devicePending_ = false;
  for (auto *timer : {&deviceTimer_, &validateTimer_, &statusTimer_, &watchdog_, &reconnectTimer_}) timer->stop();
  for (auto *socket : {socket_.data(), replacement_.data()}) if (socket) {
    socket->disconnect(this); socket->abort(); socket->deleteLater();
  }
  socket_ = nullptr; replacement_ = nullptr;
  auto replies = replies_; replies_.clear();
  for (auto *reply : replies) reply->abort();
  refreshQueue_.clear(); blockedUntil_.clear(); seenEvents_.clear(); eventIds_.clear();
  access_.clear(); refresh_.clear(); user_.clear(); login_.clear(); device_.clear();
  scopes_.clear(); activitySubscriptions_.clear(); eventSession_.clear();
  if (erase && sessionLock_.isLocked() && QFile::exists(path_) && !QFile::remove(path_))
    emit statusMessage("Could not remove saved Twitch credentials. Remove twitch.tokens from your profile before restarting.");
  sessionLock_.unlock();
  emit connectionChanged(false, {});
  emit chatStateChanged("Chat disconnected");
  emit activityStateChanged("Activity feed disconnected");
}
void TwitchService::disconnectAccount() {
  upgradingUser_.clear();
  endSession(true);
  emit statusMessage(QFile::exists(path_) ? "Twitch disconnected locally. Saved credentials could not be removed; delete twitch.tokens before restarting." : "Twitch disconnected locally. Saved tokens removed.");
}
bool TwitchService::saveTokens() {
  QDir().mkpath(QFileInfo(path_).absolutePath());
  const auto cipher = protectTokens(QJsonDocument(QJsonObject{{"client_id", client_}, {"access_token", access_}, {"refresh_token", refresh_}}).toJson(QJsonDocument::Compact));
  if (cipher.isEmpty()) return false;
  QSaveFile file(path_);
  return file.open(QIODevice::WriteOnly) && file.write(cipher) == cipher.size() && file.commit();
}
bool TwitchService::acceptTokens(const QJsonObject &object) {
  auto access = object["access_token"].toString(), refresh = object["refresh_token"].toString();
  if (access.isEmpty() || refresh.isEmpty()) { if (restorePreviousAuthorization()) return false; endSession(true); emit statusMessage("Twitch returned incomplete credentials. Connect again."); return false; }
  access_ = access; refresh_ = refresh;
  if (!upgradingUser_.isEmpty()) return true;
  if (!saveTokens()) { endSession(true); emit statusMessage("Windows could not save encrypted Twitch credentials. Connect again."); return false; }
  return true;
}
void TwitchService::restore() {
  if (online_ || !access_.isEmpty() || devicePending_) return;
  if (authRetryAfter_ > QDateTime::currentSecsSinceEpoch()) { emit statusMessage("Twitch authentication rate limit reached. Wait before connecting again."); return; }
  QFile file(path_);
  if (!file.exists()) return;
  if (!sessionLock_.isLocked() && !sessionLock_.tryLock(0)) { emit statusMessage("Another VELA instance is using this Twitch profile. Close it before connecting."); return; }
  if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) { sessionLock_.unlock(); emit statusMessage("Saved Twitch credentials could not be read. Connect again."); return; }
  const auto data = QJsonDocument::fromJson(unprotectTokens(file.readAll())).object();
  file.close();
  client_ = data["client_id"].toString(); access_ = data["access_token"].toString(); refresh_ = data["refresh_token"].toString();
  if (client_.isEmpty() || access_.isEmpty() || refresh_.isEmpty()) { endSession(true); emit statusMessage("Saved Twitch credentials are unavailable for this Windows account. Connect again."); return; }
  emit statusMessage("Validating saved Twitch account…"); validate();
}
void TwitchService::connectAccount(const QString &id) {
  upgradingUser_.clear();
  beginAuthorization(id);
}
void TwitchService::authorizeActivity() {
  if (!online_) return;
  if (authRetryAfter_ > QDateTime::currentSecsSinceEpoch()) {
    emit statusMessage("Twitch authentication rate limit reached. Wait before authorizing the activity feed.");
    return;
  }
  upgradingUser_ = user_;
  const auto id = client_;
  beginAuthorization(id);
}
bool TwitchService::restorePreviousAuthorization() {
  if (upgradingUser_.isEmpty()) return false;
  upgradingUser_.clear();
  endSession(false);
  emit deviceAuthorization({}, {});
  emit statusMessage("Activity authorization did not complete. Restoring your previous Twitch connection.");
  QTimer::singleShot(0, this, &TwitchService::restore);
  return true;
}
void TwitchService::beginAuthorization(const QString &id) {
  if (authRetryAfter_ > QDateTime::currentSecsSinceEpoch()) { emit statusMessage("Twitch authentication rate limit reached. Wait before connecting again."); return; }
  endSession(false);
  QDir().mkpath(QFileInfo(path_).absolutePath());
  if (!sessionLock_.tryLock(0)) { emit statusMessage("Another VELA instance is using this Twitch profile. Close it before connecting."); return; }
  client_ = id.trimmed();
  if (!QRegularExpression("^[A-Za-z0-9]{5,128}$").match(client_).hasMatch()) { sessionLock_.unlock(); emit statusMessage("Enter a valid public Twitch Client ID."); return; }
  devicePending_ = true;
  emit statusMessage("Requesting Twitch authorization…");
  oauth("device", {{"client_id", client_}, {"scopes", authorizationScopes().join(' ')}}, [this](int status, QJsonObject data) {
    const QUrl url(data["verification_uri"].toString());
    if (status != 200 || data["device_code"].toString().isEmpty() || data["user_code"].toString().isEmpty() ||
        url.scheme() != "https" || url.host() != "www.twitch.tv" || !url.userInfo().isEmpty() || data["expires_in"].toInt() <= 0) {
      if (restorePreviousAuthorization()) return;
      devicePending_ = false; sessionLock_.unlock(); emit deviceAuthorization({}, {}); emit statusMessage("Twitch authorization could not start. Check your public Client ID and network, then try again."); return;
    }
    device_ = data["device_code"].toString();
    deviceInterval_ = std::clamp(data["interval"].toInt(5), 1, 60) * 1000;
    deviceExpiry_ = QDateTime::currentDateTimeUtc().addSecs(std::min(data["expires_in"].toInt(), 1800));
    emit deviceAuthorization(url, data["user_code"].toString());
    deviceTimer_.start(int(std::max<qint64>(deviceInterval_, std::max<qint64>(0, authRetryAfter_ - QDateTime::currentSecsSinceEpoch()) * 1000)));
  });
}
void TwitchService::pollDevice() {
  if (!devicePending_) return;
  if (QDateTime::currentDateTimeUtc() >= deviceExpiry_) { if (restorePreviousAuthorization()) return; devicePending_ = false; device_.clear(); sessionLock_.unlock(); emit deviceAuthorization({}, {}); emit statusMessage("Twitch authorization expired. Connect again."); return; }
  oauth("token", {{"client_id", client_}, {"device_code", device_}, {"scopes", authorizationScopes().join(' ')},
      {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}}, [this](int status, QJsonObject data) {
    if (status == 200) { devicePending_ = false; device_.clear(); if (acceptTokens(data)) validate(); return; }
    const auto error = data["error"].toString(data["message"].toString());
    if (error == "slow_down" || status == 429) deviceInterval_ = std::min(deviceInterval_ + 5000, 60000);
    else if (status != 0 && status < 500 && error != "authorization_pending") {
      if (restorePreviousAuthorization()) return;
      devicePending_ = false; device_.clear(); sessionLock_.unlock(); emit deviceAuthorization({}, {}); emit statusMessage("Twitch authorization was declined or expired. Connect again."); return;
    }
    deviceTimer_.start(int(std::max<qint64>(deviceInterval_, std::max<qint64>(0, authRetryAfter_ - QDateTime::currentSecsSinceEpoch()) * 1000)));
  });
}
void TwitchService::validate(bool afterRefresh) {
  if (access_.isEmpty() || validating_ || refreshing_) return;
  validating_ = true;
  const auto sentToken = access_;
  raw("GET", endpoints_.oauth.resolved(QUrl("validate")), {}, {}, true, [this, afterRefresh, sentToken](int status, QJsonObject data) {
    validating_ = false;
    if (sentToken != access_) { validate(true); return; }
    if (status == 401 && !afterRefresh) { refreshToken(); return; }
    bool valid = status == 200 && data["client_id"].toString() == client_ && !data["user_id"].toString().isEmpty();
    const bool upgrading = !upgradingUser_.isEmpty();
    const auto scopes = data["scopes"].toArray();
    for (const auto &scope : upgrading ? authorizationScopes() : requiredScopes()) valid = valid && scopes.contains(scope);
    if (upgrading && data["user_id"].toString() != upgradingUser_) valid = false;
    if (!valid) {
      if (restorePreviousAuthorization()) return;
      const bool transient = status == 0 || status == 429 || status >= 500;
      endSession(!transient);
      emit statusMessage(transient ? "Twitch validation unavailable. Use Connect to retry saved credentials." : "Twitch authorization is invalid or lacks required permissions. Connect again.");
      return;
    }
    if (upgrading) {
      if (!saveTokens()) { restorePreviousAuthorization(); return; }
      upgradingUser_.clear();
    }
    scopes_.clear();
    for (const auto &scope : scopes) scopes_.insert(scope.toString());
    user_ = data["user_id"].toString(); login_ = data["login"].toString();
    const bool first = !online_; online_ = true;
    validateTimer_.start(60 * 60 * 1000);
    if (first) {
      emit connectionChanged(true, login_); emit statusMessage("Connected to Twitch as " + login_);
      statusTimer_.start(30000); pollStatus(); openSocket(endpoints_.events);
    }
    auto queue = std::move(refreshQueue_); refreshQueue_.clear();
    while (!queue.isEmpty()) api(queue.dequeue());
  });
}
void TwitchService::refreshToken() {
  if (refreshing_) return;
  if (refresh_.isEmpty()) { endSession(true); emit statusMessage("Twitch authorization expired. Connect again."); return; }
  refreshing_ = true;
  oauth("token", {{"client_id", client_}, {"refresh_token", refresh_}, {"grant_type", "refresh_token"}}, [this](int status, QJsonObject data) {
    refreshing_ = false;
    if (status != 200) {
      if (restorePreviousAuthorization()) return;
      const bool transient = status == 0 || status == 429 || status >= 500;
      endSession(!transient);
      emit statusMessage(transient ? "Twitch credentials could not be renewed. Wait, then use Connect to retry saved credentials." : "Twitch credentials expired or were revoked. Connect again."); return;
    }
    if (acceptTokens(data)) validate(true);
  });
}
void TwitchService::api(Work work) {
  if (!online_) { if (work.done) work.done(0, {}); return; }
  if (refreshing_ || validating_) {
    if (refreshQueue_.size() < 32) refreshQueue_.enqueue(std::move(work));
    else if (work.done) work.done(0, {});
    return;
  }
  const auto bucket = work.path.startsWith("chat/messages") ? QString("chat") : QString("helix");
  if (blockedUntil_.value(bucket) > QDateTime::currentSecsSinceEpoch()) {
    emit statusMessage("Twitch rate limit reached. Wait before trying again.");
    if (work.done) work.done(429, {}); return;
  }
  const auto sentToken = access_;
  const auto method = work.method;
  const auto url = endpoints_.helix.resolved(QUrl(work.path));
  const auto body = work.body.isEmpty() ? QByteArray{} : QJsonDocument(work.body).toJson(QJsonDocument::Compact);
  raw(method, url, body,
      "application/json", true, [this, work = std::move(work), sentToken](int status, QJsonObject data) mutable {
    if (status == 401 && !work.retried) {
      work.retried = true;
      if (sentToken != access_) { api(std::move(work)); return; }
      if (refreshQueue_.size() >= 32) { if (work.done) work.done(0, {}); return; }
      refreshQueue_.enqueue(std::move(work)); refreshToken(); return;
    }
    if (status == 401) { endSession(true); emit statusMessage("Twitch authorization failed. Connect again."); return; }
    if (status == 429) emit statusMessage("Twitch rate limit reached. Wait before trying again.");
    else if (status == 403) emit statusMessage("Twitch denied this action. Check channel permissions.");
    else if (status == 0 || status >= 500) emit statusMessage("Twitch request failed. Check your connection. Changes and chat are never automatically resent after a network error.");
    else if (status >= 400) emit statusMessage("Twitch rejected this request. Check your input and try again.");
    if (work.done) work.done(status, data);
  });
}
void TwitchService::pollStatus() {
  if (!online_) return;
  api({"GET", "streams?user_id=" + user_, {}, [this](int code, QJsonObject o) {
    if (code == 200 && o["data"].isArray()) {
      const auto streams = o["data"].toArray();
      if (streams.isEmpty()) emit streamReceived({});
      else {
        const auto stream = streams.first().toObject();
        if (stream["viewer_count"].isDouble() && stream["viewer_count"].toInt(-1) >= 0 &&
            QDateTime::fromString(stream["started_at"].toString(), Qt::ISODate).isValid()) emit streamReceived(stream);
        else emit streamUnavailable();
      }
    } else emit streamUnavailable();
  }});
  api({"GET", "channels?broadcaster_id=" + user_, {}, [this](int code, QJsonObject o) {
    if (code == 200 && !o["data"].toArray().isEmpty()) emit channelReceived(o["data"].toArray().first().toObject());
  }});
}
void TwitchService::searchCategories(const QString &query) {
  if (!online_ || query.trimmed().isEmpty()) return;
  api({"GET", "search/categories?first=30&query=" + QString::fromLatin1(QUrl::toPercentEncoding(query.trimmed())), {},
       [this](int code, QJsonObject o) { if (code == 200) { const auto results = o["data"].toArray(); emit categoriesReceived(results); emit statusMessage(results.isEmpty() ? "No Twitch categories found." : "Choose a category from the search results, then Save."); } }});
}
void TwitchService::updateChannel(const QString &title, const QString &category, const QStringList &tags) {
  if (!online_ || !validMetadata(title, tags)) { emit statusMessage("Use a title of 1–140 characters and up to 10 unique alphanumeric tags of 1–25 characters."); emit channelUpdateFinished(false); return; }
  QJsonObject body{{"title", title}, {"tags", QJsonArray::fromStringList(tags)}};
  if (!category.isEmpty()) body["game_id"] = category;
  api({"PATCH", "channels?broadcaster_id=" + user_, body, [this](int status, QJsonObject) {
    emit channelUpdateFinished(status == 204);
    if (status == 204) { emit statusMessage("Twitch channel information saved."); pollStatus(); }
  }});
}
void TwitchService::sendChat(const QString &message) {
  if (!online_ || message.trimmed().isEmpty() || message.toUcs4().size() > 500 || message.contains('\n') || message.contains('\r')) { emit statusMessage("Chat messages must contain 1–500 characters on one line."); emit chatSendFinished(false); return; }
  api({"POST", "chat/messages", {{"broadcaster_id", user_}, {"sender_id", user_}, {"message", message}},
       [this](int status, QJsonObject o) {
    const auto data = o["data"].toArray();
    const bool sent = status == 200 && !data.isEmpty() && data.first().toObject()["is_sent"].toBool();
    emit chatSendFinished(sent);
    if (status == 200 && !sent) emit statusMessage("Twitch did not send this message. It may be blocked, held for moderation, or restricted by chat settings.");
  }});
}
void TwitchService::scheduleReconnect() {
  if (!online_ || reconnectTimer_.isActive()) return;
  ++eventGeneration_;
  emit chatStateChanged("Chat reconnecting; messages during the gap may be lost");
  emit activityStateChanged("Activity feed reconnecting; events during the gap may be lost");
  const auto rateWait = std::max<qint64>(0, blockedUntil_.value("helix") - QDateTime::currentSecsSinceEpoch()) * 1000;
  reconnectTimer_.start(int(std::max<qint64>(socketBackoff_, rateWait)));
  socketBackoff_ = std::min(socketBackoff_ * 2, 60000);
}
void TwitchService::openSocket(const QUrl &url, bool transferred) {
  if (!online_ || (transferred && replacement_)) return;
  auto *ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
  ws->setMaxAllowedIncomingMessageSize(1024 * 1024);
  ws->setMaxAllowedIncomingFrameSize(1024 * 1024);
  if (transferred) replacement_ = ws;
  else {
    if (socket_) { socket_->disconnect(this); socket_->abort(); socket_->deleteLater(); }
    socket_ = ws;
    watchdog_.start(20000);
  }
  const auto epoch = generation_;
  connect(ws, &QWebSocket::textMessageReceived, this, [this, ws, epoch, transferred](const QString &text) {
    if (epoch == generation_) handleEvent(ws, transferred, text);
  });
  connect(ws, &QWebSocket::disconnected, this, [this, ws, epoch] {
    if (epoch != generation_) return;
    if (replacement_ == ws) replacement_ = nullptr;
    if (socket_ == ws) { socket_ = nullptr; watchdog_.stop(); if (!replacement_) scheduleReconnect(); }
    else if (!socket_ && !replacement_) scheduleReconnect();
    ws->deleteLater();
  });
  connect(ws, &QWebSocket::errorOccurred, this, [this, ws, epoch](QAbstractSocket::SocketError) {
    if (epoch != generation_) return;
    emit chatStateChanged("Chat connection failed");
    if (ws == socket_ || (!socket_ && !replacement_)) scheduleReconnect();
  });
  ws->open(url);
  QTimer::singleShot(20000, ws, [this, ws, epoch] {
    if (epoch == generation_ && ws == replacement_) { replacement_ = nullptr; ws->abort(); ws->deleteLater(); }
  });
}
void TwitchService::reportActivityState() {
  QStringList active, unavailable;
  for (const auto &type : activityTypes()) {
    const auto state = activitySubscriptions_.value(type, "pending");
    const auto label = type.mid(QString("channel.").size());
    if (state == "active") active.append(label);
    else unavailable.append(label + " (" + state + ")");
  }
  QString text = active.isEmpty() ? "Activity subscriptions connecting" : "Receiving " + active.join(", ");
  if (!unavailable.isEmpty()) text += ". Unavailable: " + unavailable.join(", ");
  if (!activityAuthorized()) text += ". Authorize activity feed to enable all events.";
  emit activityStateChanged(text);
}
void TwitchService::subscribeChat(int attempt) {
  if (!online_ || !socket_ || eventSession_.isEmpty()) return;
  const auto epoch = generation_, eventEpoch = eventGeneration_;
  api({"POST", "eventsub/subscriptions", {{"type", "channel.chat.message"}, {"version", "1"},
    {"condition", QJsonObject{{"broadcaster_user_id", user_}, {"user_id", user_}}},
    {"transport", QJsonObject{{"method", "websocket"}, {"session_id", eventSession_}}}},
    [this, attempt, epoch, eventEpoch](int code, QJsonObject) {
      if (epoch != generation_ || eventEpoch != eventGeneration_) return;
      if (code == 202) { socketBackoff_ = 1000; emit chatStateChanged("Chat connected"); }
      else if (code == 429 || code == 0 || code >= 500) {
        emit chatStateChanged("Chat subscription retrying");
        const auto rateWait = std::max<qint64>(0, blockedUntil_.value("helix") - QDateTime::currentSecsSinceEpoch()) * 1000;
        const auto delay = int(std::max<qint64>(std::min(1000 * (1 << std::min(attempt, 6)), 60000), rateWait));
        QTimer::singleShot(delay, this, [this, attempt, epoch, eventEpoch] {
          if (epoch == generation_ && eventEpoch == eventGeneration_) subscribeChat(std::min(attempt + 1, 6));
        });
      } else emit chatStateChanged("Chat subscription denied. Disconnect and reconnect to retry.");
    }});
}
void TwitchService::subscribeActivity(const QString &type, int attempt) {
  if (!online_ || !socket_ || eventSession_.isEmpty()) return;
  QJsonObject condition{{"broadcaster_user_id", user_}};
  if (type == "channel.follow") condition["moderator_user_id"] = user_;
  if (type == "channel.raid") condition = {{"to_broadcaster_user_id", user_}};
  const auto session = eventSession_;
  const auto epoch = generation_;
  const auto eventEpoch = eventGeneration_;
  api({"POST", "eventsub/subscriptions", {{"type", type}, {"version", type == "channel.follow" ? "2" : "1"},
       {"condition", condition}, {"transport", QJsonObject{{"method", "websocket"}, {"session_id", session}}}},
       [this, type, attempt, epoch, eventEpoch](int code, QJsonObject) {
    if (epoch != generation_ || eventEpoch != eventGeneration_) return;
    if (code == 202) activitySubscriptions_[type] = "active";
    else if (code == 0 || code == 429 || code >= 500) {
      activitySubscriptions_[type] = "retrying";
      const auto rateWait = std::max<qint64>(0, blockedUntil_.value("helix") - QDateTime::currentSecsSinceEpoch()) * 1000;
      const auto delay = int(std::max<qint64>(std::min(1000 * (1 << std::min(attempt, 6)), 60000), rateWait));
      QTimer::singleShot(delay, this, [this, type, attempt, epoch, eventEpoch] {
        if (epoch == generation_ && eventEpoch == eventGeneration_)
          subscribeActivity(type, std::min(attempt + 1, 6));
      });
    } else activitySubscriptions_[type] = "denied";
    reportActivityState();
  }});
}
void TwitchService::handleEvent(QWebSocket *ws, bool transferred, const QString &text) {
  const auto root = QJsonDocument::fromJson(text.toUtf8()).object();
  const auto meta = root["metadata"].toObject(), payload = root["payload"].toObject();
  const auto type = meta["message_type"].toString();
  if (type.isEmpty()) return;
  const auto session = payload["session"].toObject();
  if (type == "session_welcome") {
    const auto id = session["id"].toString();
    if (id.isEmpty()) { ws->abort(); return; }
    if (ws == replacement_) {
      auto old = socket_; socket_ = ws; replacement_ = nullptr;
      if (old) { old->disconnect(this); old->close(); old->deleteLater(); }
    }
    reconnectTimer_.stop();
    watchdog_.setInterval(std::clamp(session["keepalive_timeout_seconds"].toInt(10), 10, 600) * 1000 + 1000);
    watchdog_.start();
    if (transferred) {
      socketBackoff_ = 1000;
      eventSession_ = id;
      emit chatStateChanged("Chat connected");
      reportActivityState();
      return;
    }
    eventSession_ = id;
    activitySubscriptions_.clear();
    subscribeChat();
    for (const auto &activity : activityTypes()) {
      const auto scope = activityScope(activity);
      activitySubscriptions_[activity] = !scope.isEmpty() && !scopes_.contains(scope) ? "permission needed" : "pending";
    }
    reportActivityState();
    for (const auto &activity : activityTypes())
      if (activitySubscriptions_.value(activity) == "pending") subscribeActivity(activity);
  } else if (type == "session_keepalive" || type == "notification") {
    if (ws == socket_) watchdog_.start();
    if (type != "notification") return;
    const auto subscriptionType = meta["subscription_type"].toString();
    if (subscriptionType != "channel.chat.message" && !activitySubscriptions_.contains(subscriptionType)) return;
    const auto id = meta["message_id"].toString();
    if (id.isEmpty() || seenEvents_.contains(id)) return;
    seenEvents_.insert(id); eventIds_.enqueue(id);
    while (eventIds_.size() > 1000) seenEvents_.remove(eventIds_.dequeue());
    const auto event = payload["event"].toObject();
    const auto broadcaster = subscriptionType == "channel.raid" ? event["to_broadcaster_user_id"] : event["broadcaster_user_id"];
    if (broadcaster.toString() != user_) return;
    if (subscriptionType == "channel.chat.message") {
      const auto message = event["message"].toObject();
      emit chatReceived(event["chatter_user_name"].toString().left(100),
                        message["text"].toString().left(2000), message["fragments"].toArray());
    } else {
      if (subscriptionType == "channel.subscribe" && event["is_gift"].toBool()) return;
      auto timestamp = QDateTime::fromString(meta["message_timestamp"].toString(), Qt::ISODateWithMs);
      if (!timestamp.isValid()) timestamp = QDateTime::currentDateTimeUtc();
      emit activityReceived(subscriptionType, event, timestamp);
    }
  } else if (type == "session_reconnect") {
    const QUrl url(session["reconnect_url"].toString());
    if (validReconnectUrl(url)) openSocket(url, true);
    else { emit chatStateChanged("Twitch sent an invalid chat reconnect address"); ws->abort(); }
  } else if (type == "revocation") {
    const auto reason = payload["subscription"].toObject()["status"].toString();
    if (reason == "authorization_revoked" || reason == "user_removed") { endSession(true); emit statusMessage("Twitch authorization was revoked. Connect again."); }
    else {
      const auto subscriptionType = payload["subscription"].toObject()["type"].toString();
      if (activitySubscriptions_.contains(subscriptionType)) {
        activitySubscriptions_[subscriptionType] = "denied";
        reportActivityState();
      } else emit chatStateChanged("Chat subscription revoked. Reconnect your account to retry.");
    }
  }
}
} // namespace feathercast::twitch
