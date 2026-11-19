#include "feathercast/ui/twitch_docks.hpp"
#include "feathercast/twitch/twitch_service.hpp"
#include <QtWidgets>
#include <QJsonArray>

namespace feathercast::ui {
void installTwitchDocks(QMainWindow *window, const QString &directory) {
  auto *service = new twitch::TwitchService(directory, window);
  auto *info = new QWidget;
  auto *layout = new QVBoxLayout(info);
  auto *client = new QLineEdit;
  client->setObjectName("twitchClientId");
  client->setAccessibleName("Public Twitch Client ID");
  client->setPlaceholderText("Public Twitch Client ID");
  QSettings settings(directory + "/ui.ini", QSettings::IniFormat);
  client->setText(settings.value("twitch/clientId").toString());
  layout->addWidget(client);
  auto *actions = new QHBoxLayout;
  auto *connectButton = new QPushButton("Connect Twitch");
  auto *disconnectButton = new QPushButton("Disconnect");
  actions->addWidget(connectButton); actions->addWidget(disconnectButton); layout->addLayout(actions);
  auto *account = new QLabel("No Twitch account connected");
  auto *feedback = new QLabel;
  auto *authorization = new QLabel;
  for (auto *l : {account, feedback, authorization}) { l->setWordWrap(true); l->setTextFormat(Qt::PlainText); layout->addWidget(l); }
  authorization->setTextInteractionFlags(Qt::TextSelectableByMouse);
  auto *openAuth = new QPushButton("Open Twitch authorization");
  openAuth->hide(); layout->addWidget(openAuth);
  auto authUrl = std::make_shared<QUrl>();
  QObject::connect(openAuth, &QPushButton::clicked, window, [authUrl] { QDesktopServices::openUrl(*authUrl); });
  QObject::connect(service, &twitch::TwitchService::deviceAuthorization, window,
      [authorization, openAuth, authUrl](const QUrl &url, const QString &code) {
    if (url.isEmpty()) { authorization->clear(); openAuth->hide(); *authUrl = QUrl{}; return; }
    *authUrl = url; authorization->setText("Authorize in your browser. Twitch code: " + code); openAuth->show();
  });
  QObject::connect(connectButton, &QPushButton::clicked, window, [service, client, directory, authorization, openAuth] {
    authorization->clear(); openAuth->hide();
    QSettings settings(directory + "/ui.ini", QSettings::IniFormat);
    settings.setValue("twitch/clientId", client->text().trimmed());
    if (QFile::exists(directory + "/twitch.tokens") && client->text().trimmed() == service->clientId()) service->restore();
    else service->connectAccount(client->text());
  });
  QObject::connect(disconnectButton, &QPushButton::clicked, service, &twitch::TwitchService::disconnectAccount);
  QObject::connect(service, &twitch::TwitchService::statusMessage, feedback, &QLabel::setText);
  auto *live = new QLabel("Twitch status unavailable"); live->setTextFormat(Qt::PlainText); live->setWordWrap(true); layout->addWidget(live);
  auto started = std::make_shared<QDateTime>();
  auto viewers = std::make_shared<int>(0);
  auto isLive = std::make_shared<bool>(false);
  auto *clock = new QTimer(info); clock->setInterval(1000);
  auto updateLive = [live, started, viewers, isLive] {
    if (!*isLive) { live->setText("Offline · 0 viewers · uptime —"); return; }
    const auto seconds = std::max<qint64>(0, started->secsTo(QDateTime::currentDateTimeUtc()));
    live->setText(QString("LIVE · %1 viewers · uptime %2:%3:%4").arg(*viewers)
      .arg(seconds / 3600, 2, 10, QChar('0')).arg(seconds / 60 % 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0')));
  };
  QObject::connect(clock, &QTimer::timeout, info, updateLive);
  QObject::connect(service, &twitch::TwitchService::streamUnavailable, info, [clock, live] { clock->stop(); live->setText("Twitch status unavailable · waiting for next update"); });
  QObject::connect(service, &twitch::TwitchService::streamReceived, info, [started, viewers, isLive, clock, updateLive](const QJsonObject &o) {
    *isLive = !o.isEmpty(); *viewers = o["viewer_count"].toInt(); *started = QDateTime::fromString(o["started_at"].toString(), Qt::ISODate);
    updateLive(); clock->start();
  });
  auto *editor = new QWidget; auto *form = new QFormLayout(editor); form->setContentsMargins(0, 0, 0, 0);
  auto *title = new QLineEdit; title->setObjectName("twitchTitle"); title->setAccessibleName("Twitch stream title");
  auto *searchRow = new QWidget; auto *searchLayout = new QHBoxLayout(searchRow); searchLayout->setContentsMargins(0, 0, 0, 0);
  auto *query = new QLineEdit; query->setAccessibleName("Twitch category search"); query->setPlaceholderText("Search category");
  auto *search = new QPushButton("Search"); searchLayout->addWidget(query); searchLayout->addWidget(search);
  auto *categories = new QComboBox; categories->setAccessibleName("Twitch category"); categories->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  auto *tags = new QLineEdit; tags->setObjectName("twitchTags"); tags->setAccessibleName("Twitch tags, separated by commas"); tags->setPlaceholderText("Up to 10 tags, comma separated");
  form->addRow("Title", title); form->addRow("Category", searchRow); form->addRow("Selection", categories); form->addRow("Tags", tags);
  layout->addWidget(editor);
  auto *save = new QPushButton("Save channel information"); layout->addWidget(save);
  auto *note = new QLabel("Changes are sent only when you click Save. Twitch status updates every 30 seconds. Connecting does not start a broadcast.");
  note->setWordWrap(true); layout->addWidget(note); layout->addStretch();
  auto dirty = std::make_shared<bool>(false);
  QObject::connect(title, &QLineEdit::textEdited, info, [dirty] { *dirty = true; });
  QObject::connect(tags, &QLineEdit::textEdited, info, [dirty] { *dirty = true; });
  QObject::connect(categories, &QComboBox::activated, info, [dirty] { *dirty = true; });
  QObject::connect(search, &QPushButton::clicked, info, [service, query] { service->searchCategories(query->text()); });
  QObject::connect(query, &QLineEdit::returnPressed, search, &QPushButton::click);
  QObject::connect(service, &twitch::TwitchService::categoriesReceived, info, [categories](const QJsonArray &results) {
    if (results.isEmpty()) return;
    const auto currentName = categories->currentText(); const auto currentId = categories->currentData();
    categories->clear();
    categories->addItem(currentName.isEmpty() ? "No category" : currentName, currentId.isValid() ? currentId : QVariant("0"));
    if (currentId.toString() != "0") categories->addItem("No category", "0");
    for (const auto &item : results) { const auto o = item.toObject(); categories->addItem(o["name"].toString(), o["id"].toString()); }
  });
  QObject::connect(service, &twitch::TwitchService::channelReceived, info, [title, tags, categories, dirty](const QJsonObject &o) {
    if (*dirty) return;
    title->setText(o["title"].toString()); QStringList ts;
    for (const auto &tag : o["tags"].toArray()) ts.append(tag.toString());
    tags->setText(ts.join(", ")); categories->clear(); categories->addItem(o["game_name"].toString(), o["game_id"].toString());
  });
  QObject::connect(save, &QPushButton::clicked, info, [service, title, tags, categories, save, editor] {
    QStringList ts;
    if (!tags->text().trimmed().isEmpty()) for (const auto &tag : tags->text().split(',')) ts.append(tag.trimmed());
    save->setEnabled(false); editor->setEnabled(false); service->updateChannel(title->text(), categories->currentData().toString(), ts);
  });
  QObject::connect(service, &twitch::TwitchService::channelUpdateFinished, info, [save, editor, dirty, service](bool saved) {
    save->setEnabled(service->connected()); editor->setEnabled(service->connected()); if (saved) *dirty = false;
  });
  auto *infoDock = new QDockWidget("Twitch channel", window); infoDock->setObjectName("Twitch channel"); infoDock->setWidget(info);
  window->addDockWidget(Qt::RightDockWidgetArea, infoDock);

  auto *chat = new QWidget; auto *chatLayout = new QVBoxLayout(chat);
  auto *chatState = new QLabel("Chat disconnected"); chatState->setTextFormat(Qt::PlainText); chatState->setWordWrap(true); chatLayout->addWidget(chatState);
  auto *chatFeedback = new QLabel; chatFeedback->setTextFormat(Qt::PlainText); chatFeedback->setWordWrap(true); chatLayout->addWidget(chatFeedback);
  QObject::connect(service, &twitch::TwitchService::statusMessage, chatFeedback, &QLabel::setText);
  auto *messages = new QPlainTextEdit; messages->setObjectName("twitchChatHistory"); messages->setAccessibleName("Twitch chat history");
  messages->setReadOnly(true); messages->setMaximumBlockCount(500); chatLayout->addWidget(messages, 1);
  auto *compose = new QLineEdit; compose->setObjectName("twitchChatMessage"); compose->setAccessibleName("Twitch chat message"); compose->setPlaceholderText("Message your Twitch chat");
  auto *send = new QPushButton("Send"); auto *composeRow = new QHBoxLayout; composeRow->addWidget(compose, 1); composeRow->addWidget(send); chatLayout->addLayout(composeRow);
  QObject::connect(service, &twitch::TwitchService::chatStateChanged, chatState, &QLabel::setText);
  QObject::connect(service, &twitch::TwitchService::chatReceived, messages, [messages](const QString &name, QString text) {
    text.replace('\n', ' '); text.replace('\r', ' '); messages->appendPlainText(name + ": " + text);
  });
  QObject::connect(send, &QPushButton::clicked, chat, [service, compose, send] { send->setEnabled(false); compose->setEnabled(false); service->sendChat(compose->text()); });
  QObject::connect(compose, &QLineEdit::returnPressed, send, &QPushButton::click);
  QObject::connect(service, &twitch::TwitchService::chatSendFinished, chat, [compose, send, service, chatFeedback](bool sent) {
    if (sent) { compose->clear(); chatFeedback->setText("Message sent"); }
    compose->setEnabled(service->connected()); send->setEnabled(service->connected());
    if (sent) compose->setFocus();
  });
  auto *chatDock = new QDockWidget("Twitch chat", window); chatDock->setObjectName("Twitch chat"); chatDock->setWidget(chat);
  window->addDockWidget(Qt::RightDockWidgetArea, chatDock);
  window->tabifyDockWidget(infoDock, chatDock); infoDock->raise();
  QObject::connect(service, &twitch::TwitchService::connectionChanged, info,
      [=](bool connected, const QString &name) {
    account->setText(connected ? "Connected as " + name : "No Twitch account connected");
    connectButton->setEnabled(!connected); client->setEnabled(!connected);
    editor->setEnabled(connected); save->setEnabled(connected); compose->setEnabled(connected); send->setEnabled(connected);
    authorization->clear(); openAuth->hide();
    if (!connected) { *dirty = false; title->clear(); tags->clear(); categories->clear(); messages->clear(); compose->clear(); clock->stop(); live->setText("Twitch status unavailable"); }
  });
  editor->setEnabled(false); save->setEnabled(false); compose->setEnabled(false); send->setEnabled(false);
  // New optional docks start hidden; View menu and persisted main-window state control them.
  infoDock->hide(); chatDock->hide();
  QTimer::singleShot(0, service, &twitch::TwitchService::restore);
}
} // namespace feathercast::ui
