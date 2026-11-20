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
  auto *history = window.findChild<QTextEdit *>("twitchChatHistory");
  auto *compose = window.findChild<QLineEdit *>("twitchChatMessage");
  auto *title = window.findChild<QLineEdit *>("twitchTitle");
  auto *service = window.findChild<feathercast::twitch::TwitchService *>();
  auto *category = window.findChild<QComboBox *>("twitchCategory");
  auto *addOns = window.findChild<QComboBox *>("twitchChatAddOns");
  auto *activity = window.findChild<QTreeWidget *>("twitchActivityHistory");
  check(history && compose && title && service && category && addOns && activity, "production widgets found");
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
  for (int i = 0; i < 600; ++i) service->chatReceived("Someone", "<b>plain text</b>", {});
  check(history->document()->blockCount() == 500 && history->toPlainText().contains("<b>plain text</b>"), "bounded plain text chat");
  service->streamReceived({{"viewer_count", 123}, {"started_at", QDateTime::currentDateTimeUtc().addSecs(-3661).toString(Qt::ISODate)}});
  auto *status = window.findChild<QLabel *>("twitchStreamStatus");
  auto *viewers = window.findChild<QLabel *>("twitchViewerCount");
  auto *uptime = window.findChild<QLabel *>("twitchUptime");
  check(status && viewers && uptime && status->text() == "Live" && viewers->text() == "123" &&
            uptime->text().startsWith("01:01:0"), "live viewer and uptime rendering");
  service->streamReceived({});
  check(status->text() == "Offline" && viewers->text() == "0" && uptime->text() == "—", "offline state");
  service->streamUnavailable();
  check(status->text() == "Unavailable" && viewers->text() == "—", "stats failure clears previous viewer count");
  check(addOns->count() == 4 && addOns->currentIndex() == 0, "optional chat add-ons default disabled");
  for (int i = 0; i < 220; ++i)
    service->activityReceived("channel.cheer", {{"user_name", "<b>Viewer</b>"}, {"bits", 100}}, QDateTime::currentDateTimeUtc());
  check(activity->topLevelItemCount() == 200 && activity->topLevelItem(0)->text(2).contains("<b>Viewer</b>"), "bounded plain text activity feed");
  auto *chatDock = window.findChild<QDockWidget *>("Twitch chat");
  chatDock->setFloating(true); chatDock->resize(620, 500); app.processEvents();
  check(chatDock->isFloating() && chatDock->width() >= 600, "chat dock resizes and floats");
  if (argc > 1) check(chatDock->grab().save(QString::fromLocal8Bit(argv[1])), "screenshot saved");
  compose->setText("unsent prior account"); service->connectionChanged(false, {});
  check(compose->text().isEmpty() && history->toPlainText().isEmpty() && activity->topLevelItemCount() == 0 && !compose->isEnabled(), "disconnect clears account chat data");
  std::cout << "Production Twitch dock controls, metadata edits, category choice, bounded text, live status and resizing passed\n";
}
