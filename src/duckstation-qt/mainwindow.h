#pragma once
#include <QtCore/QThread>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QStackedWidget>
#include <memory>

#include "controllersettingsdialog.h"
#include "core/types.h"
#include "displaywidget.h"
#include "settingsdialog.h"
#include "ui_mainwindow.h"

class QLabel;
class QThread;
class QProgressBar;

class GameListWidget;
class EmuThread;
class AutoUpdaterDialog;
class MemoryCardEditorDialog;
class CheatManagerDialog;
class DebuggerWindow;
class MainWindow;

class HostDisplay;
namespace GameList {
struct Entry;
}

class GDBServer;

class MainWindow final : public QMainWindow
{
  Q_OBJECT

public:
  /// This class is a scoped lock on the VM, which prevents it from running while
  /// the object exists. Its purpose is to be used for blocking/modal popup boxes,
  /// where the VM needs to exit fullscreen temporarily.
  class SystemLock
  {
  public:
    SystemLock(SystemLock&& lock);
    SystemLock(const SystemLock&) = delete;
    ~SystemLock();

    /// Returns the parent widget, which can be used for any popup dialogs.
    ALWAYS_INLINE QWidget* getDialogParent() const { return m_dialog_parent; }

    /// Cancels any pending unpause/fullscreen transition.
    /// Call when you're going to destroy the VM anyway.
    void cancelResume();

  private:
    SystemLock(QWidget* dialog_parent, bool was_paused, bool was_fullscreen);
    friend MainWindow;

    QWidget* m_dialog_parent;
    bool m_was_paused;
    bool m_was_fullscreen;
  };

public:
  explicit MainWindow();
  ~MainWindow();

  /// Initializes the window. Call once at startup.
  void initialize();

  /// Performs update check if enabled in settings.
  void startupUpdateCheck();

  /// Opens memory card editor with the specified paths.
  void openMemoryCardEditor(const QString& card_a_path, const QString& card_b_path);

  /// Updates the state of the controls which should be disabled by achievements challenge mode.
  void onAchievementsChallengeModeToggled(bool enabled);

  /// Locks the VM by pausing it, while a popup dialog is displayed.
  SystemLock pauseAndLockSystem();

public Q_SLOTS:
  /// Updates debug menu visibility (hides if disabled).
  void updateDebugMenuVisibility();

  void refreshGameList(bool invalidate_cache);

  void runOnUIThread(const std::function<void()>& func);
  bool requestShutdown(bool allow_confirm = true, bool allow_save_to_state = true, bool block_until_done = false);
  void requestExit();
  void checkForSettingChanges();

  void checkForUpdates(bool display_message);

private Q_SLOTS:
  void reportError(const QString& title, const QString& message);
  bool confirmMessage(const QString& title, const QString& message);
  DisplayWidget* createDisplay(bool fullscreen, bool render_to_main);
  DisplayWidget* updateDisplay(bool fullscreen, bool render_to_main, bool surfaceless);
  void displaySizeRequested(qint32 width, qint32 height);
  void destroyDisplay();
  void focusDisplayWidget();
  void onMouseModeRequested(bool relative_mode, bool hide_cursor);
  void updateMouseMode(bool paused);

  void onSettingsResetToDefault();
  void onSystemStarting();
  void onSystemStarted();
  void onSystemDestroyed();
  void onSystemPaused();
  void onSystemResumed();
  void onSystemPerformanceCountersUpdated(float speed, float fps, float vps, float average_frame_time,
                                          float worst_frame_time, GPURenderer renderer, quint32 render_width,
                                          quint32 render_height, bool render_interlaced);
  void onRunningGameChanged(const QString& filename, const QString& game_code, const QString& game_title);
  void onApplicationStateChanged(Qt::ApplicationState state);

  void onStartFileActionTriggered();
  void onStartDiscActionTriggered();
  void onStartBIOSActionTriggered();
  void onChangeDiscFromFileActionTriggered();
  void onChangeDiscFromGameListActionTriggered();
  void onChangeDiscFromDeviceActionTriggered();
  void onChangeDiscMenuAboutToShow();
  void onChangeDiscMenuAboutToHide();
  void onLoadStateMenuAboutToShow();
  void onSaveStateMenuAboutToShow();
  void onCheatsMenuAboutToShow();
  void onRemoveDiscActionTriggered();
  void onViewToolbarActionToggled(bool checked);
  void onViewLockToolbarActionToggled(bool checked);
  void onViewStatusBarActionToggled(bool checked);
  void onViewGameListActionTriggered();
  void onViewGameGridActionTriggered();
  void onViewSystemDisplayTriggered();
  void onViewGamePropertiesActionTriggered();
  void onGitHubRepositoryActionTriggered();
  void onIssueTrackerActionTriggered();
  void onDiscordServerActionTriggered();
  void onAboutActionTriggered();
  void onCheckForUpdatesActionTriggered();
  void onToolsMemoryCardEditorTriggered();
  void onToolsCheatManagerTriggered();
  void onToolsOpenDataDirectoryTriggered();

