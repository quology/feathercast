#include "feathercast/ui/twitch_chat_history.hpp"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QCache>
#include <QDeadlineTimer>
#include <QFontMetricsF>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QMovie>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPainter>
#include <QPointer>
#include <QQueue>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSet>
#include <QTimer>
#include <deque>
#include <functional>

namespace feathercast::ui {
namespace {
constexpr int historyLimit = 500;
constexpr int emoteObject = QTextFormat::UserObject + 1;
constexpr int emoteUrl = QTextFormat::UserProperty + 1;
constexpr int emoteCode = QTextFormat::UserProperty + 2;
constexpr int imageLimit = 512 * 1024;
constexpr int catalogLimit = 2 * 1024 * 1024;
constexpr int animationLimit = 8;

QString singleLine(QString text) {
  static const QRegularExpression blockBreaks(R"([\r\n\v\f\x{2028}\x{2029}\x{fffc}])");
  return text.replace(blockBreaks, " ");
}

using Catalog = QHash<QString, QUrl>;

void addBetterTtv(Catalog &catalog, const QJsonArray &emotes) {
  static const QRegularExpression idPattern("^[a-f0-9]{24}$");
  for (const auto &value : emotes) {
    const auto emote = value.toObject();
    const QString id = emote["id"].toString();
    const QString code = emote["code"].toString();
    if (!code.isEmpty() && code.size() <= 100 && idPattern.match(id).hasMatch())
      catalog.insert(code, QUrl("https://cdn.betterttv.net/emote/" + id + "/1x"));
  }
}

void addFrankerFaceZ(Catalog &catalog, const QJsonObject &set) {
  static const QRegularExpression pathPattern("^/emote/[0-9]+/animated/[1-4]$|^/emote/[0-9]+/[1-4]$");
  static const bool canDecodeWebP = QImageReader::supportedImageFormats().contains("webp");
  for (const auto &value : set["emoticons"].toArray()) {
    const auto emote = value.toObject();
    const auto code = emote["name"].toString();
    if (code.isEmpty() || code.size() > 100) continue;
    auto source = canDecodeWebP ? emote["animated"].toObject()["1"].toString() : QString{};
    if (source.isEmpty()) source = emote["urls"].toObject()["1"].toString();
    if (source.startsWith("//")) source.prepend("https:");
    const QUrl url(source, QUrl::StrictMode);
    if (url.isValid() && url.scheme() == "https" && url.authority() == "cdn.frankerfacez.com" &&
        !url.hasQuery() && !url.hasFragment() && pathPattern.match(url.path(QUrl::FullyEncoded)).hasMatch())
      catalog.insert(code, url);
  }
}

QUrl nativeEmote(const QJsonObject &fragment) {
  static const QRegularExpression idPattern("^[a-zA-Z0-9_]{1,100}$");
  const auto emote = fragment["emote"].toObject();
  const auto id = emote["id"].toString();
  if (!idPattern.match(id).hasMatch()) return {};
  const auto format = emote["format"].toArray().contains("animated") ? "animated" : "static";
  return QUrl("https://static-cdn.jtvnw.net/emoticons/v2/" + id + "/" + format + "/dark/1.0");
}

struct ChatLine { QString name, text; QJsonArray fragments; };
struct EmoteAsset {
  QImage image;
  std::unique_ptr<QMovie> movie;
  QDeadlineTimer retryAfter;
};

class EmoteRenderer final : public QObject, public QTextObjectInterface {
  Q_OBJECT
  Q_INTERFACES(QTextObjectInterface)
public:
  using Lookup = std::function<QImage(const QUrl &)>;
  EmoteRenderer(TwitchChatHistory *history, Lookup lookup)
    : QObject(history->document()), history_(history), lookup_(std::move(lookup)) {
    history->document()->documentLayout()->registerHandler(emoteObject, this);
  }

  QSizeF intrinsicSize(QTextDocument *, int, const QTextFormat &format) override {
    const QFontMetricsF metrics(history_->font());
    const auto image = lookup_(format.property(emoteUrl).toUrl());
    if (image.isNull())
      return {metrics.horizontalAdvance(format.stringProperty(emoteCode)), metrics.height()};
    return image.size().scaled(54, 24, Qt::KeepAspectRatio);
  }

