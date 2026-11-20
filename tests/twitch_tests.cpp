#include "feathercast/twitch/twitch_service.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QWebSocket>
#include <QWebSocketServer>
#include <iostream>
using namespace feathercast::twitch;
static bool waitFor(const std::function<bool()> &predicate, int timeout = 3000) {
  QElapsedTimer elapsed; elapsed.start();
  while (!predicate() && elapsed.elapsed() < timeout) { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); QThread::msleep(1); }
  return predicate();
}
static void check(bool condition, const char *message) {
  if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}
struct Mock {
  QTcpServer http;
  QWebSocketServer events{"mock", QWebSocketServer::NonSecureMode};
  QPointer<QWebSocket> ws;
  int refreshes{}, validates{}, subscriptions{}, sends{}, updates{}, streams{}, searches{};
  int refreshStatus{200}, chatSubFailures{};
  bool declineDevice{}, fullScopes{}, legacySavedToken{}, wrongDeviceAccount{}, failFollow{}, expire{}, missingScope{}, delayStream{}, rateChat{}, pendingDevice{}, deviceIssued{}, live{}, streamError{};
  QByteArray lastForm, lastSearch;
  QJsonObject lastUpdate;
  QHash<QString, QJsonObject> lastSubscriptions;
  Mock() {
    check(http.listen(QHostAddress::LocalHost), "mock HTTP listen"); check(events.listen(QHostAddress::LocalHost), "mock WS listen");
    QObject::connect(&http, &QTcpServer::newConnection, &http, [this] {
      while (http.hasPendingConnections()) {
        auto *socket = http.nextPendingConnection(); auto input = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, input] {
          *input += socket->readAll(); const int end = input->indexOf("\r\n\r\n"); if (end < 0) return;
          int size = 0; for (auto line : input->left(end).split('\n')) if (line.toLower().startsWith("content-length:")) size = line.mid(15).trimmed().toInt();
          if (input->size() < end + 4 + size) return;
          auto line = input->left(input->indexOf("\r\n")).split(' '); auto path = line.value(1); auto body = input->mid(end + 4, size);
          int status = 200; QJsonObject data; int delay = 0;
          if (path == "/oauth/validate") {
            ++validates;
            if (input->contains("Bearer old")) status = 401;
            else {
              const bool wrongAccount = wrongDeviceAccount && !input->contains("Bearer valid");
              const bool expanded = fullScopes && !(legacySavedToken && input->contains("Bearer valid"));
              data = {{"client_id", "public123"}, {"user_id", wrongAccount ? "99" : "42"}, {"login", "tester"},
                      {"scopes", missingScope ? QJsonArray{} : QJsonArray::fromStringList(expanded ? authorizationScopes() : requiredScopes())}};
            }
          } else if (path == "/oauth/device") {
            deviceIssued = true; lastForm = body;
            data = {{"device_code", "fake-device"}, {"user_code", "FAKE"}, {"verification_uri", "https://www.twitch.tv/activate"}, {"interval", 1}, {"expires_in", 30}};
          } else if (path == "/oauth/token") {
            lastForm = body;
            if (body.contains("device_code")) {
              if (declineDevice) { status = 403; data = {{"message", "access_denied"}}; }
              else if (!pendingDevice) { pendingDevice = true; status = 400; data = {{"message", "authorization_pending"}}; }
              else data = {{"access_token", "new"}, {"refresh_token", "rotate+%&"}};
            } else { ++refreshes; delay = 30; status = refreshStatus; data = {{"access_token", "new" + QString::number(refreshes)}, {"refresh_token", "rotate+%&"}}; }
          } else if (path.startsWith("/helix/streams")) {
            ++streams; if (delayStream) delay = 150;
            if (expire && input->contains("Bearer new1")) status = 401;
            else if (streamError) status = 503;
            else data = {{"data", live ? QJsonArray{QJsonObject{{"viewer_count", 123}, {"started_at", "2026-01-01T12:00:00Z"}}} : QJsonArray{}}};
          } else if (path.startsWith("/helix/channels")) {
            if (line[0] == "PATCH") { ++updates; lastUpdate = QJsonDocument::fromJson(body).object(); status = 204; }
            else if (expire && input->contains("Bearer new1")) status = 401;
            else data = {{"data", QJsonArray{QJsonObject{{"title", "Original"}, {"game_id", "1"}, {"game_name", "One"}, {"tags", QJsonArray{"English"}}}}}};
          } else if (path.startsWith("/helix/search/categories")) { ++searches; lastSearch = path; data = {{"data", QJsonArray{QJsonObject{{"name", "Example"}, {"id", "2"}}}}};
          } else if (path == "/helix/eventsub/subscriptions") {
            ++subscriptions; auto subscription = QJsonDocument::fromJson(body).object();
            lastSubscriptions[subscription["type"].toString()] = subscription;
            status = failFollow && subscription["type"].toString() == "channel.follow" ? 403 : 202;
            if (subscription["type"] == "channel.chat.message" && chatSubFailures > 0) { --chatSubFailures; status = 503; }
            data = {{"data", QJsonArray{}}};
          }
          else if (path == "/helix/chat/messages") { ++sends; status = rateChat ? 429 : 200; data = {{"data", QJsonArray{QJsonObject{{"is_sent", true}}}}}; }
          else status = 404;
          auto response = QByteArray("HTTP/1.1 ") + QByteArray::number(status) + " Mock\r\nContent-Type: application/json\r\nConnection: close\r\n";
          if (status == 429) response += "Ratelimit-Remaining: 0\r\nRatelimit-Reset: " + QByteArray::number(QDateTime::currentSecsSinceEpoch() + 60) + "\r\n";
          const auto bytes = status == 204 ? QByteArray{} : QJsonDocument(data).toJson(QJsonDocument::Compact);
          response += "Content-Length: " + QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes;
          socket->disconnect(&http); socket->disconnect(socket);
          QTimer::singleShot(delay, socket, [socket, response] { socket->write(response); socket->disconnectFromHost(); });
        });
      }
    });
    QObject::connect(&events, &QWebSocketServer::newConnection, &events, [this] {
      ws = events.nextPendingConnection();
      ws->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{{"metadata", QJsonObject{{"message_type", "session_welcome"}}}, {"payload", QJsonObject{{"session", QJsonObject{{"id", "test-session"}, {"keepalive_timeout_seconds", 10}}}}}}).toJson()));
    });
  }
  Endpoints endpoints() const {
    return {QUrl(QString("http://127.0.0.1:%1/oauth/").arg(http.serverPort())), QUrl(QString("http://127.0.0.1:%1/helix/").arg(http.serverPort())), QUrl(QString("ws://127.0.0.1:%1").arg(events.serverPort()))};
  }
};
static void seed(const QString &dir, const QString &access = "old") {
  auto plain = QJsonDocument(QJsonObject{{"client_id", "public123"}, {"access_token", access}, {"refresh_token", "rotate+%&"}}).toJson();
  auto encrypted = protectTokens(plain); check(!encrypted.isEmpty() && !encrypted.contains("rotate+%&"), "DPAPI encrypted");
  check(unprotectTokens(encrypted) == plain, "DPAPI roundtrip");
  QFile file(dir + "/twitch.tokens"); check(file.open(QIODevice::WriteOnly), "token fixture open"); file.write(encrypted);
}
int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  check(validMetadata("Title", {"English", "Deutsch"}), "valid metadata");
  check(!validMetadata(QString(141, 'x'), {}) && !validMetadata("Title", {"two words"}) && !validMetadata("Title", {"English", "english"}) && !validMetadata("Title", QStringList(11, "x")), "metadata boundaries");
  check(validReconnectUrl(QUrl("wss://eventsub.wss.twitch.tv/ws?opaque=1")) && !validReconnectUrl(QUrl("wss://eventsub.wss.twitch.tv.evil.invalid/")) && !validReconnectUrl(QUrl("ws://eventsub.wss.twitch.tv")) && !validReconnectUrl(QUrl("wss://user@eventsub.wss.twitch.tv")), "reconnect URL guard");
  QTemporaryDir directory; check(directory.isValid(), "test directory"); seed(directory.path());
  Mock mock; TwitchService service(directory.path(), nullptr, mock.endpoints());
  int offline = 0, chats = 0, sent = 0, saved = 0, category = 0, disconnected = 0, live = 0, unavailable = 0;
  QObject::connect(&service, &TwitchService::streamReceived, &service, [&](const QJsonObject &o) { if (o.isEmpty()) ++offline; else if (o["viewer_count"].toInt() == 123 && !o["started_at"].toString().isEmpty()) ++live; });
  QObject::connect(&service, &TwitchService::streamUnavailable, &service, [&] { ++unavailable; });
  QObject::connect(&service, &TwitchService::chatReceived, &service, [&](const QString &, const QString &, const QJsonArray &) { ++chats; });
  QObject::connect(&service, &TwitchService::chatSendFinished, &service, [&](bool result) { if (result) ++sent; });
  QObject::connect(&service, &TwitchService::channelUpdateFinished, &service, [&](bool result) { if (result) ++saved; });
  QObject::connect(&service, &TwitchService::categoriesReceived, &service, [&](const QJsonArray &) { ++category; });
  QObject::connect(&service, &TwitchService::connectionChanged, &service, [&](bool state, const QString &) { if (!state) ++disconnected; });
  service.restore(); check(waitFor([&] { return service.connected() && mock.subscriptions == 2 && offline == 1; }), "restore refresh validation offline subscription");
  check(mock.refreshes == 1 && mock.lastForm.contains("refresh_token=rotate%2B%25%26"), "single use opaque refresh encoded");
  TwitchService second(directory.path(), nullptr, mock.endpoints()); second.restore(); check(!second.connected(), "profile session lock");
  auto notification = QString::fromUtf8(QJsonDocument(QJsonObject{{"metadata", QJsonObject{{"message_type", "notification"}, {"subscription_type", "channel.chat.message"}, {"message_id", "dedup"}}}, {"payload", QJsonObject{{"event", QJsonObject{{"broadcaster_user_id", "42"}, {"chatter_user_name", "Someone"}, {"message", QJsonObject{{"text", "<b>plain chat</b>"}}}}}}}}).toJson());
  mock.ws->sendTextMessage(notification); mock.ws->sendTextMessage(notification); check(waitFor([&] { return chats == 1; }), "EventSub notification and duplicate suppression");
  service.sendChat("Fake local test"); service.updateChannel("Updated", "2", {"English"}); service.searchCategories("A&B + C");
  check(waitFor([&] { return sent == 1 && saved == 1 && category == 1; }), "explicit chat metadata category requests");
  check(mock.lastUpdate["title"].toString() == "Updated" && mock.lastUpdate["tags"].toArray().size() == 1 && mock.lastSearch.contains("A%26B%20%2B%20C"), "request method body URL encoding");
  mock.expire = true; const int previous = mock.refreshes; service.pollStatus();
  check(waitFor([&] { return mock.refreshes == previous + 1 && offline >= 3; }), "concurrent 401 refresh and replay");
  check(mock.refreshes == 2 && service.connected(), "one refresh for concurrent expired requests");
  mock.live = true; service.pollStatus(); check(waitFor([&] { return live == 1; }), "live viewers and start timestamp");
  mock.streamError = true; service.pollStatus(); check(waitFor([&] { return unavailable == 1; }), "stream error marked unavailable"); mock.streamError = false;
  mock.rateChat = true; service.sendChat("Rate test"); check(waitFor([&] { return mock.sends == 2; }), "chat 429 received");
  waitFor([&] { return false; }, 30); service.sendChat("Blocked locally"); service.searchCategories("General bucket still works");
  check(waitFor([&] { return mock.searches == 2; }) && mock.sends == 2, "separate chat and Helix rate buckets");
  mock.delayStream = true; service.pollStatus(); const auto before = offline; service.disconnectAccount();
  waitFor([&] { return false; }, 200); check(!service.connected() && offline == before && !QFile::exists(directory.filePath("twitch.tokens")), "disconnect aborts stale callbacks and removes tokens");
  seed(directory.path(), "valid"); mock.missingScope = true; service.restore(); check(waitFor([&] { return disconnected >= 2; }) && !service.connected() && !QFile::exists(directory.filePath("twitch.tokens")), "missing scopes rejected");
  mock.missingScope = false; service.connectAccount("public123"); check(waitFor([&] { return mock.deviceIssued; }), "device authorization request");
  check(mock.lastForm.contains("scopes=channel%3Amanage%3Abroadcast%20user%3Aread%3Achat%20user%3Awrite%3Achat%20moderator%3Aread%3Afollowers%20channel%3Aread%3Asubscriptions%20bits%3Aread"), "public device scopes no secret");
  check(waitFor([&] { return service.connected(); }, 4000) && mock.pendingDevice, "device pending polling and success");
  service.disconnectAccount();
  {
    QTemporaryDir retryDirectory; seed(retryDirectory.path()); Mock retryMock; retryMock.refreshStatus = 429;
    TwitchService retry(retryDirectory.path(), nullptr, retryMock.endpoints()); bool failed = false;
    QObject::connect(&retry, &TwitchService::statusMessage, &retry, [&](const QString &message) { if (message.contains("could not be renewed")) failed = true; });
    retry.restore(); check(waitFor([&] { return failed; }), "transient refresh failure finishes");
    check(!retry.connected() && QFile::exists(retryDirectory.filePath("twitch.tokens")), "transient refresh preserves encrypted credentials");
    retry.restore(); waitFor([&] { return false; }, 30); check(retryMock.refreshes == 1, "OAuth 429 cooldown prevents refresh storm");
  }
  {
    QTemporaryDir revokedDirectory; seed(revokedDirectory.path()); Mock revokedMock; revokedMock.refreshStatus = 400;
    TwitchService revoked(revokedDirectory.path(), nullptr, revokedMock.endpoints()); bool failed = false;
    QObject::connect(&revoked, &TwitchService::statusMessage, &revoked, [&](const QString &message) { if (message.contains("expired or were revoked")) failed = true; });
    revoked.restore(); check(waitFor([&] { return failed; }) && !QFile::exists(revokedDirectory.filePath("twitch.tokens")), "permanent refresh failure removes credentials");
  }
  {
    QTemporaryDir activityDirectory; seed(activityDirectory.path(), "valid");
    Mock activityMock; activityMock.fullScopes = true; activityMock.failFollow = true; activityMock.chatSubFailures = 1;
    TwitchService activity(activityDirectory.path(), nullptr, activityMock.endpoints());
    QString activityState, chatState; int received = 0; QJsonArray fragments;
    QObject::connect(&activity, &TwitchService::activityStateChanged, &activity, [&](const QString &text) { activityState = text; });
    QObject::connect(&activity, &TwitchService::chatStateChanged, &activity, [&](const QString &text) { chatState = text; });
    QObject::connect(&activity, &TwitchService::activityReceived, &activity, [&](const QString &, const QJsonObject &, const QDateTime &time) { check(time.isValid(), "event timestamp"); ++received; });
    QObject::connect(&activity, &TwitchService::chatReceived, &activity, [&](const QString &, const QString &, const QJsonArray &parts) { fragments = parts; });
    activity.restore();
    check(waitFor([&] { return activityMock.subscriptions == 7 && activityState.contains("follow (denied)") && activityState.contains("Receiving"); }), "independent activity subscription denial");
    const auto originalSocket = activityMock.ws;
    check(waitFor([&] { return chatState == "Chat connected" && activityMock.subscriptions == 8; }) &&
          activity.connected() && activity.activityAuthorized() && originalSocket == activityMock.ws, "chat retries without interrupting activity socket");
    check(activityMock.lastSubscriptions["channel.follow"]["version"] == "2" &&
          activityMock.lastSubscriptions["channel.follow"]["condition"].toObject()["moderator_user_id"] == "42" &&
          activityMock.lastSubscriptions["channel.raid"]["condition"].toObject()["to_broadcaster_user_id"] == "42", "official subscription versions and conditions");
    int notificationId = 0;
    auto notify = [&](const QString &type, QJsonObject event, bool duplicate = false) {
      const auto message = QString::fromUtf8(QJsonDocument(QJsonObject{
        {"metadata", QJsonObject{{"message_type", "notification"}, {"subscription_type", type},
          {"message_id", QString::number(++notificationId)}, {"message_timestamp", "2026-11-19T12:00:00.000Z"}}},
        {"payload", QJsonObject{{"event", event}}}}).toJson());
      activityMock.ws->sendTextMessage(message);
      if (duplicate) activityMock.ws->sendTextMessage(message);
    };
    for (const auto &type : {"channel.subscribe", "channel.subscription.message", "channel.subscription.gift", "channel.cheer"})
      notify(type, {{"broadcaster_user_id", "42"}, {"user_name", "Viewer"}}, true);
    notify("channel.raid", {{"to_broadcaster_user_id", "42"}, {"from_broadcaster_user_name", "Raider"}});
    check(waitFor([&] { return received == 5; }), "activity types and cross-type duplicate suppression");
    notify("channel.subscribe", {{"broadcaster_user_id", "another"}});
    notify("channel.subscribe", {{"broadcaster_user_id", "42"}, {"is_gift", true}});
    notify("channel.chat.message", {{"broadcaster_user_id", "42"}, {"chatter_user_name", "Viewer"},
      {"message", QJsonObject{{"text", "Kappa"}, {"fragments", QJsonArray{QJsonObject{{"type", "emote"}, {"text", "Kappa"}, {"emote", QJsonObject{{"id", "25"}}}}}}}}});
    check(waitFor([&] { return fragments.size() == 1; }) && received == 5, "fragment delivery and activity channel gift guards");
    const auto revocation = QString::fromUtf8(QJsonDocument(QJsonObject{
      {"metadata", QJsonObject{{"message_type", "revocation"}}},
      {"payload", QJsonObject{{"subscription", QJsonObject{{"type", "channel.cheer"}, {"status", "version_removed"}}}}}}).toJson());
    activityMock.ws->sendTextMessage(revocation);
    check(waitFor([&] { return activityState.contains("cheer (denied)"); }) && activity.connected() && chatState == "Chat connected", "single activity revocation preserves chat");
    activity.disconnectAccount();
  }
  {
    QTemporaryDir upgradeDirectory; seed(upgradeDirectory.path(), "valid");
    Mock upgradeMock;
    upgradeMock.legacySavedToken = true;
    TwitchService upgrade(upgradeDirectory.path(), nullptr, upgradeMock.endpoints());
    upgrade.restore();
    check(waitFor([&] { return upgrade.connected(); }) && !upgrade.activityAuthorized(), "legacy account remains connected");
    QFile vault(upgradeDirectory.filePath("twitch.tokens")); check(vault.open(QIODevice::ReadOnly), "read legacy vault");
    const auto previousVault = vault.readAll(); vault.close();
    upgradeMock.declineDevice = true; upgrade.authorizeActivity();
    check(waitFor([&] { return upgrade.connected() && upgradeMock.deviceIssued; }, 4000), "declined activity authorization restores prior connection");
    check(vault.open(QIODevice::ReadOnly) && vault.readAll() == previousVault, "decline retains original encrypted vault"); vault.close();
    upgradeMock.declineDevice = false; upgradeMock.pendingDevice = true;
    upgrade.authorizeActivity();
    check(waitFor([&] { return upgrade.connected(); }, 4000) && !upgrade.activityAuthorized(), "incomplete scope upgrade restores prior account");
    check(vault.open(QIODevice::ReadOnly) && vault.readAll() == previousVault, "invalid scope upgrade retains original vault"); vault.close();
    upgradeMock.fullScopes = true; upgradeMock.wrongDeviceAccount = true;
    upgrade.authorizeActivity();
    check(waitFor([&] { return upgrade.connected(); }, 4000) && upgrade.userId() == "42" && !upgrade.activityAuthorized(),
          "different Twitch account cannot replace the current account during activity consent");
    check(vault.open(QIODevice::ReadOnly) && vault.readAll() == previousVault, "different account retains original vault"); vault.close();
    upgradeMock.wrongDeviceAccount = false; upgrade.authorizeActivity();
    check(waitFor([&] { return upgrade.connected() && upgrade.activityAuthorized(); }, 4000), "successful activity scope upgrade");
    check(vault.open(QIODevice::ReadOnly) && vault.readAll() != previousVault, "successful upgrade commits encrypted token rotation"); vault.close();
    upgrade.disconnectAccount();
  }
  std::cout << "Twitch DPAPI, device flow, validation, refresh single-flight, Helix, EventSub dedup, buckets, disconnect and scopes passed\n";
}
