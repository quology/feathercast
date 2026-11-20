#include "feathercast/ui/twitch_docks.hpp"
#include "feathercast/ui/twitch_chat_history.hpp"
#include "feathercast/twitch/twitch_service.hpp"
#include <QtWidgets>
#include <QJsonArray>
#include <algorithm>

namespace feathercast::ui {
namespace {
QString activityDetail(const QString &type, const QJsonObject &event) {
  QString name = event["user_name"].toString().left(100);
  if (name.isEmpty() || event["is_anonymous"].toBool()) name = "Anonymous";
  if (type == "channel.follow") return name + " followed";
  if (type == "channel.subscribe") return name + " subscribed";
  if (type == "channel.subscription.message")
    return name + QString(" resubscribed for %1 months").arg(event["cumulative_months"].toInt());
  if (type == "channel.subscription.gift")
    return name + QString(" gifted %1 subscriptions").arg(event["total"].toInt());
  if (type == "channel.cheer")
    return name + QString(" cheered %1 Bits").arg(event["bits"].toInt());
  return event["from_broadcaster_user_name"].toString().left(100) +
      QString(" raided with %1 viewers").arg(event["viewers"].toInt());
}
QString activityLabel(const QString &type) {
  if (type == "channel.follow") return "Follow";
  if (type == "channel.subscribe") return "Subscription";
  if (type == "channel.subscription.message") return "Resubscription";
  if (type == "channel.subscription.gift") return "Gift";
  if (type == "channel.cheer") return "Cheer";
  return "Raid";
}
}

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
  auto *stats = new QWidget;
  auto *statsLayout = new QVBoxLayout(stats);
  auto *metrics = new QFormLayout;
  auto *live = new QLabel("Disconnected");
  auto *viewers = new QLabel("—");
  auto *uptime = new QLabel("—");
  auto *lastUpdated = new QLabel("—");
  live->setObjectName("twitchStreamStatus");
  viewers->setObjectName("twitchViewerCount");
  uptime->setObjectName("twitchUptime");
  for (auto *metric : {live, viewers, uptime, lastUpdated}) metric->setTextFormat(Qt::PlainText);
  metrics->addRow("Status", live);
  metrics->addRow("Viewers", viewers);
  metrics->addRow("Uptime", uptime);
  metrics->addRow("Last updated", lastUpdated);
  statsLayout->addLayout(metrics);
  auto *refreshStats = new QPushButton("Refresh");
  statsLayout->addWidget(refreshStats); statsLayout->addStretch();
  QObject::connect(refreshStats, &QPushButton::clicked, service, &twitch::TwitchService::pollStatus);
  auto *statsDock = new QDockWidget("Twitch Stats", window);
  statsDock->setObjectName("Twitch stats"); statsDock->setWidget(stats);
  window->addDockWidget(Qt::RightDockWidgetArea, statsDock);
  auto started = std::make_shared<QDateTime>();
  auto *clock = new QTimer(stats);
  clock->setInterval(1000);
  const auto updateUptime = [uptime, started] {
    const auto seconds = std::max<qint64>(0, started->secsTo(QDateTime::currentDateTimeUtc()));
    uptime->setText(QString("%1:%2:%3").arg(seconds / 3600, 2, 10, QChar('0'))
        .arg(seconds / 60 % 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0')));
  };
  const auto resetStats = [=](const QString &state) {
    clock->stop();
    live->setText(state);
    viewers->setText("—");
    uptime->setText("—");
    lastUpdated->setText("—");
  };
  QObject::connect(clock, &QTimer::timeout, stats, updateUptime);
  QObject::connect(service, &twitch::TwitchService::streamUnavailable, stats, [resetStats] { resetStats("Unavailable"); });
  QObject::connect(service, &twitch::TwitchService::streamReceived, stats, [=](const QJsonObject &stream) {
    const bool isLive = !stream.isEmpty();
    live->setText(isLive ? "Live" : "Offline");
    viewers->setText(QLocale().toString(stream["viewer_count"].toInt()));
    lastUpdated->setText(QLocale().toString(QTime::currentTime(), QLocale::ShortFormat));
    *started = QDateTime::fromString(stream["started_at"].toString(), Qt::ISODate);
    if (isLive && started->isValid()) {
      updateUptime();
      clock->start();
    } else {
      clock->stop();
      uptime->setText("—");
    }
  });
  auto *editor = new QWidget; auto *form = new QFormLayout(editor); form->setContentsMargins(0, 0, 0, 0);
  auto *title = new QLineEdit; title->setObjectName("twitchTitle"); title->setAccessibleName("Twitch stream title");
  auto *searchRow = new QWidget; auto *searchLayout = new QHBoxLayout(searchRow); searchLayout->setContentsMargins(0, 0, 0, 0);
  auto *query = new QLineEdit; query->setAccessibleName("Twitch category search"); query->setPlaceholderText("Search category");
  auto *search = new QPushButton("Search"); searchLayout->addWidget(query); searchLayout->addWidget(search);
  auto *categories = new QComboBox; categories->setObjectName("twitchCategory"); categories->setAccessibleName("Twitch category"); categories->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  auto *tags = new QLineEdit; tags->setObjectName("twitchTags"); tags->setAccessibleName("Twitch tags, separated by commas"); tags->setPlaceholderText("Up to 10 tags, comma separated");
  form->addRow("Title", title); form->addRow("Category", searchRow); form->addRow("Selection", categories); form->addRow("Tags", tags);
  layout->addWidget(editor);
  auto *save = new QPushButton("Save channel information"); layout->addWidget(save);
  layout->addStretch();
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
  auto *infoDock = new QDockWidget("Stream Information", window); infoDock->setObjectName("Twitch channel"); infoDock->setWidget(info);
  window->addDockWidget(Qt::RightDockWidgetArea, infoDock);

  auto *chat = new QWidget; auto *chatLayout = new QVBoxLayout(chat);
  auto *chatState = new QLabel("Chat disconnected"); chatState->setTextFormat(Qt::PlainText); chatState->setWordWrap(true); chatLayout->addWidget(chatState);
  auto *chatFeedback = new QLabel; chatFeedback->setTextFormat(Qt::PlainText); chatFeedback->setWordWrap(true); chatLayout->addWidget(chatFeedback);
  QObject::connect(service, &twitch::TwitchService::statusMessage, chatFeedback, &QLabel::setText);
  auto *addOns = new QComboBox;
  addOns->setObjectName("twitchChatAddOns");
  addOns->setAccessibleName("Twitch chat add-ons");
  addOns->addItems({"None", "BetterTTV", "FrankerFaceZ", "BetterTTV and FrankerFaceZ"});
  addOns->setCurrentIndex(std::clamp(settings.value("twitch/chatAddOns", 0).toInt(), 0, 3));
  auto *addOnRow = new QFormLayout; addOnRow->addRow("Chat add-ons", addOns); chatLayout->addLayout(addOnRow);
  auto *messages = new TwitchChatHistory; messages->setObjectName("twitchChatHistory"); messages->setAccessibleName("Twitch chat history");
  chatLayout->addWidget(messages, 1);
  messages->setAddOns(addOns->currentIndex() & 1, addOns->currentIndex() & 2);
  QObject::connect(addOns, &QComboBox::currentIndexChanged, chat, [messages, directory](int index) {
    QSettings settings(directory + "/ui.ini", QSettings::IniFormat);
    settings.setValue("twitch/chatAddOns", index);
    messages->setAddOns(index & 1, index & 2);
  });
  auto *compose = new QLineEdit; compose->setObjectName("twitchChatMessage"); compose->setAccessibleName("Twitch chat message"); compose->setPlaceholderText("Message your Twitch chat");
  auto *send = new QPushButton("Send"); auto *composeRow = new QHBoxLayout; composeRow->addWidget(compose, 1); composeRow->addWidget(send); chatLayout->addLayout(composeRow);
  QObject::connect(service, &twitch::TwitchService::chatStateChanged, chatState, &QLabel::setText);
  QObject::connect(service, &twitch::TwitchService::chatReceived, messages, &TwitchChatHistory::appendMessage);
  QObject::connect(send, &QPushButton::clicked, chat, [service, compose, send] { send->setEnabled(false); compose->setEnabled(false); service->sendChat(compose->text()); });
  QObject::connect(compose, &QLineEdit::returnPressed, send, &QPushButton::click);
  QObject::connect(service, &twitch::TwitchService::chatSendFinished, chat, [compose, send, service, chatFeedback](bool sent) {
    if (sent) { compose->clear(); chatFeedback->setText("Message sent"); }
    compose->setEnabled(service->connected()); send->setEnabled(service->connected());
    if (sent) compose->setFocus();
  });
  auto *chatDock = new QDockWidget("Twitch Chat", window); chatDock->setObjectName("Twitch chat"); chatDock->setWidget(chat);
  window->addDockWidget(Qt::RightDockWidgetArea, chatDock);
  window->tabifyDockWidget(infoDock, statsDock);
  window->tabifyDockWidget(infoDock, chatDock);
  auto *activity = new QWidget;
  auto *activityLayout = new QVBoxLayout(activity);
  auto *activityState = new QLabel("Activity feed disconnected");
  activityState->setObjectName("twitchActivityState"); activityState->setTextFormat(Qt::PlainText); activityState->setWordWrap(true);
  activityLayout->addWidget(activityState);
  auto *authorizeActivity = new QPushButton("Authorize activity feed");
  activityLayout->addWidget(authorizeActivity); authorizeActivity->hide();
  QObject::connect(authorizeActivity, &QPushButton::clicked, infoDock, [infoDock, service] {
    infoDock->show();
    infoDock->raise();
    service->authorizeActivity();
  });
  QObject::connect(service, &twitch::TwitchService::activityStateChanged, activityState, &QLabel::setText);
  auto *events = new QTreeWidget;
  events->setObjectName("twitchActivityHistory"); events->setAccessibleName("Twitch activity feed");
  events->setColumnCount(3); events->setHeaderLabels({"Time", "Event", "Details"});
  events->setRootIsDecorated(false); events->setAlternatingRowColors(true);
  events->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  events->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  events->header()->setSectionResizeMode(2, QHeaderView::Stretch);
  activityLayout->addWidget(events, 1);
  auto *clearEvents = new QPushButton("Clear"); activityLayout->addWidget(clearEvents);
  QObject::connect(clearEvents, &QPushButton::clicked, events, &QTreeWidget::clear);
  QObject::connect(service, &twitch::TwitchService::activityReceived, events,
      [events](const QString &type, const QJsonObject &event, const QDateTime &timestamp) {
    auto *item = new QTreeWidgetItem({timestamp.toLocalTime().toString("HH:mm:ss"), activityLabel(type), activityDetail(type, event)});
    item->setToolTip(2, item->text(2)); events->insertTopLevelItem(0, item);
    while (events->topLevelItemCount() > 200) delete events->takeTopLevelItem(events->topLevelItemCount() - 1);
  });
  auto *activityDock = new QDockWidget("Twitch Activity Feed", window);
  activityDock->setObjectName("Twitch activity"); activityDock->setWidget(activity);
  window->addDockWidget(Qt::RightDockWidgetArea, activityDock);
  window->tabifyDockWidget(infoDock, activityDock); infoDock->raise();
  QObject::connect(service, &twitch::TwitchService::connectionChanged, info,
      [=](bool connected, const QString &name) {
    messages->setChannel(connected ? service->userId() : QString{});
    refreshStats->setEnabled(connected);
    resetStats(connected ? "Checking Twitch…" : "Disconnected");
    authorizeActivity->setVisible(connected && !service->activityAuthorized());
    account->setText(connected ? "Connected as " + name : "No Twitch account connected");
    connectButton->setEnabled(!connected); client->setEnabled(!connected);
    editor->setEnabled(connected); save->setEnabled(connected); compose->setEnabled(connected); send->setEnabled(connected);
    authorization->clear(); openAuth->hide();
    if (!connected) { *dirty = false; title->clear(); tags->clear(); categories->clear(); messages->clear(); events->clear(); compose->clear(); }
  });
  editor->setEnabled(false); save->setEnabled(false); compose->setEnabled(false); send->setEnabled(false);
  // New optional docks start hidden; View menu and persisted main-window state control them.
  infoDock->hide(); chatDock->hide(); statsDock->hide(); activityDock->hide();
  refreshStats->setEnabled(false);
  QTimer::singleShot(0, service, &twitch::TwitchService::restore);
}
} // namespace feathercast::ui