  void drawObject(QPainter *painter, const QRectF &rect, QTextDocument *, int, const QTextFormat &format) override {
    const auto image = lookup_(format.property(emoteUrl).toUrl());
    if (!image.isNull()) {
      painter->drawImage(rect, image);
      return;
    }
    painter->save();
    painter->setPen(history_->palette().color(QPalette::Text));
    painter->setFont(history_->font());
    painter->drawText(rect, Qt::AlignLeft | Qt::AlignVCenter, format.stringProperty(emoteCode));
    painter->restore();
  }

private:
  TwitchChatHistory *history_;
  Lookup lookup_;
};
} // namespace

struct TwitchChatHistory::State : QObject {
  State(TwitchChatHistory *history, QNetworkAccessManager *manager)
    : history(history), network(manager ? manager : new QNetworkAccessManager(this)), assets(256) {
    new EmoteRenderer(history, [this](const QUrl &url) {
      if (const auto *asset = assets.object(url)) return asset->image;
      return QImage{};
    });
  }

  TwitchChatHistory *history;
  QNetworkAccessManager *network;
  QString channel;
  bool betterTtv{}, frankerFaceZ{};
  int generation{};
  Catalog betterTtvGlobal, betterTtvChannel, frankerFaceZGlobal, frankerFaceZChannel;
  std::deque<ChatLine> lines;
  QCache<QUrl, EmoteAsset> assets;
  QSet<QNetworkReply *> requests;
  QSet<QUrl> loading;
  QQueue<QUrl> imageQueue;
  int imageRequests{};
  QList<QPointer<QMovie>> movies;

  void fetch(const QUrl &url, int limit, std::function<void(const QByteArray &)> complete) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setTransferTimeout(5000);
    auto *reply = network->get(request);
    requests.insert(reply);
    reply->setReadBufferSize(limit + 1);
    auto bytes = std::make_shared<QByteArray>();
    const auto capture = [reply, bytes, limit] {
      bytes->append(reply->read(limit + 1 - bytes->size()));
      if (bytes->size() > limit) reply->abort();
    };
    connect(reply, &QNetworkReply::readyRead, this, capture);
    QTimer::singleShot(10000, reply, [reply] { if (!reply->isFinished()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, limit, revision = generation, complete = std::move(complete)] {
      requests.remove(reply);
      reply->deleteLater();
      if (revision != generation) return;
      const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      if (http == 200 && reply->error() == QNetworkReply::NoError)
        bytes->append(reply->read(limit + 1 - bytes->size()));
      complete(http == 200 && reply->error() == QNetworkReply::NoError && bytes->size() <= limit ? *bytes : QByteArray{});
    });
  }

  void reloadCatalogs() {
    ++generation;
    const auto oldRequests = requests;
    for (auto *request : oldRequests) request->abort();
    for (auto *catalog : {&betterTtvGlobal, &betterTtvChannel, &frankerFaceZGlobal, &frankerFaceZChannel}) catalog->clear();
    loading.clear();
    imageQueue.clear();
    imageRequests = 0;
    assets.clear();
    movies.clear();
    rebuild();

    if (channel.isEmpty()) return;
    if (betterTtv) {
      fetch(QUrl("https://api.betterttv.net/3/cached/emotes/global"), catalogLimit, [this](const QByteArray &bytes) {
        addBetterTtv(betterTtvGlobal, QJsonDocument::fromJson(bytes).array());
        if (!betterTtvGlobal.isEmpty()) rebuild();
      });
      fetch(QUrl("https://api.betterttv.net/3/cached/users/twitch/" + channel), catalogLimit, [this](const QByteArray &bytes) {
        const auto root = QJsonDocument::fromJson(bytes).object();
        addBetterTtv(betterTtvChannel, root["sharedEmotes"].toArray());
        addBetterTtv(betterTtvChannel, root["channelEmotes"].toArray());
        if (!betterTtvChannel.isEmpty()) rebuild();
      });
    }
    if (frankerFaceZ) {
      fetch(QUrl("https://api.frankerfacez.com/v1/set/global"), catalogLimit, [this](const QByteArray &bytes) {
        const auto root = QJsonDocument::fromJson(bytes).object();
        const auto sets = root["sets"].toObject();
        for (const auto &id : root["default_sets"].toArray())
          addFrankerFaceZ(frankerFaceZGlobal, sets[QString::number(id.toInt())].toObject());
        if (!frankerFaceZGlobal.isEmpty()) rebuild();
      });
      fetch(QUrl("https://api.frankerfacez.com/v1/room/id/" + channel), catalogLimit, [this](const QByteArray &bytes) {
        const auto root = QJsonDocument::fromJson(bytes).object();
        const auto set = QString::number(root["room"].toObject()["set"].toInt());
        addFrankerFaceZ(frankerFaceZChannel, root["sets"].toObject()[set].toObject());
        if (!frankerFaceZChannel.isEmpty()) rebuild();
      });
    }
  }