  void onGameListRefreshComplete();
  void onGameListRefreshProgress(const QString& status, int current, int total);
  void onGameListSelectionChanged();
  void onGameListEntryActivated();
  void onGameListEntryContextMenuRequested(const QPoint& point);

  void onUpdateCheckComplete();

  void openCPUDebugger();
  void onCPUDebuggerClosed();

protected:
  void closeEvent(QCloseEvent* event) override;
  void changeEvent(QEvent* event) override;
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;

private:
  void setTheme(const QString& theme);
  void setStyleFromSettings();
  void setIconThemeFromSettings();
  void setupAdditionalUi();
  void connectSignals();
  void addThemeToMenu(const QString& name, const QString& key);

  void updateEmulationActions(bool starting, bool running, bool cheevos_challenge_mode);
  void updateStatusBarWidgetVisibility();
  void updateWindowTitle();
  void updateWindowState(bool force_visible = false);

  void setProgressBar(int current, int total);
  void clearProgressBar();

  QWidget* getDisplayContainer() const;
  bool isShowingGameList() const;
  bool isRenderingFullscreen() const;
  bool isRenderingToMain() const;
  bool shouldHideMouseCursor() const;
  bool shouldHideMainWindow() const;

  void switchToGameListView();
  void switchToEmulationView();
  void saveStateToConfig();
  void restoreStateFromConfig();
  void saveDisplayWindowGeometryToConfig();
  void restoreDisplayWindowGeometryFromConfig();
  void destroyDisplayWidget();
  void setDisplayFullscreen(const std::string& fullscreen_mode);
  bool shouldHideCursorInFullscreen() const;

  SettingsDialog* getSettingsDialog();
  void doSettings(const char* category = nullptr);

  ControllerSettingsDialog* getControllerSettingsDialog();
  void doControllerSettings(ControllerSettingsDialog::Category category = ControllerSettingsDialog::Category::Count);

  void updateDebugMenuCPUExecutionMode();
  void updateDebugMenuGPURenderer();
  void updateDebugMenuCropMode();
  void updateMenuSelectedTheme();
  std::string getDeviceDiscPath(const QString& title);
  void setGameListEntryCoverImage(const GameList::Entry* entry);
  void recreate();

  /// Fills menu with save state info and handlers.
  void populateGameListContextMenu(const GameList::Entry* entry, QWidget* parent_window, QMenu* menu);

  std::optional<bool> promptForResumeState(const std::string& save_state_path);
  void startGameListEntry(const GameList::Entry* entry, std::optional<std::string> save_path, std::optional<bool> fast_boot);

  Ui::MainWindow m_ui;

  QString m_unthemed_style_name;

  GameListWidget* m_game_list_widget = nullptr;

  DisplayWidget* m_display_widget = nullptr;
  DisplayContainer* m_display_container = nullptr;

  QProgressBar* m_status_progress_widget = nullptr;
  QLabel* m_status_speed_widget = nullptr;
  QLabel* m_status_fps_widget = nullptr;
  QLabel* m_status_frame_time_widget = nullptr;
  QLabel* m_status_renderer_widget = nullptr;
  QLabel* m_status_resolution_widget = nullptr;

  SettingsDialog* m_settings_dialog = nullptr;
  ControllerSettingsDialog* m_controller_settings_dialog = nullptr;

  AutoUpdaterDialog* m_auto_updater_dialog = nullptr;
  MemoryCardEditorDialog* m_memory_card_editor_dialog = nullptr;
  CheatManagerDialog* m_cheat_manager_dialog = nullptr;
  DebuggerWindow* m_debugger_window = nullptr;

  std::string m_current_game_title;
  std::string m_current_game_code;

  bool m_was_paused_by_focus_loss = false;
  bool m_open_debugger_on_start = false;
  bool m_relative_mouse_mode = false;
  bool m_mouse_cursor_hidden = false;

  bool m_display_created = false;
  bool m_save_states_invalidated = false;
  bool m_was_paused_on_surface_loss = false;
  bool m_was_disc_change_request = false;
  bool m_is_closing = false;

  GDBServer* m_gdb_server = nullptr;
};

extern MainWindow* g_main_window;
