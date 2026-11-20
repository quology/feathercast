#include "feathercast/ui/twitch_chat_history.hpp"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QElapsedTimer>
#include <QImageReader>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <cstring>
#include <functional>
#include <iostream>

namespace {
constexpr auto betterTtvGlobal = "https://api.betterttv.net/3/cached/emotes/global";
constexpr auto betterTtvChannel = "https://api.betterttv.net/3/cached/users/twitch/42";
constexpr auto frankerFaceZGlobal = "https://api.frankerfacez.com/v1/set/global";
constexpr auto frankerFaceZChannel = "https://api.frankerfacez.com/v1/room/id/42";
constexpr auto betterTtvImage = "https://cdn.betterttv.net/emote/54fa8ce91e468494b85b53a5/1x";
constexpr auto frankerFaceZImage = "https://cdn.frankerfacez.com/emote/9/1";
constexpr auto animatedImage = "https://cdn.frankerfacez.com/emote/81/animated/1";
constexpr auto roomImage = "https://cdn.frankerfacez.com/emote/21/1";
constexpr auto twitchImage = "https://static-cdn.jtvnw.net/emoticons/v2/62836/static/dark/1.0";
constexpr auto twitchAnimation = "https://static-cdn.jtvnw.net/emoticons/v2/62837/animated/dark/1.0";

void check(bool ok, const char *description) {
  if (!ok) { std::cerr << description << '\n'; std::exit(1); }
}

QByteArray png(QColor color) {
  QImage image(24, 24, QImage::Format_RGB32);
  image.fill(color);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  return bytes;
}

class Reply final : public QNetworkReply {
public:
  Reply(const QNetworkRequest &request, QByteArray bytes, QObject *parent)
      : QNetworkReply(parent), body_(std::move(bytes)) {
    setRequest(request);
    setUrl(request.url());
    open(QIODevice::ReadOnly);
    QTimer::singleShot(0, this, [this] {
      if (isFinished()) return;
      setAttribute(QNetworkRequest::HttpStatusCodeAttribute, body_.isEmpty() ? 404 : 200);
      emit readyRead();
      setFinished(true);
      emit finished();
    });
  }
  void abort() override {
    if (isFinished()) return;
    setError(OperationCanceledError, "aborted");
    setFinished(true);
    emit finished();
  }
  qint64 bytesAvailable() const override { return body_.size() - position_ + QNetworkReply::bytesAvailable(); }

protected:
  qint64 readData(char *data, qint64 max) override {
    const auto size = std::min(max, body_.size() - position_);
    if (size <= 0) return -1;
    std::memcpy(data, body_.constData() + position_, size);
    position_ += size;
    return size;
  }

private:
  QByteArray body_;
  qint64 position_{};
};

class Network final : public QNetworkAccessManager {
public:
  QStringList requests;
  QHash<QString, QByteArray> responses = {
    {betterTtvGlobal, R"([{"id":"54fa8ce91e468494b85b53a5","code":"BetterGlobal"},{"id":"54fa8ce91e468494b85b53a5","code":"Collision"}])"},
    {betterTtvChannel, R"({"channelEmotes":[{"id":"54fa8ce91e468494b85b53a5","code":"BetterRoom"}],"sharedEmotes":[]})"},
    {frankerFaceZGlobal, R"({"default_sets":[3],"sets":{"3":{"emoticons":[
      {"name":"FrankerGlobal","urls":{"1":"//cdn.frankerfacez.com/emote/9/1"}},
      {"name":"WrongHost","urls":{"1":"https://invalid.example/emote/9/1"}}]}}})"},
    {frankerFaceZChannel, R"({"room":{"set":12},"sets":{"12":{"emoticons":[
      {"name":"Collision","urls":{"1":"https://cdn.frankerfacez.com/emote/21/1"}},
      {"name":"Animated","animated":{"1":"https://cdn.frankerfacez.com/emote/81/animated/1"}}]}}})"},
    {betterTtvImage, png(QColor(255, 0, 255))},
    {frankerFaceZImage, png(QColor(0, 255, 0))},
    {roomImage, png(QColor(0, 255, 255))},
    {twitchImage, png(QColor(255, 32, 0))},
    // Two small solid-color frames in a looping WebP image.
    {animatedImage, QByteArray::fromBase64(
      "UklGRogAAABXRUJQVlA4WAoAAAACAAAAEwAAEwAAQU5JTQYAAAAAAAAAAABBTk1GKgAAAAAAAAAAABMAABMAAKAAAAJWUDhMEQAAAC8TwAQAB1DfRnSv/4GI6H8AAEFOTUYqAAAAAAAAAAAAEwAAEwAAoAAAAFZQOEwRAAAALxPABAAH0LKGNL7/gYjofwAA")},
    {twitchAnimation, QByteArray::fromBase64(
      "R0lGODlhFAAUAIEAABG+ewAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQAEAAAACwAAAAAFAAUAAAIIgABCBxIsKDBgwgTKlzIsKHDhxAjSpxIsaLFixgzatxoMSAAIfkEARAAAQAsAAAAABQAFACBIWXxAAAAAAAAAAAACCIAAQgcSLCgwYMIEypcyLChw4cQI0qcSLGixYsYM2rcaDEgADs=")},
  };

