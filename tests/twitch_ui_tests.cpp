#include "feathercast/ui/twitch_docks.hpp"
#include "feathercast/twitch/twitch_service.hpp"
#include <QtWidgets>
#include <QJsonArray>
#include <iostream>
static void check(bool value, const char *label) { if (!value) { std::cerr << label << '\n'; std::exit(1); } }
int main(int argc, char **argv) {
  QApplication app(argc, argv); QTemporaryDir temporary;
  QMainWindow window; window.resize(1100, 800); window.setCentralWidget(new QLabel("Native Twitch dock fixture"));
  feathercast::ui::installTwitchDocks(&window, temporary.path());
  window.show(); app.processEvents();
  auto docks = window.findChildren<QDockWidget *>();
  check(docks.size() == 4, "four optional docks");
  auto *history = window.findChild<QPlainTextEdit *>("twitchChatHistory");
  auto *compose = window.findChild<QLineEdit *>("twitchChatMessage");
  auto *title = window.findChild<QLineEdit *>("twitchTitle");
  auto *service = window.findChild<feathercast::twitch::TwitchService *>();
  auto *category = window.findChild<QComboBox *>();
  check(history && compose && title && service && category, "production widgets found");
  check(!compose->isEnabled() && !title->isEnabled() && history->isReadOnly(), "disconnected controls");
  for (auto *dock : docks) { check(!dock->isVisible(), "optional dock initially hidden"); dock->toggleViewAction()->trigger(); }
  app.processEvents();
  service->connectionChanged(true, "LocalFixture");
  service->channelReceived({{"title", "Fixture title"}, {"game_name", "Original"}, {"game_id", "12"}, {"tags", QJsonArray{"English"}}});
  service->categoriesReceived({QJsonObject{{"name", "Search result"}, {"id", "99"}}});
  check(category->currentData().toString() == "12", "search preserves original category until selection");
  title->setText("Unsaved title"); title->textEdited(title->text());
  service->channelReceived({{"title", "Poll result"}});
  check(title->text() == "Unsaved title", "poll preserves dirty edit");
  for (int i = 0; i < 600; ++i) service->chatReceived("Someone", "<b>plain text</b>");
  check(history->document()->blockCount() == 500 && history->toPlainText().contains("<b>plain text</b>"), "bounded plain text chat");
  service->streamReceived({{"viewer_count", 123}, {"started_at", QDateTime::currentDateTimeUtc().addSecs(-3661).toString(Qt::ISODate)}});
  bool live = false; for (auto *label : window.findChildren<QLabel *>()) if (label->text().contains("123 viewers") && label->text().contains("01:01:01")) live = true;
  check(live, "live viewer and uptime rendering");
  auto *feed = window.findChild<QListWidget *>("twitchActivityFeed");
  auto *filter = window.findChild<QComboBox *>("twitchActivityFilter");
  auto *stats = window.findChild<QLabel *>("twitchStatistics");
  check(feed && filter && stats && window.findChild<QPushButton *>("twitchReauthorize"), "activity and stats controls");
  for (int i = 0; i < 510; ++i) service->activityReceived({{"type", "channel.follow"}, {"text", "<b>literal activity</b>"}});
  check(feed->count() == 500 && feed->item(0)->text().contains("<b>literal activity</b>"), "bounded plain safe feed");
  filter->setCurrentIndex(2); check(feed->item(0)->isHidden(), "type filter hides nonmatching events");
  filter->setCurrentIndex(1); check(!feed->item(0)->isHidden(), "type filter restores matches");
  feed->setCurrentRow(0); window.findChild<QPushButton *>("twitchActivityDismiss")->click(); check(feed->count() == 499, "local dismiss");
  window.findChild<QPushButton *>("twitchActivityClear")->click(); check(feed->count() == 0, "local clear");
  service->statisticsReceived({{"followers", 0}, {"subscribers", 12}, {"points", 15}});
  check(stats->text().contains("Followers: 0") && stats->text().contains("Subscribers: 12") && stats->text().contains("unavailable (not provided by Twitch)"), "available zero versus deprecated unavailable");
  service->activityReceived({{"type", "channel.raid"}, {"text", "Incoming raid"}});
  docks.last()->setFloating(true); docks.last()->resize(620, 500); app.processEvents();
  check(docks.last()->isFloating() && docks.last()->width() >= 600, "chat dock resizes and floats");
  if (argc > 1) check(docks.last()->grab().save(QString::fromLocal8Bit(argv[1])), "screenshot saved");
  compose->setText("unsent prior account"); service->connectionChanged(false, {});
  check(compose->text().isEmpty() && history->toPlainText().isEmpty() && !compose->isEnabled(), "disconnect clears account chat data");
  check(feed->count() == 0 && stats->text().contains("Followers: unavailable"), "disconnect clears activity and statistics");
  std::cout << "Production Twitch dock controls, metadata edits, category choice, bounded text, live status and resizing passed\n";
}
