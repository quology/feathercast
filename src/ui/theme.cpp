#include "feathercast/ui/theme.hpp"
#include <QtWidgets>

namespace feathercast::ui {
// Shared neutral tokens keep dialogs, floating docks and both themes consistent.
void applyTheme(bool dark) {
  const QString background = dark ? "#09090b" : "#fafafa";
  const QString surface = dark ? "#18181b" : "#ffffff";
  const QString muted = dark ? "#27272a" : "#f4f4f5";
  const QString border = dark ? "#3f3f46" : "#d4d4d8";
  const QString text = dark ? "#fafafa" : "#18181b";
  const QString secondary = dark ? "#a1a1aa" : "#52525b";
  QPalette palette;
  palette.setColor(QPalette::Window, QColor(background));
  palette.setColor(QPalette::WindowText, QColor(text));
  palette.setColor(QPalette::Base, QColor(surface));
  palette.setColor(QPalette::AlternateBase, QColor(muted));
  palette.setColor(QPalette::Text, QColor(text));
  palette.setColor(QPalette::Button, QColor(surface));
  palette.setColor(QPalette::ButtonText, QColor(text));
  palette.setColor(QPalette::Highlight, QColor(muted));
  palette.setColor(QPalette::HighlightedText, QColor(text));
  palette.setColor(QPalette::ToolTipBase, QColor(surface));
  palette.setColor(QPalette::ToolTipText, QColor(text));
  palette.setColor(QPalette::PlaceholderText, QColor(secondary));
  palette.setColor(QPalette::Disabled, QPalette::Text, QColor(secondary));
  palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(secondary));
  qApp->setPalette(palette);
  QString style = R"(
    QWidget { color: @text; font-family: "Segoe UI"; font-size: 10pt; }
    QMainWindow, QDialog { background: @background; }
    QMainWindow::separator { background: @background; width: 6px; height: 6px; }
    QMainWindow::separator:hover { background: @border; }
    QDockWidget { background: @surface; border: 1px solid @border; }
    QDockWidget::title { background: @surface; padding: 10px 12px; border-bottom: 1px solid @border; font-weight: 600; }
    QToolBar { background: @surface; border: 0; border-bottom: 1px solid @border; spacing: 12px; padding: 10px 16px; }
    QLabel[role="heading"] { font-size: 16pt; font-weight: 600; }
    QLabel[role="section"] { font-weight: 600; padding: 2px 0; }
    QLabel[role="muted"] { color: @secondary; }
    QLabel[role="badge"] { background: @muted; color: @secondary; border: 1px solid @border; border-radius: 5px; padding: 4px 8px; font-size: 9pt; }
    QWidget#previewCard { background: @surface; border: 1px solid @border; border-radius: 8px; }
    QPushButton { background: @surface; border: 1px solid @border; border-radius: 6px; padding: 6px 12px; min-height: 18px; font-weight: 500; }
    QPushButton:hover { background: @muted; }
    QPushButton:pressed { background: @border; }
    QPushButton:focus, QToolButton:focus { border: 2px solid @secondary; padding: 5px 11px; }
    QPushButton[variant="primary"], QDialogButtonBox QPushButton:default { background: @text; color: @background; border-color: @text; font-weight: 600; }
    QPushButton[variant="primary"]:hover, QDialogButtonBox QPushButton:default:hover { background: @secondary; border-color: @secondary; }
    QPushButton:disabled { background: @muted; color: @secondary; border-color: @border; }
    QLineEdit, QAbstractSpinBox, QComboBox, QKeySequenceEdit { background: @surface; border: 1px solid @border; border-radius: 6px; padding: 5px 8px; min-height: 20px; selection-background-color: @muted; selection-color: @text; }
    QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus, QKeySequenceEdit:focus { border: 1px solid @secondary; }
    QComboBox { padding-right: 24px; }
    QComboBox::drop-down { border: 0; width: 22px; }
    QComboBox QAbstractItemView { background: @surface; border: 1px solid @border; selection-background-color: @muted; selection-color: @text; padding: 4px; }
    QListWidget, QTableWidget { background: @surface; alternate-background-color: @muted; border: 1px solid @border; border-radius: 6px; padding: 3px; outline: 0; gridline-color: @border; }
    QListWidget::item { padding: 7px 9px; border-radius: 4px; }
    QListWidget::item:hover { background: @muted; }
    QListWidget::item:selected { background: @muted; color: @text; }
    QListWidget::item:focus { border: 1px solid @secondary; }
    QHeaderView::section { background: @muted; border: 0; border-bottom: 1px solid @border; padding: 7px; color: @secondary; font-weight: 500; }
    QCheckBox { spacing: 8px; padding: 3px 0; }
    QCheckBox::indicator { width: 16px; height: 16px; }
    QCheckBox::indicator:unchecked { background: @surface; border: 1px solid @border; border-radius: 4px; }
    QCheckBox::indicator:unchecked:hover { border-color: @secondary; }
    QTabWidget::pane { background: @surface; border: 1px solid @border; border-radius: 6px; }
    QTabBar::tab { background: @background; color: @secondary; padding: 7px 12px; border: 1px solid transparent; }
    QTabBar::tab:selected { background: @surface; color: @text; border-color: @border; border-radius: 5px; }
    QTabBar::tab:hover { color: @text; }
    QMenuBar, QMenu { background: @surface; }
    QMenuBar { border-bottom: 1px solid @border; padding: 3px 8px; }
    QMenu { border: 1px solid @border; padding: 4px; }
    QMenu::item { padding: 7px 24px; border-radius: 4px; }
    QMenu::item:selected, QMenuBar::item:selected { background: @muted; }
    QMenu::separator { height: 1px; background: @border; margin: 4px 8px; }
    QStatusBar { background: @surface; color: @secondary; border-top: 1px solid @border; padding: 3px 8px; }
    QToolTip { color: @text; background: @surface; border: 1px solid @border; padding: 6px; }
    QScrollArea { border: 0; background: transparent; }
    QScrollBar:vertical { background: @background; width: 10px; margin: 0; }
    QScrollBar::handle:vertical { background: @border; border-radius: 4px; min-height: 24px; }
    QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
    QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
    QSlider::groove:horizontal { height: 4px; background: @border; border-radius: 2px; }
    QSlider::handle:horizontal { background: @text; width: 14px; margin: -5px 0; border-radius: 7px; }
  )";
  style.replace("@background", background).replace("@surface", surface)
      .replace("@muted", muted).replace("@border", border)
      .replace("@text", text).replace("@secondary", secondary);
  qApp->setStyleSheet(style);
}
}