  void loadImage(const QUrl &url) {
    const auto *cached = assets.object(url);
    if (cached && (!cached->image.isNull() || !cached->retryAfter.hasExpired())) return;
    if (loading.contains(url) || loading.size() >= 96) return;
    loading.insert(url);
    imageQueue.enqueue(url);
    pumpImages();
  }

  void pumpImages() {
    while (!imageQueue.isEmpty() && imageRequests < 6) {
      ++imageRequests;
      const auto url = imageQueue.dequeue();
      fetch(url, imageLimit, [this, url](const QByteArray &bytes) {
        --imageRequests;
        loading.remove(url);
        cacheImage(url, bytes);
        pumpImages();
      });
    }
  }

  void cacheImage(const QUrl &url, const QByteArray &bytes) {
    auto asset = std::make_unique<EmoteAsset>();
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    const auto format = QImageReader::imageFormat(&buffer);
    if (format == "png" || format == "gif" || format == "webp") {
      QImageReader reader(&buffer, format);
      const auto size = reader.size();
      if (size.isValid() && size.width() <= 256 && size.height() <= 256) {
        asset->image = reader.read();
        movies.removeIf([](const auto &movie) { return movie.isNull(); });
        if (!asset->image.isNull() && reader.supportsAnimation() && movies.size() < animationLimit) {
          asset->movie = std::make_unique<QMovie>();
          auto *movie = asset->movie.get();
          auto *device = new QBuffer(movie);
          device->setData(bytes);
          device->open(QIODevice::ReadOnly);
          movie->setDevice(device);
          movie->setFormat(format);
          movies.append(movie);
          connect(movie, &QMovie::frameChanged, this, [this, movie, url] {
            if (auto *current = assets.object(url)) current->image = movie->currentImage();
            history->viewport()->update();
          });
        }
      }
    }
    const bool loaded = !asset->image.isNull();
    if (!loaded) asset->retryAfter = QDeadlineTimer(60000);
    auto *movie = asset->movie.get();
    assets.insert(url, asset.release());
    if (loaded) history->document()->markContentsDirty(0, history->document()->characterCount());
    if (movie && history->isVisible()) movie->start();
  }

  void insertEmote(QTextCursor &cursor, const QString &word, const QUrl &url) {
    if (url.isEmpty()) {
      cursor.insertText(word, QTextCharFormat{});
      return;
    }
    QTextCharFormat format;
    format.setObjectType(emoteObject);
    format.setVerticalAlignment(QTextCharFormat::AlignMiddle);
    format.setProperty(emoteCode, word);
    format.setProperty(emoteUrl, url);
    format.setToolTip(word);
    cursor.insertText(QString(QChar::ObjectReplacementCharacter), format);
    loadImage(url);
  }

  void insertWords(QTextCursor &cursor, const QString &text) {
    if (!betterTtv && !frankerFaceZ) { cursor.insertText(text, QTextCharFormat{}); return; }
    static const QRegularExpression words(R"(\S+|\s+)");
    auto parts = words.globalMatch(text);
    while (parts.hasNext()) {
      const auto word = parts.next().captured();
      QUrl url;
      for (const auto *catalog : {&betterTtvChannel, &frankerFaceZChannel, &betterTtvGlobal, &frankerFaceZGlobal}) {
        const auto found = catalog->constFind(word);
        if (found != catalog->cend()) { url = *found; break; }
      }
      insertEmote(cursor, word, url);
    }
  }

