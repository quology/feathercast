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
QStringList activityScopes() { return {"moderator:read:followers", "channel:read:subscriptions", "bits:read", "channel:read:goals"}; }
static const QMap<QString, QString> activityTypes{
  {"channel.follow", "moderator:read:followers"}, {"channel.subscribe", "channel:read:subscriptions"},
  {"channel.subscription.message", "channel:read:subscriptions"}, {"channel.subscription.gift", "channel:read:subscriptions"},
  {"channel.cheer", "bits:read"}, {"channel.raid", ""}, {"channel.goal.begin", "channel:read:goals"},
  {"channel.goal.progress", "channel:read:goals"}, {"channel.goal.end", "channel:read:goals"}};
static QString plain(QString value, int limit = 500) { value = value.left(limit); value.replace('\r', ' '); value.replace('\n', ' '); return value; }
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
TwitchService::~TwitchService() { endSession(false); }
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
  ++generation_; online_ = false; refreshing_ = false; validating_ = false; devicePending_ = false;
  for (auto *timer : {&deviceTimer_, &validateTimer_, &statusTimer_, &watchdog_, &reconnectTimer_}) timer->stop();
  for (auto *socket : {socket_.data(), replacement_.data()}) if (socket) {
    socket->disconnect(this); socket->abort(); socket->deleteLater();
  }
  socket_ = nullptr; replacement_ = nullptr;
  auto replies = replies_; replies_.clear();
  for (auto *reply : replies) reply->abort();
  refreshQueue_.clear(); blockedUntil_.clear(); seenEvents_.clear(); eventIds_.clear();
  scopes_.clear(); revokedTypes_.clear(); statistics_ = {}; goals_.clear(); pendingStats_.clear();
  emit statisticsReceived({}); emit goalsReceived({});
  access_.clear(); refresh_.clear(); user_.clear(); login_.clear(); device_.clear();
  if (erase && sessionLock_.isLocked() && QFile::exists(path_) && !QFile::remove(path_))
    emit statusMessage("Could not remove saved Twitch credentials. Remove twitch.tokens from your profile before restarting.");
  sessionLock_.unlock();
  emit connectionChanged(false, {});
  emit chatStateChanged("Chat disconnected");
}
void TwitchService::disconnectAccount() {
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
  if (access.isEmpty() || refresh.isEmpty()) { endSession(true); emit statusMessage("Twitch returned incomplete credentials. Connect again."); return false; }
  access_ = access; refresh_ = refresh;
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
  if (authRetryAfter_ > QDateTime::currentSecsSinceEpoch()) { emit statusMessage("Twitch authentication rate limit reached. Wait before connecting again."); return; }
  endSession(true);
  QDir().mkpath(QFileInfo(path_).absolutePath());
  if (!sessionLock_.tryLock(0)) { emit statusMessage("Another VELA instance is using this Twitch profile. Close it before connecting."); return; }
  client_ = id.trimmed();
  if (!QRegularExpression("^[A-Za-z0-9]{5,128}$").match(client_).hasMatch()) { sessionLock_.unlock(); emit statusMessage("Enter a valid public Twitch Client ID."); return; }
  devicePending_ = true;
  emit statusMessage("Requesting Twitch authorization…");
  oauth("device", {{"client_id", client_}, {"scopes", (requiredScopes() + activityScopes()).join(' ')}}, [this](int status, QJsonObject data) {
    const QUrl url(data["verification_uri"].toString());
    if (status != 200 || data["device_code"].toString().isEmpty() || data["user_code"].toString().isEmpty() ||
        url.scheme() != "https" || url.host() != "www.twitch.tv" || !url.userInfo().isEmpty() || data["expires_in"].toInt() <= 0) {
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
  if (QDateTime::currentDateTimeUtc() >= deviceExpiry_) { devicePending_ = false; device_.clear(); sessionLock_.unlock(); emit deviceAuthorization({}, {}); emit statusMessage("Twitch authorization expired. Connect again."); return; }
  oauth("token", {{"client_id", client_}, {"device_code", device_}, {"scopes", (requiredScopes() + activityScopes()).join(' ')},
      {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}}, [this](int status, QJsonObject data) {
    if (status == 200) { devicePending_ = false; device_.clear(); if (acceptTokens(data)) validate(); return; }
    const auto error = data["error"].toString(data["message"].toString());
    if (error == "slow_down" || status == 429) deviceInterval_ = std::min(deviceInterval_ + 5000, 60000);
    else if (status != 0 && status < 500 && error != "authorization_pending") {
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
    const auto scopes = data["scopes"].toArray();
    for (const auto &scope : requiredScopes()) valid = valid && scopes.contains(scope);
    if (!valid) {
      const bool transient = status == 0 || status == 429 || status >= 500;
      endSession(!transient);
      emit statusMessage(transient ? "Twitch validation unavailable. Use Connect to retry saved credentials." : "Twitch authorization is invalid or lacks required permissions. Connect again.");
      return;
    }
    scopes_.clear(); for (const auto &scope : scopes) scopes_.insert(scope.toString());
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
    if (work.optional && ((status == 401 && work.retried) || status == 403)) { if (work.done) work.done(status, data); return; }
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
  pollStatistics();
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
  emit chatStateChanged("Chat reconnecting; messages during the gap may be lost");
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
    if (transferred) { socketBackoff_ = 1000; emit chatStateChanged("Chat connected"); return; }
    revokedTypes_.clear();
    subscribeActivity(id, ws);
    api({"POST", "eventsub/subscriptions", {{"type", "channel.chat.message"}, {"version", "1"},
      {"condition", QJsonObject{{"broadcaster_user_id", user_}, {"user_id", user_}}},
      {"transport", QJsonObject{{"method", "websocket"}, {"session_id", id}}}},
      [this, ws = QPointer<QWebSocket>(ws)](int code, QJsonObject) {
        if (!ws || ws != socket_) return;
        if (code == 202) { socketBackoff_ = 1000; emit chatStateChanged("Chat connected"); }
        else if (code == 429 || code == 0 || code >= 500) { emit chatStateChanged("Chat subscription failed; reconnecting"); ws->abort(); }
        else { ws->disconnect(this); ws->abort(); ws->deleteLater(); socket_ = nullptr; watchdog_.stop(); emit chatStateChanged("Chat subscription denied. Disconnect and reconnect to retry."); }
      }});
  } else if (type == "session_keepalive" || type == "notification") {
    if (ws == socket_) watchdog_.start();
    if (type != "notification") return;
    const auto subscriptionType = meta["subscription_type"].toString();
    if (subscriptionType != "channel.chat.message" && !activityTypes.contains(subscriptionType)) return;
    const auto id = meta["message_id"].toString();
    if (id.isEmpty() || id.size() > 200 || seenEvents_.contains(id)) return;
    seenEvents_.insert(id); eventIds_.enqueue(id);
    while (eventIds_.size() > 1000) seenEvents_.remove(eventIds_.dequeue());
    const auto event = payload["event"].toObject();
    if (subscriptionType != "channel.chat.message") { receiveActivity(subscriptionType, event, meta); return; }
    if (event["broadcaster_user_id"].toString() != user_) return;
    emit chatReceived(event["chatter_user_name"].toString().left(100), event["message"].toObject()["text"].toString().left(2000));
  } else if (type == "session_reconnect") {
    const QUrl url(session["reconnect_url"].toString());
    if (validReconnectUrl(url)) openSocket(url, true);
    else { emit chatStateChanged("Twitch sent an invalid chat reconnect address"); ws->abort(); }
  } else if (type == "revocation") {
    const auto subscription = payload["subscription"].toObject();
    const auto revokedType = subscription["type"].toString();
    if (revokedType != "channel.chat.message") {
      if (activityTypes.contains(revokedType)) {
        revokedTypes_.insert(revokedType);
        emit activityStateChanged(revokedType, "Revoked; reauthorize activity permissions to retry");
        if (revokedType.startsWith("channel.goal.")) { ++goalRevision_; goals_.clear(); publishGoals(); statistics_["goals_status"] = "Unavailable; reauthorize activity permissions"; }
        if (revokedType.startsWith("channel.subscri")) { statistics_.remove("subscribers"); statistics_.remove("points"); statistics_["subscribers_status"] = "Unavailable; reauthorize activity permissions"; }
        emit statisticsReceived(statistics_);
      }
      return;
    }
    const auto reason = subscription["status"].toString();
    if (reason == "authorization_revoked" || reason == "user_removed") { endSession(true); emit statusMessage("Twitch authorization was revoked. Connect again."); }
    else { ws->disconnect(this); ws->abort(); watchdog_.stop(); emit chatStateChanged("Chat subscription revoked. Reconnect your account to retry."); }
  }
}

void TwitchService::publishGoals() {
  QJsonArray values; for (auto it = goals_.cbegin(); it != goals_.cend(); ++it) values.append(it.value());
  emit goalsReceived(values);
}
void TwitchService::pollStatistics() {
  struct Request { QString key, scope, path; };
  const QList<Request> requests{{"followers", "", "channels/followers"},
      {"subscribers", "channel:read:subscriptions", "subscriptions"}, {"goals", "channel:read:goals", "goals"}};
  for (const auto &request : requests) {
    const auto key = request.key;
    if (!request.scope.isEmpty() && !scopes_.contains(request.scope)) {
      statistics_.remove(key); if (key == "subscribers") statistics_.remove("points");
      statistics_[key + "_status"] = "Unavailable; reauthorize activity permissions"; continue;
    }
    if (pendingStats_.contains(key)) continue;
    pendingStats_.insert(key);
    const auto revision = goalRevision_;
    api({"GET", request.path + "?broadcaster_id=" + user_, {}, [this, key, revision](int code, QJsonObject data) {
      pendingStats_.remove(key);
      const bool valid = code == 200 && (key == "goals" ? data["data"].isArray() :
        data["total"].isDouble() && data["total"].toDouble(-1) >= 0 &&
        (key != "subscribers" || (data["points"].isDouble() && data["points"].toDouble(-1) >= 0)));
      statistics_[key + "_status"] = valid ? "Available" : "Unavailable; check permissions or retry later";
      if (key == "goals") {
        if (revision != goalRevision_) { emit statisticsReceived(statistics_); return; }
        goals_.clear();
        if (valid) for (const auto &value : data["data"].toArray()) {
          const auto goal = value.toObject(); const auto id = goal["id"].toString().left(100);
          if (id.isEmpty() || goal["broadcaster_id"].toString() != user_ || goals_.size() >= 100) continue;
          goals_.insert(id, {{"id", id}, {"type", plain(goal["type"].toString(), 50)}, {"description", plain(goal["description"].toString())},
            {"current_amount", goal["current_amount"]}, {"target_amount", goal["target_amount"]}});
        }
        publishGoals();
      } else {
        statistics_.remove(key); if (key == "subscribers") statistics_.remove("points");
        if (valid) { statistics_[key] = data["total"]; if (key == "subscribers") statistics_["points"] = data["points"]; }
      }
      emit statisticsReceived(statistics_);
    }, false, true});
  }
  emit statisticsReceived(statistics_);
}
void TwitchService::subscribeActivity(const QString &session, QWebSocket *ws) {
  for (auto it = activityTypes.cbegin(); it != activityTypes.cend(); ++it) {
    const auto type = it.key();
    if (!it.value().isEmpty() && !scopes_.contains(it.value())) {
      emit activityStateChanged(type, "Unavailable; reauthorize activity permissions"); continue;
    }
    QJsonObject condition{{"broadcaster_user_id", user_}};
    if (type == "channel.follow") condition["moderator_user_id"] = user_;
    if (type == "channel.raid") condition = {{"to_broadcaster_user_id", user_}};
    api({"POST", "eventsub/subscriptions", {{"type", type}, {"version", type == "channel.follow" ? "2" : "1"},
      {"condition", condition}, {"transport", QJsonObject{{"method", "websocket"}, {"session_id", session}}}},
      [this, type, ws = QPointer<QWebSocket>(ws)](int code, QJsonObject) {
        if (!ws || ws != socket_) return;
        emit activityStateChanged(type, code == 202 ? "Receiving live events" : "Unavailable; subscription rejected. Reconnect to retry");
      }, false, true});
  }
}
void TwitchService::receiveActivity(const QString &type, const QJsonObject &event, const QJsonObject &metadata) {
  if (event[type == "channel.raid" ? "to_broadcaster_user_id" : "broadcaster_user_id"].toString() != user_) return;
  if (revokedTypes_.contains(type)) return;
  const auto scope = activityTypes.value(type);
  if (!scope.isEmpty() && !scopes_.contains(scope)) return;
  const auto name = plain(event["user_name"].toString(), 100);
  QString text;
  if (type == "channel.follow") text = name + " followed";
  else if (type == "channel.subscribe") text = name + (event["is_gift"].toBool() ? " received a gift subscription" : " subscribed") + " (tier " + plain(event["tier"].toString(), 10) + ")";
  else if (type == "channel.subscription.message") text = name + " resubscribed for " + QString::number(event["cumulative_months"].toInt()) + " months: " + plain(event["message"].toObject()["text"].toString());
  else if (type == "channel.subscription.gift") text = (event["is_anonymous"].toBool() ? "Anonymous" : name) + " gifted " + QString::number(event["total"].toInt()) + " subscriptions";
  else if (type == "channel.cheer") text = (event["is_anonymous"].toBool() ? "Anonymous" : name) + " cheered " + QString::number(event["bits"].toInt()) + " bits: " + plain(event["message"].toString());
  else if (type == "channel.raid") text = plain(event["from_broadcaster_user_name"].toString(), 100) + " raided with " + QString::number(event["viewers"].toInt()) + " viewers";
  else if (type.startsWith("channel.goal.")) {
    const auto id = event["id"].toString().left(100); if (id.isEmpty()) return;
    ++goalRevision_;
    QJsonObject goal{{"id", id}, {"type", plain(event["type"].toString(), 50)}, {"description", plain(event["description"].toString())},
      {"current_amount", event["current_amount"]}, {"target_amount", event["target_amount"]}};
    if (type == "channel.goal.end") goals_.remove(id);
    else if (goals_.contains(id) || goals_.size() < 100) goals_.insert(id, goal);
    publishGoals();
    text = (type == "channel.goal.begin" ? QString("Goal started") : type == "channel.goal.end" ? QString("Goal ended") : QString("Goal progress")) + ": " + goal["description"].toString() + " (" + QString::number(event["current_amount"].toInt()) + "/" + QString::number(event["target_amount"].toInt()) + ")";
  }
  if (!text.isEmpty()) emit activityReceived({{"id", metadata["message_id"].toString().left(200)}, {"type", type},
    {"timestamp", metadata["message_timestamp"].toString().left(50)}, {"text", text.left(1000)}});
}
} // namespace feathercast::twitch