protected:
  QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *) override {
    check(operation == GetOperation, "emotes use anonymous read-only requests");
    check(!request.hasRawHeader("Authorization") && !request.hasRawHeader("Cookie") &&
          !request.hasRawHeader("Client-Id"), "emote requests omit Twitch credentials");
    check(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt() == QNetworkRequest::ManualRedirectPolicy,
          "emote requests block redirects");
    const auto url = request.url().toString();
    requests.append(url);
    check(responses.contains(url), "unexpected image or catalog request");
    return new Reply(request, responses[url], this);
  }
};

void until(const std::function<bool()> &ready, const char *description) {
  QElapsedTimer deadline;
  deadline.start();
  while (!ready() && deadline.elapsed() < 2000) {
    QApplication::processEvents();
    QThread::msleep(4);
  }
  check(ready(), description);
}

bool hasPixel(QWidget &widget, QColor color) {
  const auto image = widget.grab().toImage();
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x)
      if (image.pixelColor(x, y) == color) return true;
  return false;
}
} // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  Network network;
  feathercast::ui::TwitchChatHistory history(nullptr, &network);
  history.resize(700, 180);
  history.show();
  history.setAddOns(true, true);
  check(network.requests.isEmpty(), "waits for a valid channel before fetching provider catalogs");
  history.setChannel("not/a/twitch/id");
  check(network.requests.isEmpty(), "rejects invalid channel IDs");
  history.setAddOns(false, false);
  history.setChannel("42");
  history.appendMessage("<b>viewer</b>", "BetterGlobal", {});
  check(history.toPlainText() == "<b>viewer</b>: BetterGlobal" && network.requests.isEmpty(),
        "disabled providers leave plain text and make no requests");

  history.setAddOns(true, true);
  const auto original = QString("BetterRoom FrankerGlobal Collision WrongHost twitchRaid");
  const QJsonArray fragments{
      QJsonObject{{"type", "text"}, {"text", "BetterRoom FrankerGlobal Collision WrongHost "}},
      QJsonObject{{"type", "emote"}, {"text", "twitchRaid"}, {"emote", QJsonObject{{"id", "62836"}}}}};
  history.appendMessage("event", original, fragments);
  until([&] {
    return hasPixel(*history.viewport(), QColor(255, 0, 255)) &&
           hasPixel(*history.viewport(), QColor(0, 255, 0)) &&
           hasPixel(*history.viewport(), QColor(0, 255, 255)) &&
           hasPixel(*history.viewport(), QColor(255, 32, 0));
  }, "native, global, channel and channel-over-global emotes rendered");
  for (const auto url : {betterTtvGlobal, betterTtvChannel, frankerFaceZGlobal, frankerFaceZChannel,
                         betterTtvImage, frankerFaceZImage, roomImage, twitchImage})
    check(network.requests.count(url) == 1, "deduplicates catalog and image requests");
  history.selectAll();
  history.copy();
  check(QApplication::clipboard()->text() == "<b>viewer</b>: BetterGlobal\nevent: " + original,
        "copy returns original emote names and literal text");
  history.setAddOns(false, false);
  check(history.toPlainText().contains("BetterRoom FrankerGlobal Collision WrongHost"),
        "disabling providers converts existing text emotes back to codes");
  history.setChannel("");
  check(history.toPlainText().isEmpty(), "leaving channel clears its history");

  if (QImageReader::supportedImageFormats().contains("gif")) {
    history.setChannel("42");
    const QJsonArray animated{QJsonObject{{"type", "emote"}, {"text", "gifEmote"},
        {"emote", QJsonObject{{"id", "62837"}, {"format", QJsonArray{"animated", "static"}}}}}};
    history.appendMessage("event", "gifEmote", animated);
    until([&] { return hasPixel(*history.viewport(), QColor(17, 190, 123)); }, "first Twitch GIF frame rendered");
    until([&] { return hasPixel(*history.viewport(), QColor(33, 101, 241)); }, "animated Twitch GIF advanced");
    history.setChannel("");
    std::cout << "Animated GIF passed\n";
  }
  if (QImageReader::supportedImageFormats().contains("webp")) {
    history.setAddOns(false, true);
    history.setChannel("42");
    history.appendMessage("event", "Animated", {});
    until([&] { return hasPixel(*history.viewport(), QColor(17, 190, 123)); }, "first FrankerFaceZ WebP frame rendered");
    until([&] { return hasPixel(*history.viewport(), QColor(33, 101, 241)); }, "animated FrankerFaceZ WebP advanced");
    std::cout << "Animated WebP passed\n";
  }

  std::cout << "Native Twitch, BetterTTV and FrankerFaceZ emotes rendered with isolated bounded requests\n";
}