  void insertLine(QTextCursor &cursor, const ChatLine &line) {
    if (!history->document()->isEmpty()) cursor.insertBlock();
    QTextCharFormat name;
    name.setFontWeight(QFont::DemiBold);
    cursor.insertText(line.name + ": ", name);
    QString combined;
    for (const auto &fragment : line.fragments) combined += fragment.toObject()["text"].toString();
    if (line.fragments.isEmpty() || singleLine(combined) != line.text) {
      insertWords(cursor, line.text);
      return;
    }
    for (const auto &value : line.fragments) {
      const auto fragment = value.toObject();
      auto text = singleLine(fragment["text"].toString());
      const auto type = fragment["type"].toString();
      if (type == "emote") insertEmote(cursor, text, nativeEmote(fragment));
      else if (type == "text") insertWords(cursor, text);
      else cursor.insertText(text, QTextCharFormat{});
    }
  }

  void rebuild() {
    auto *scroll = history->verticalScrollBar();
    const auto position = scroll->value();
    const bool atEnd = position == scroll->maximum();
    history->QTextEdit::clear();
    QTextCursor cursor(history->document());
    cursor.beginEditBlock();
    for (const auto &line : lines) insertLine(cursor, line);
    cursor.endEditBlock();
    scroll->setValue(atEnd ? scroll->maximum() : position);
  }

  void setAnimating(bool visible) {
    for (const auto &movie : movies) {
      if (!movie) continue;
      if (movie->state() == QMovie::Paused && visible) movie->setPaused(false);
      else if (movie->state() == QMovie::Running && !visible) movie->setPaused(true);
      else if (movie->state() == QMovie::NotRunning && visible) movie->start();
    }
  }
};

TwitchChatHistory::TwitchChatHistory(QWidget *parent, QNetworkAccessManager *network)
  : QTextEdit(parent), state_(std::make_unique<State>(this, network)) {
  setReadOnly(true);
  setAcceptRichText(false);
  document()->setMaximumBlockCount(historyLimit);
}
TwitchChatHistory::~TwitchChatHistory() {
  document()->documentLayout()->unregisterHandler(emoteObject);
}

void TwitchChatHistory::setChannel(const QString &id) {
  static const QRegularExpression pattern("^[0-9]{1,32}$");
  const QString valid = pattern.match(id).hasMatch() ? id : QString{};
  if (state_->channel == valid) return;
  state_->channel = valid;
  clear();
  state_->reloadCatalogs();
}

void TwitchChatHistory::setAddOns(bool betterTtv, bool frankerFaceZ) {
  if (state_->betterTtv == betterTtv && state_->frankerFaceZ == frankerFaceZ) return;
  state_->betterTtv = betterTtv;
  state_->frankerFaceZ = frankerFaceZ;
  state_->reloadCatalogs();
}

void TwitchChatHistory::appendMessage(const QString &name, const QString &text, const QJsonArray &fragments) {
  auto *scroll = verticalScrollBar();
  const bool atEnd = scroll->value() == scroll->maximum();
  state_->lines.push_back({singleLine(name.left(100)), singleLine(text.left(2000)), fragments.size() <= 500 ? fragments : QJsonArray{}});
  if (state_->lines.size() > historyLimit) state_->lines.pop_front();
  QTextCursor cursor(document());
  cursor.movePosition(QTextCursor::End);
  cursor.beginEditBlock();
  state_->insertLine(cursor, state_->lines.back());
  cursor.endEditBlock();
  if (atEnd) scroll->setValue(scroll->maximum());
}

void TwitchChatHistory::clear() {
  state_->lines.clear();
  QTextEdit::clear();
}

QMimeData *TwitchChatHistory::createMimeDataFromSelection() const {
  auto cursor = textCursor();
  const int start = cursor.selectionStart();
  auto text = cursor.selectedText();
  for (auto i = text.size(); i-- > 0;) {
    if (text[i] != QChar::ObjectReplacementCharacter) continue;
    cursor.setPosition(start + i + 1);
    const auto code = cursor.charFormat().stringProperty(emoteCode);
    text.replace(i, 1, code);
  }
  auto *mime = new QMimeData;
  mime->setText(text.replace(QChar::ParagraphSeparator, '\n'));
  return mime;
}

void TwitchChatHistory::showEvent(QShowEvent *event) {
  QTextEdit::showEvent(event);
  state_->setAnimating(true);
}
void TwitchChatHistory::hideEvent(QHideEvent *event) {
  state_->setAnimating(false);
  QTextEdit::hideEvent(event);
}
} // namespace feathercast::ui

#include "twitch_chat_history.moc"
