﻿#include "qthostinterface.h"
#include "common/assert.h"
#include "common/byte_stream.h"
#include "common/file_system.h"
#include "common/log.h"
#include "common/path.h"
#include "common/string_util.h"
#include "core/cheats.h"
#include "core/controller.h"
#include "core/gpu.h"
#include "core/host.h"
#include "core/host_settings.h"
#include "core/memory_card.h"
#include "core/system.h"
#include "frontend-common/fullscreen_ui.h"
#include "frontend-common/game_list.h"
#include "frontend-common/imgui_manager.h"
#include "frontend-common/ini_settings_interface.h"
#include "frontend-common/input_manager.h"
#include "frontend-common/opengl_host_display.h"
#include "frontend-common/sdl_audio_stream.h"
#include "frontend-common/vulkan_host_display.h"
#include "imgui.h"
#include "mainwindow.h"
#include "qtdisplaywidget.h"
#include "qtprogresscallback.h"
#include "qtutils.h"
#include "util/audio_stream.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDebug>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QTimer>
#include <QtCore/QTranslator>
#include <QtGui/QKeyEvent>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMenu>
#include <QtWidgets/QMessageBox>
#include <algorithm>
#include <memory>
Log_SetChannel(QtHostInterface);

#ifdef _WIN32
#include "common/windows_headers.h"
#include "frontend-common/d3d11_host_display.h"
#include "frontend-common/d3d12_host_display.h"
#include <KnownFolders.h>
#include <ShlObj.h>
#endif

#ifdef WITH_CHEEVOS
#include "frontend-common/cheevos.h"
#endif

enum : u32
{
  SETTINGS_SAVE_DELAY = 1000,
};

//////////////////////////////////////////////////////////////////////////
// Local function declarations
//////////////////////////////////////////////////////////////////////////
namespace QtHost {
static void SaveSettings();
} // namespace QtHost

static std::unique_ptr<INISettingsInterface> s_base_settings_interface;
static std::unique_ptr<QTimer> s_settings_save_timer;

QtHostInterface::QtHostInterface(QObject* parent) : QObject(parent), CommonHostInterface()
{
  qRegisterMetaType<std::shared_ptr<SystemBootParameters>>();
  qRegisterMetaType<const GameListEntry*>();
  qRegisterMetaType<GPURenderer>();
}

QtHostInterface::~QtHostInterface()
{
  Assert(!m_display);
}

const char* QtHostInterface::GetFrontendName() const
{
  return "DuckStation Qt Frontend";
}

std::vector<std::pair<QString, QString>> QtHostInterface::getAvailableLanguageList()
{
  return {{QStringLiteral("English"), QStringLiteral("en")},
          {QStringLiteral("Deutsch"), QStringLiteral("de")},
          {QStringLiteral("Español de Hispanoamérica"), QStringLiteral("es")},
          {QStringLiteral("Español de España"), QStringLiteral("es-es")},
          {QStringLiteral("Français"), QStringLiteral("fr")},
          {QStringLiteral("עברית"), QStringLiteral("he")},
          {QStringLiteral("日本語"), QStringLiteral("ja")},
          {QStringLiteral("Italiano"), QStringLiteral("it")},
          {QStringLiteral("Nederlands"), QStringLiteral("nl")},
          {QStringLiteral("Polski"), QStringLiteral("pl")},
          {QStringLiteral("Português (Pt)"), QStringLiteral("pt-pt")},
          {QStringLiteral("Português (Br)"), QStringLiteral("pt-br")},
          {QStringLiteral("Русский"), QStringLiteral("ru")},
          {QStringLiteral("Türkçe"), QStringLiteral("tr")},
          {QStringLiteral("简体中文"), QStringLiteral("zh-cn")}};
}

bool QtHostInterface::Initialize()
{
  createThread();
  if (!m_worker_thread->waitForInit())
    return false;

  installTranslator();
  return true;
}

void QtHostInterface::Shutdown()
{
  stopThread();

  delete m_main_window;
}

bool QtHostInterface::initializeOnThread()
{
  SetUserDirectory();

  s_base_settings_interface = std::make_unique<INISettingsInterface>(GetSettingsFileName());
  Host::Internal::SetBaseSettingsLayer(s_base_settings_interface.get());

  const bool loaded = s_base_settings_interface->Load();
  if (!loaded)
    SetDefaultSettings(*s_base_settings_interface.get());

  const int settings_version = s_base_settings_interface->GetIntValue("Main", "SettingsVersion", -1);
  if (settings_version != SETTINGS_VERSION)
  {
    ReportFormattedError("Settings version %d does not match expected version %d, resetting", settings_version,
                         SETTINGS_VERSION);

    s_base_settings_interface->Clear();
    s_base_settings_interface->SetIntValue("Main", "SettingsVersion", SETTINGS_VERSION);
    SetDefaultSettings(*s_base_settings_interface);
    s_base_settings_interface->Save();
  }

  if (!CommonHostInterface::Initialize())
    return false;

  // imgui setup
  setImGuiFont();

  // bind buttons/axises
  createBackgroundControllerPollTimer();
  startBackgroundControllerPollTimer();
  return true;
}

void QtHostInterface::shutdownOnThread()
{
  destroyBackgroundControllerPollTimer();
  CommonHostInterface::Shutdown();
}

void QtHostInterface::installTranslator()
{
  const QString language(QString::fromStdString(GetStringSettingValue("Main", "Language", "en")));

  // install the base qt translation first
  const QString base_dir(QStringLiteral("%1/translations").arg(qApp->applicationDirPath()));
  const QString base_path(QStringLiteral("%1/qtbase_%2.qm").arg(base_dir).arg(language));
  if (QFile::exists(base_path))
  {
    QTranslator* base_translator = new QTranslator(qApp);
    if (!base_translator->load(base_path))
    {
      QMessageBox::warning(
        nullptr, QStringLiteral("Translation Error"),
        QStringLiteral("Failed to find load base translation file for '%1':\n%2").arg(language).arg(base_path));
      delete base_translator;
    }
    else
    {
      m_translators.push_back(base_translator);
      qApp->installTranslator(base_translator);
    }
  }

  const QString path = QStringLiteral("%1/duckstation-qt_%3.qm").arg(base_dir).arg(language);
  if (!QFile::exists(path))
  {
    QMessageBox::warning(
      nullptr, QStringLiteral("Translation Error"),
      QStringLiteral("Failed to find translation file for language '%1':\n%2").arg(language).arg(path));
    return;
  }

  QTranslator* translator = new QTranslator(qApp);
  if (!translator->load(path))
  {
    QMessageBox::warning(
      nullptr, QStringLiteral("Translation Error"),
      QStringLiteral("Failed to load translation file for language '%1':\n%2").arg(language).arg(path));
    delete translator;
    return;
  }

  qDebug() << "Loaded translation file for language " << language;
  qApp->installTranslator(translator);
  m_translators.push_back(translator);
}

void QtHostInterface::reinstallTranslator()
{
  for (QTranslator* translator : m_translators)
  {
    qApp->removeTranslator(translator);
    translator->deleteLater();
  }
  m_translators.clear();

  installTranslator();
}

void QtHostInterface::ReportError(const char* message)
{
  HostInterface::ReportError(message);

  const bool was_fullscreen = m_is_fullscreen;
  if (was_fullscreen)
    SetFullscreen(false);

  emit errorReported(QString::fromUtf8(message));

  if (was_fullscreen)
    SetFullscreen(true);
}

void QtHostInterface::ReportMessage(const char* message)
{
  HostInterface::ReportMessage(message);

  emit messageReported(QString::fromUtf8(message));
}

void QtHostInterface::ReportDebuggerMessage(const char* message)
{
  HostInterface::ReportDebuggerMessage(message);

  emit debuggerMessageReported(QString::fromUtf8(message));
}

bool QtHostInterface::ConfirmMessage(const char* message)
{
  const bool was_fullscreen = m_is_fullscreen;
  if (was_fullscreen)
    SetFullscreen(false);

  const bool result = messageConfirmed(QString::fromUtf8(message));

  if (was_fullscreen)
    SetFullscreen(true);

  return result;
}

void QtHostInterface::SetBoolSettingValue(const char* section, const char* key, bool value)
{
  QtHost::SetBaseBoolSettingValue(section, key, value);
}

void QtHostInterface::SetIntSettingValue(const char* section, const char* key, int value)
{
  QtHost::SetBaseIntSettingValue(section, key, value);
}

void QtHostInterface::SetFloatSettingValue(const char* section, const char* key, float value)
{
  QtHost::SetBaseFloatSettingValue(section, key, value);
}

void QtHostInterface::SetStringSettingValue(const char* section, const char* key, const char* value)
{
  QtHost::SetBaseStringSettingValue(section, key, value);
}

void QtHostInterface::SetStringListSettingValue(const char* section, const char* key,
                                                const std::vector<std::string>& values)
{
  QtHost::SetBaseStringListSettingValue(section, key, values);
}

bool QtHostInterface::AddValueToStringList(const char* section, const char* key, const char* value)
{
  auto lock = Host::GetSettingsLock();
  if (!Host::Internal::GetBaseSettingsLayer()->AddToStringList(section, key, value))
    return false;

  queueSettingsSave();
  return true;
}

bool QtHostInterface::RemoveValueFromStringList(const char* section, const char* key, const char* value)
{
  auto lock = Host::GetSettingsLock();
  if (!Host::Internal::GetBaseSettingsLayer()->RemoveFromStringList(section, key, value))
    return false;

  queueSettingsSave();
  return true;
}

void QtHostInterface::RemoveSettingValue(const char* section, const char* key)
{
  QtHost::RemoveBaseSettingValue(section, key);
}

void QtHostInterface::queueSettingsSave()
{
  QtHost::QueueSettingsSave();
}

void QtHostInterface::setDefaultSettings()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "setDefaultSettings", Qt::QueuedConnection);
    return;
  }

  SetDefaultSettings();
}

void QtHostInterface::SetDefaultSettings()
{
  CommonHostInterface::SetDefaultSettings();
  checkRenderToMainState();
  queueSettingsSave();
  emit settingsResetToDefault();
}

void QtHostInterface::applySettings(bool display_osd_messages /* = false */)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "applySettings", Qt::QueuedConnection, Q_ARG(bool, display_osd_messages));
    return;
  }

  ApplySettings(display_osd_messages);
}

void QtHostInterface::reloadGameSettings()
{
  Panic("IMPLEMENT ME");
}

void QtHostInterface::reloadInputBindings()
{
  Panic("IMPLEMENT ME");
}

void QtHostInterface::ApplySettings(bool display_osd_messages)
{
  CommonHostInterface::ApplySettings(display_osd_messages);
  checkRenderToMainState();
}

void QtHostInterface::checkRenderToMainState()
{
  // detect when render-to-main flag changes
  if (!System::IsShutdown())
  {
    const bool render_to_main = Host::GetBaseBoolSettingValue("Main", "RenderToMainWindow", true);
    if (m_display && !m_is_fullscreen && render_to_main != m_is_rendering_to_main)
    {
      m_is_rendering_to_main = render_to_main;
      updateDisplayState();
    }
    else if (!FullscreenUI::IsInitialized())
    {
      renderDisplay();
    }
  }
}

void QtHostInterface::refreshGameList(bool invalidate_cache /* = false */, bool invalidate_database /* = false */)
{
  Assert(!isOnWorkerThread());

  auto lock = Host::GetSettingsLock();
  m_game_list->SetSearchDirectoriesFromSettings(*s_base_settings_interface.get());

  QtProgressCallback progress(m_main_window, invalidate_cache ? 0.0f : 1.0f);
  m_game_list->Refresh(invalidate_cache, invalidate_database, &progress);
  emit gameListRefreshed();
}

void QtHostInterface::setMainWindow(MainWindow* window)
{
  DebugAssert((!m_main_window && window) || (m_main_window && !window));
  m_main_window = window;
}

void QtHostInterface::bootSystem(std::shared_ptr<SystemBootParameters> params)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "bootSystem", Qt::QueuedConnection,
                              Q_ARG(std::shared_ptr<SystemBootParameters>, std::move(params)));
    return;
  }

  emit emulationStarting();
  if (!BootSystem(std::move(params)))
    return;

  // force a frame to be drawn to repaint the window
  renderDisplay();
}

void QtHostInterface::resumeSystemFromState(const QString& filename, bool boot_on_failure)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "resumeSystemFromState", Qt::QueuedConnection, Q_ARG(const QString&, filename),
                              Q_ARG(bool, boot_on_failure));
    return;
  }

  emit emulationStarting();
  if (filename.isEmpty())
    ResumeSystemFromMostRecentState();
  else
    ResumeSystemFromState(filename.toStdString().c_str(), boot_on_failure);
}

void QtHostInterface::resumeSystemFromMostRecentState()
{
  std::string state_filename = GetMostRecentResumeSaveStatePath();
  if (state_filename.empty())
  {
    emit errorReported(tr("No resume save state found."));
    return;
  }

  loadState(QString::fromStdString(state_filename));
}

void QtHostInterface::onDisplayWindowKeyEvent(int key, bool pressed)
{
  DebugAssert(isOnWorkerThread());

  InputManager::InvokeEvents(InputManager::MakeHostKeyboardKey(key), static_cast<float>(pressed), GenericInputBinding::Unknown);
}

void QtHostInterface::onDisplayWindowMouseMoveEvent(float x, float y)
{
  // display might be null here if the event happened after shutdown
  DebugAssert(isOnWorkerThread());
  if (m_display)
    m_display->SetMousePosition(static_cast<s32>(x), static_cast<s32>(y));

  InputManager::UpdatePointerAbsolutePosition(0, x, y);
}

void QtHostInterface::onDisplayWindowMouseButtonEvent(int button, bool pressed)
{
  DebugAssert(isOnWorkerThread());

  InputManager::InvokeEvents(InputManager::MakePointerButtonKey(0, button), static_cast<float>(pressed), GenericInputBinding::Unknown);
}

void QtHostInterface::onDisplayWindowMouseWheelEvent(const QPoint& delta_angle)
{
  DebugAssert(isOnWorkerThread());

  const float dx = std::clamp(static_cast<float>(delta_angle.x()) / QtUtils::MOUSE_WHEEL_DELTA, -1.0f, 1.0f);
  if (dx != 0.0f)
    InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::WheelX, dx);

  const float dy = std::clamp(static_cast<float>(delta_angle.y()) / QtUtils::MOUSE_WHEEL_DELTA, -1.0f, 1.0f);
  if (dy != 0.0f)
    InputManager::UpdatePointerRelativeDelta(0, InputPointerAxis::WheelY, dy);
}

void QtHostInterface::onDisplayWindowResized(int width, int height)
{
  // this can be null if it was destroyed and the main thread is late catching up
  if (!m_display)
    return;

  Log_DevPrintf("Display window resized to %dx%d", width, height);
  m_display->ResizeRenderWindow(width, height);
  OnHostDisplayResized();

  // re-render the display, since otherwise it will be out of date and stretched if paused
  if (!System::IsShutdown())
  {
    if (m_is_exclusive_fullscreen && !m_display->IsFullscreen())
    {
      // we lost exclusive fullscreen, switch to borderless
      AddOSDMessage(TranslateStdString("OSDMessage", "Lost exclusive fullscreen."), 10.0f);
      m_is_exclusive_fullscreen = false;
      m_is_fullscreen = false;
      m_lost_exclusive_fullscreen = true;
    }

    // force redraw if we're paused
    if (!FullscreenUI::IsInitialized())
      renderDisplay();
  }
}

void QtHostInterface::onDisplayWindowFocused()
{
  if (!m_display || !m_lost_exclusive_fullscreen)
    return;

  // try to restore exclusive fullscreen
  m_lost_exclusive_fullscreen = false;
  m_is_exclusive_fullscreen = true;
  m_is_fullscreen = true;
  updateDisplayState();
}

void QtHostInterface::redrawDisplayWindow()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "redrawDisplayWindow", Qt::QueuedConnection);
    return;
  }

  if (!m_display || System::IsShutdown())
    return;

  renderDisplay();
}

void QtHostInterface::toggleFullscreen()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "toggleFullscreen", Qt::QueuedConnection);
    return;
  }

  SetFullscreen(!m_is_fullscreen);
}

bool QtHostInterface::AcquireHostDisplay()
{
  Assert(!m_display);

  m_is_rendering_to_main = Host::GetBaseBoolSettingValue("Main", "RenderToMainWindow", true);

  QtDisplayWidget* display_widget = createDisplayRequested(m_worker_thread, m_is_fullscreen, m_is_rendering_to_main);
  if (!display_widget || !m_display->HasRenderDevice())
  {
    emit destroyDisplayRequested();
    m_display.reset();
    return false;
  }

  if (!m_display->MakeRenderContextCurrent() ||
      !m_display->InitializeRenderDevice(GetShaderCacheBasePath(), g_settings.gpu_use_debug_device,
                                         g_settings.gpu_threaded_presentation) ||
      !ImGuiManager::Initialize() || !CreateHostDisplayResources())
  {
    ImGuiManager::Shutdown();
    ReleaseHostDisplayResources();
    m_display->DestroyRenderDevice();
    emit destroyDisplayRequested();
    m_display.reset();
    return false;
  }

  m_is_exclusive_fullscreen = m_display->IsFullscreen();
  return true;
}

HostDisplay* QtHostInterface::createHostDisplay()
{
  switch (g_settings.gpu_renderer)
  {
    case GPURenderer::HardwareVulkan:
      m_display = std::make_unique<FrontendCommon::VulkanHostDisplay>();
      break;

    case GPURenderer::HardwareOpenGL:
#ifndef _WIN32
    default:
#endif
      m_display = std::make_unique<FrontendCommon::OpenGLHostDisplay>();
      break;

#ifdef _WIN32
    case GPURenderer::HardwareD3D12:
      m_display = std::make_unique<FrontendCommon::D3D12HostDisplay>();
      break;

    case GPURenderer::HardwareD3D11:
    default:
      m_display = std::make_unique<FrontendCommon::D3D11HostDisplay>();
      break;
#endif
  }

  return m_display.get();
}

void QtHostInterface::connectDisplaySignals(QtDisplayWidget* widget)
{
  widget->disconnect(this);

  connect(widget, &QtDisplayWidget::windowFocusEvent, this, &QtHostInterface::onDisplayWindowFocused);
  connect(widget, &QtDisplayWidget::windowResizedEvent, this, &QtHostInterface::onDisplayWindowResized);
  connect(widget, &QtDisplayWidget::windowRestoredEvent, this, &QtHostInterface::redrawDisplayWindow);
  connect(widget, &QtDisplayWidget::windowClosedEvent, this, &QtHostInterface::powerOffSystem,
          Qt::BlockingQueuedConnection);
  connect(widget, &QtDisplayWidget::windowKeyEvent, this, &QtHostInterface::onDisplayWindowKeyEvent);
  connect(widget, &QtDisplayWidget::windowMouseMoveEvent, this, &QtHostInterface::onDisplayWindowMouseMoveEvent);
  connect(widget, &QtDisplayWidget::windowMouseButtonEvent, this, &QtHostInterface::onDisplayWindowMouseButtonEvent);
  connect(widget, &QtDisplayWidget::windowMouseWheelEvent, this, &QtHostInterface::onDisplayWindowMouseWheelEvent);
}

void QtHostInterface::updateDisplayState()
{
  if (!m_display)
    return;

  // this expects the context to get moved back to us afterwards
  m_display->DoneRenderContextCurrent();

  QtDisplayWidget* display_widget =
    updateDisplayRequested(m_worker_thread, m_is_fullscreen, m_is_rendering_to_main && !m_is_fullscreen);
  if (!display_widget || !m_display->MakeRenderContextCurrent())
    Panic("Failed to make device context current after updating");

  m_is_exclusive_fullscreen = m_display->IsFullscreen();

  OnHostDisplayResized();

  if (!System::IsShutdown())
  {
    UpdateSoftwareCursor();

    if (!FullscreenUI::IsInitialized())
      redrawDisplayWindow();
  }

  UpdateSpeedLimiterState();
}

void QtHostInterface::ReleaseHostDisplay()
{
  Assert(m_display);

  ReleaseHostDisplayResources();
  ImGuiManager::Shutdown();
  m_display->DestroyRenderDevice();
  emit destroyDisplayRequested();
  m_display.reset();
  m_is_fullscreen = false;
}

bool QtHostInterface::IsFullscreen() const
{
  return m_is_fullscreen;
}

bool QtHostInterface::SetFullscreen(bool enabled)
{
  if (m_is_fullscreen == enabled)
    return true;

  m_is_fullscreen = enabled;
  updateDisplayState();
  return true;
}

bool QtHostInterface::RequestRenderWindowSize(s32 new_window_width, s32 new_window_height)
{
  if (new_window_width <= 0 || new_window_height <= 0 || m_is_fullscreen || m_is_exclusive_fullscreen)
    return false;

  emit displaySizeRequested(new_window_width, new_window_height);
  return true;
}

void* QtHostInterface::GetTopLevelWindowHandle() const
{
  return reinterpret_cast<void*>(m_main_window->winId());
}

void QtHostInterface::RequestExit()
{
  emit exitRequested();
}

void QtHostInterface::OnSystemCreated()
{
  CommonHostInterface::OnSystemCreated();

  wakeThread();
  stopBackgroundControllerPollTimer();

  emit emulationStarted();
  emit emulationPaused(false);
}

void QtHostInterface::OnSystemPaused(bool paused)
{
  CommonHostInterface::OnSystemPaused(paused);

  emit emulationPaused(paused);

  if (!paused)
  {
    wakeThread();
    stopBackgroundControllerPollTimer();
    emit focusDisplayWidgetRequested();
  }
  else
  {
    startBackgroundControllerPollTimer();
    renderDisplay();
  }
}

void QtHostInterface::OnSystemDestroyed()
{
  CommonHostInterface::OnSystemDestroyed();

  Host::ClearOSDMessages();
  startBackgroundControllerPollTimer();
  emit emulationStopped();
}

void QtHostInterface::OnSystemPerformanceCountersUpdated()
{
  GPURenderer renderer = GPURenderer::Count;
  u32 render_width = 0;
  u32 render_height = 0;
  bool render_interlaced = false;

  if (g_gpu)
  {
    renderer = g_gpu->GetRendererType();
    std::tie(render_width, render_height) = g_gpu->GetEffectiveDisplayResolution();
    render_interlaced = g_gpu->IsInterlacedDisplayEnabled();
  }

  emit systemPerformanceCountersUpdated(System::GetEmulationSpeed(), System::GetFPS(), System::GetVPS(),
                                        System::GetAverageFrameTime(), System::GetWorstFrameTime(), renderer,
                                        render_width, render_height, render_interlaced);
}

void QtHostInterface::OnRunningGameChanged(const std::string& path, CDImage* image, const std::string& game_code,
                                           const std::string& game_title)
{
  CommonHostInterface::OnRunningGameChanged(path, image, game_code, game_title);

  if (!System::IsShutdown())
  {
    emit runningGameChanged(QString::fromStdString(System::GetRunningPath()),
                            QString::fromStdString(System::GetRunningCode()),
                            QString::fromStdString(System::GetRunningTitle()));
  }
  else
  {
    emit runningGameChanged(QString(), QString(), QString());
  }
}

void QtHostInterface::SetDefaultSettings(SettingsInterface& si)
{
  CommonHostInterface::SetDefaultSettings(si);

  si.SetStringValue("Hotkeys", "PowerOff", "Keyboard/Escape");
  si.SetStringValue("Hotkeys", "LoadSelectedSaveState", "Keyboard/F1");
  si.SetStringValue("Hotkeys", "SaveSelectedSaveState", "Keyboard/F2");
  si.SetStringValue("Hotkeys", "SelectPreviousSaveStateSlot", "Keyboard/F3");
  si.SetStringValue("Hotkeys", "SelectNextSaveStateSlot", "Keyboard/F4");

  si.SetBoolValue("Main", "RenderToMainWindow", true);
}

void QtHostInterface::SetMouseMode(bool relative, bool hide_cursor)
{
  emit mouseModeRequested(relative, hide_cursor);
}

void QtHostInterface::applyInputProfile(const QString& profile_path)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "applyInputProfile", Qt::QueuedConnection, Q_ARG(const QString&, profile_path));
    return;
  }

  Panic("Fixme");
  // ApplyInputProfile(profile_path.toUtf8().data());
  emit inputProfileLoaded();
}

void QtHostInterface::saveInputProfile(const QString& profile_name)
{
  // std::lock_guard<std::recursive_mutex> lock(m_settings_mutex);
  // SaveInputProfile(profile_name.toUtf8().data());
  Panic("Fixme");
}

QString QtHostInterface::getUserDirectoryRelativePath(const QString& arg) const
{
  QString result = QString::fromStdString(m_user_directory);
  result += FS_OSPATH_SEPARATOR_CHARACTER;
  result += arg;
  return result;
}

QString QtHostInterface::getProgramDirectoryRelativePath(const QString& arg) const
{
  QString result = QString::fromStdString(m_program_directory);
  result += FS_OSPATH_SEPARATOR_CHARACTER;
  result += arg;
  return result;
}

QString QtHostInterface::getProgramDirectory() const
{
  return QString::fromStdString(m_program_directory);
}

void QtHostInterface::powerOffSystem()
{
  if (!isOnWorkerThread())
  {
    System::CancelPendingStartup();
    QMetaObject::invokeMethod(this, "powerOffSystem", Qt::QueuedConnection);
    return;
  }

  PowerOffSystem(ShouldSaveResumeState());
}

void QtHostInterface::powerOffSystemWithoutSaving()
{
  if (!isOnWorkerThread())
  {
    System::CancelPendingStartup();
    QMetaObject::invokeMethod(this, "powerOffSystemWithoutSaving", Qt::QueuedConnection);
    return;
  }

  PowerOffSystem(false);
}

void QtHostInterface::synchronousPowerOffSystem()
{
  if (!isOnWorkerThread())
  {
    System::CancelPendingStartup();
    QMetaObject::invokeMethod(this, "powerOffSystem", Qt::BlockingQueuedConnection);
  }
  else
  {
    powerOffSystem();
  }
}

void QtHostInterface::resetSystem()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "resetSystem", Qt::QueuedConnection);
    return;
  }

  if (System::IsShutdown())
  {
    Log_ErrorPrintf("resetSystem() called without system");
    return;
  }

  HostInterface::ResetSystem();
}

void QtHostInterface::pauseSystem(bool paused, bool wait_until_paused /* = false */)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "pauseSystem",
                              wait_until_paused ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                              Q_ARG(bool, paused), Q_ARG(bool, wait_until_paused));
    return;
  }

  CommonHostInterface::PauseSystem(paused);
}

void QtHostInterface::changeDisc(const QString& new_disc_filename)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "changeDisc", Qt::QueuedConnection, Q_ARG(const QString&, new_disc_filename));
    return;
  }

  if (System::IsShutdown())
    return;

  if (!new_disc_filename.isEmpty())
    System::InsertMedia(new_disc_filename.toStdString().c_str());
  else
    System::RemoveMedia();
}

void QtHostInterface::changeDiscFromPlaylist(quint32 index)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "changeDiscFromPlaylist", Qt::QueuedConnection, Q_ARG(quint32, index));
    return;
  }

  if (System::IsShutdown())
    return;

  if (!System::SwitchMediaSubImage(index))
    ReportFormattedError("Failed to switch to subimage %u", index);
}

static QString FormatTimestampForSaveStateMenu(u64 timestamp)
{
  const QDateTime qtime(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(timestamp)));
  return qtime.toString(QLocale::system().dateTimeFormat(QLocale::ShortFormat));
}

void QtHostInterface::populateLoadStateMenu(const char* game_code, QMenu* menu)
{
  auto add_slot = [this, game_code, menu](const QString& title, const QString& empty_title, bool global, s32 slot) {
    std::optional<SaveStateInfo> ssi = GetSaveStateInfo(global ? nullptr : game_code, slot);

    const QString menu_title =
      ssi.has_value() ? title.arg(slot).arg(FormatTimestampForSaveStateMenu(ssi->timestamp)) : empty_title.arg(slot);

    QAction* load_action = menu->addAction(menu_title);
    load_action->setEnabled(ssi.has_value());
    if (ssi.has_value())
    {
      const QString path(QString::fromStdString(ssi->path));
      connect(load_action, &QAction::triggered, [this, path]() { loadState(path); });
    }
  };

  menu->clear();

  connect(menu->addAction(tr("Load From File...")), &QAction::triggered, [this]() {
    const QString path(
      QFileDialog::getOpenFileName(m_main_window, tr("Select Save State File"), QString(), tr("Save States (*.sav)")));
    if (path.isEmpty())
      return;

    loadState(path);
  });
  QAction* load_from_state = menu->addAction(tr("Undo Load State"));
  load_from_state->setEnabled(CanUndoLoadState());
  connect(load_from_state, &QAction::triggered, this, &QtHostInterface::undoLoadState);
  menu->addSeparator();

  if (game_code && std::strlen(game_code) > 0)
  {
    for (u32 slot = 1; slot <= PER_GAME_SAVE_STATE_SLOTS; slot++)
      add_slot(tr("Game Save %1 (%2)"), tr("Game Save %1 (Empty)"), false, static_cast<s32>(slot));

    menu->addSeparator();
  }

  for (u32 slot = 1; slot <= GLOBAL_SAVE_STATE_SLOTS; slot++)
    add_slot(tr("Global Save %1 (%2)"), tr("Global Save %1 (Empty)"), true, static_cast<s32>(slot));
}

void QtHostInterface::populateSaveStateMenu(const char* game_code, QMenu* menu)
{
  auto add_slot = [this, game_code, menu](const QString& title, const QString& empty_title, bool global, s32 slot) {
    std::optional<SaveStateInfo> ssi = GetSaveStateInfo(global ? nullptr : game_code, slot);

    const QString menu_title =
      ssi.has_value() ? title.arg(slot).arg(FormatTimestampForSaveStateMenu(ssi->timestamp)) : empty_title.arg(slot);

    QAction* save_action = menu->addAction(menu_title);
    connect(save_action, &QAction::triggered, [this, global, slot]() { saveState(global, slot); });
  };

  menu->clear();

  connect(menu->addAction(tr("Save To File...")), &QAction::triggered, [this]() {
    if (!System::IsValid())
      return;

    const QString path(
      QFileDialog::getSaveFileName(m_main_window, tr("Select Save State File"), QString(), tr("Save States (*.sav)")));
    if (path.isEmpty())
      return;

    saveState(path);
  });
  menu->addSeparator();

  if (game_code && std::strlen(game_code) > 0)
  {
    for (u32 slot = 1; slot <= PER_GAME_SAVE_STATE_SLOTS; slot++)
      add_slot(tr("Game Save %1 (%2)"), tr("Game Save %1 (Empty)"), false, static_cast<s32>(slot));

    menu->addSeparator();
  }

  for (u32 slot = 1; slot <= GLOBAL_SAVE_STATE_SLOTS; slot++)
    add_slot(tr("Global Save %1 (%2)"), tr("Global Save %1 (Empty)"), true, static_cast<s32>(slot));
}

void QtHostInterface::populateGameListContextMenu(const GameListEntry* entry, QWidget* parent_window, QMenu* menu)
{
  QAction* resume_action = menu->addAction(tr("Resume"));
  resume_action->setEnabled(false);

  QMenu* load_state_menu = menu->addMenu(tr("Load State"));
  load_state_menu->setEnabled(false);

  if (!entry->code.empty())
  {
    const std::vector<SaveStateInfo> available_states(GetAvailableSaveStates(entry->code.c_str()));
    const QString timestamp_format = QLocale::system().dateTimeFormat(QLocale::ShortFormat);
    const bool challenge_mode = IsCheevosChallengeModeActive();
    for (const SaveStateInfo& ssi : available_states)
    {
      if (ssi.global)
        continue;

      const s32 slot = ssi.slot;
      const QDateTime timestamp(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(ssi.timestamp)));
      const QString timestamp_str(timestamp.toString(timestamp_format));
      const QString path(QString::fromStdString(ssi.path));

      QAction* action;
      if (slot < 0)
      {
        resume_action->setText(tr("Resume (%1)").arg(timestamp_str));
        resume_action->setEnabled(!challenge_mode);
        action = resume_action;
      }
      else
      {
        load_state_menu->setEnabled(true);
        action = load_state_menu->addAction(tr("Game Save %1 (%2)").arg(slot).arg(timestamp_str));
      }

      action->setDisabled(challenge_mode);
      connect(action, &QAction::triggered, [this, path]() { loadState(path); });
    }
  }

  QAction* open_memory_cards_action = menu->addAction(tr("Edit Memory Cards..."));
  connect(open_memory_cards_action, &QAction::triggered, [this, entry]() {
    QString paths[2];
    for (u32 i = 0; i < 2; i++)
    {
      MemoryCardType type = g_settings.memory_card_types[i];
      if (entry->code.empty() && type == MemoryCardType::PerGame)
        type = MemoryCardType::Shared;

      switch (type)
      {
        case MemoryCardType::None:
          continue;
        case MemoryCardType::Shared:
          if (g_settings.memory_card_paths[i].empty())
          {
            paths[i] = QString::fromStdString(GetSharedMemoryCardPath(i));
          }
          else
          {
            QFileInfo path(QString::fromStdString(g_settings.memory_card_paths[i]));
            path.makeAbsolute();
            paths[i] = QDir::toNativeSeparators(path.canonicalFilePath());
          }
          break;
        case MemoryCardType::PerGame:
          paths[i] = QString::fromStdString(GetGameMemoryCardPath(entry->code.c_str(), i));
          break;
        case MemoryCardType::PerGameTitle:
          paths[i] = QString::fromStdString(
            GetGameMemoryCardPath(MemoryCard::SanitizeGameTitleForFileName(entry->title).c_str(), i));
          break;
        case MemoryCardType::PerGameFileTitle:
        {
          const std::string display_name(FileSystem::GetDisplayNameFromPath(entry->path));
          paths[i] = QString::fromStdString(GetGameMemoryCardPath(
            MemoryCard::SanitizeGameTitleForFileName(Path::GetFileTitle(display_name)).c_str(), i));
        }
        break;
        default:
          break;
      }
    }

    m_main_window->openMemoryCardEditor(paths[0], paths[1]);
  });

  const bool has_any_states = resume_action->isEnabled() || load_state_menu->isEnabled();
  QAction* delete_save_states_action = menu->addAction(tr("Delete Save States..."));
  delete_save_states_action->setEnabled(has_any_states);
  if (has_any_states)
  {
    connect(delete_save_states_action, &QAction::triggered, [this, parent_window, entry] {
      if (QMessageBox::warning(
            parent_window, tr("Confirm Save State Deletion"),
            tr("Are you sure you want to delete all save states for %1?\n\nThe saves will not be recoverable.")
              .arg(QString::fromStdString(entry->code)),
            QMessageBox::Yes, QMessageBox::No) != QMessageBox::Yes)
      {
        return;
      }

      DeleteSaveStates(entry->code.c_str(), true);
    });
  }
}

void QtHostInterface::populateChangeDiscSubImageMenu(QMenu* menu, QActionGroup* action_group)
{
  if (!System::IsValid() || !System::HasMediaSubImages())
    return;

  const u32 count = System::GetMediaSubImageCount();
  const u32 current = System::GetMediaSubImageIndex();
  for (u32 i = 0; i < count; i++)
  {
    QAction* action = action_group->addAction(QString::fromStdString(System::GetMediaSubImageTitle(i)));
    action->setCheckable(true);
    action->setChecked(i == current);
    connect(action, &QAction::triggered, [this, i]() { changeDiscFromPlaylist(i); });
    menu->addAction(action);
  }
}

void QtHostInterface::populateCheatsMenu(QMenu* menu)
{
  Assert(!isOnWorkerThread());
  if (!System::IsValid())
    return;

  const bool has_cheat_list = System::HasCheatList();

  QMenu* enabled_menu = menu->addMenu(tr("&Enabled Cheats"));
  enabled_menu->setEnabled(false);
  QMenu* apply_menu = menu->addMenu(tr("&Apply Cheats"));
  apply_menu->setEnabled(false);
  if (has_cheat_list)
  {
    CheatList* cl = System::GetCheatList();
    for (const std::string& group : cl->GetCodeGroups())
    {
      QMenu* enabled_submenu = nullptr;
      QMenu* apply_submenu = nullptr;

      for (u32 i = 0; i < cl->GetCodeCount(); i++)
      {
        CheatCode& cc = cl->GetCode(i);
        if (cc.group != group)
          continue;

        QString desc(QString::fromStdString(cc.description));
        if (cc.IsManuallyActivated())
        {
          if (!apply_submenu)
          {
            apply_menu->setEnabled(true);
            apply_submenu = apply_menu->addMenu(QString::fromStdString(group));
          }

          QAction* action = apply_submenu->addAction(desc);
          connect(action, &QAction::triggered, [this, i]() { applyCheat(i); });
        }
        else
        {
          if (!enabled_submenu)
          {
            enabled_menu->setEnabled(true);
            enabled_submenu = enabled_menu->addMenu(QString::fromStdString(group));
          }

          QAction* action = enabled_submenu->addAction(desc);
          action->setCheckable(true);
          action->setChecked(cc.enabled);
          connect(action, &QAction::toggled, [this, i](bool enabled) { setCheatEnabled(i, enabled); });
        }
      }
    }
  }
}

void QtHostInterface::loadCheatList(const QString& filename)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "loadCheatList", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  LoadCheatList(filename.toUtf8().constData());
}

void QtHostInterface::setCheatEnabled(quint32 index, bool enabled)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "setCheatEnabled", Qt::QueuedConnection, Q_ARG(quint32, index),
                              Q_ARG(bool, enabled));
    return;
  }

  SetCheatCodeState(index, enabled, g_settings.auto_load_cheats);
  emit cheatEnabled(index, enabled);
}

void QtHostInterface::applyCheat(quint32 index)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "applyCheat", Qt::QueuedConnection, Q_ARG(quint32, index));
    return;
  }

  ApplyCheatCode(index);
}

void QtHostInterface::reloadPostProcessingShaders()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "reloadPostProcessingShaders", Qt::QueuedConnection);
    return;
  }

  ReloadPostProcessingShaders();
}

void QtHostInterface::requestRenderWindowScale(qreal scale)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "requestRenderWindowScale", Qt::QueuedConnection, Q_ARG(qreal, scale));
    return;
  }

  RequestRenderWindowScale(scale);
}

void QtHostInterface::executeOnEmulationThread(std::function<void()> callback, bool wait)
{
  if (isOnWorkerThread())
  {
    callback();
    if (wait)
      m_worker_thread_sync_execute_done.Signal();

    return;
  }

  QMetaObject::invokeMethod(this, "executeOnEmulationThread", Qt::QueuedConnection,
                            Q_ARG(std::function<void()>, std::move(callback)), Q_ARG(bool, wait));
  if (wait)
  {
    // don't deadlock
    while (!m_worker_thread_sync_execute_done.TryWait(10))
      qApp->processEvents(QEventLoop::ExcludeSocketNotifiers);
    m_worker_thread_sync_execute_done.Reset();
  }
}

void QtHostInterface::RunLater(std::function<void()> func)
{
  QMetaObject::invokeMethod(this, "executeOnEmulationThread", Qt::QueuedConnection,
                            Q_ARG(std::function<void()>, std::move(func)), Q_ARG(bool, false));
}

void QtHostInterface::loadState(const QString& filename)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "loadState", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  if (System::IsShutdown())
    emit emulationStarting();

  LoadState(filename.toStdString().c_str());
}

void QtHostInterface::loadState(bool global, qint32 slot)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "loadState", Qt::QueuedConnection, Q_ARG(bool, global), Q_ARG(qint32, slot));
    return;
  }

  LoadState(global, slot);
}

void QtHostInterface::saveState(const QString& filename, bool block_until_done /* = false */)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "saveState", block_until_done ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                              Q_ARG(const QString&, filename), Q_ARG(bool, block_until_done));
    return;
  }

  if (!System::IsShutdown())
    SaveState(filename.toUtf8().data());
}

void QtHostInterface::saveState(bool global, qint32 slot, bool block_until_done /* = false */)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "saveState", block_until_done ? Qt::BlockingQueuedConnection : Qt::QueuedConnection,
                              Q_ARG(bool, global), Q_ARG(qint32, slot), Q_ARG(bool, block_until_done));
    return;
  }

  if (!System::IsShutdown())
    SaveState(global, slot);
}

void QtHostInterface::undoLoadState()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "undoLoadState", Qt::QueuedConnection);
    return;
  }

  UndoLoadState();
}

void QtHostInterface::setAudioOutputVolume(int volume, int fast_forward_volume)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "setAudioOutputVolume", Qt::QueuedConnection, Q_ARG(int, volume),
                              Q_ARG(int, fast_forward_volume));
    return;
  }

  g_settings.audio_output_volume = volume;
  g_settings.audio_fast_forward_volume = fast_forward_volume;

  if (m_audio_stream)
    m_audio_stream->SetOutputVolume(GetAudioOutputVolume());
}

void QtHostInterface::setAudioOutputMuted(bool muted)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "setAudioOutputMuted", Qt::QueuedConnection, Q_ARG(bool, muted));
    return;
  }

  g_settings.audio_output_muted = muted;

  if (m_audio_stream)
    m_audio_stream->SetOutputVolume(GetAudioOutputVolume());
}

void QtHostInterface::startDumpingAudio()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "startDumpingAudio", Qt::QueuedConnection);
    return;
  }

  StartDumpingAudio();
}

void QtHostInterface::stopDumpingAudio()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "stopDumpingAudio", Qt::QueuedConnection);
    return;
  }

  StopDumpingAudio();
}

void QtHostInterface::singleStepCPU()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "singleStepCPU", Qt::BlockingQueuedConnection);
    return;
  }

  if (!System::IsValid())
    return;

  System::SingleStepCPU();
  renderDisplay();
}

void QtHostInterface::dumpRAM(const QString& filename)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "dumpRAM", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  const std::string filename_str = filename.toStdString();
  if (System::DumpRAM(filename_str.c_str()))
    ReportFormattedMessage("RAM dumped to '%s'", filename_str.c_str());
  else
    ReportFormattedMessage("Failed to dump RAM to '%s'", filename_str.c_str());
}

void QtHostInterface::dumpVRAM(const QString& filename)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "dumpVRAM", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  const std::string filename_str = filename.toStdString();
  if (System::DumpVRAM(filename_str.c_str()))
    ReportFormattedMessage("VRAM dumped to '%s'", filename_str.c_str());
  else
    ReportFormattedMessage("Failed to dump VRAM to '%s'", filename_str.c_str());
}

void QtHostInterface::dumpSPURAM(const QString& filename)
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "dumpSPURAM", Qt::QueuedConnection, Q_ARG(const QString&, filename));
    return;
  }

  const std::string filename_str = filename.toStdString();
  if (System::DumpSPURAM(filename_str.c_str()))
    ReportFormattedMessage("SPU RAM dumped to '%s'", filename_str.c_str());
  else
    ReportFormattedMessage("Failed to dump SPU RAM to '%s'", filename_str.c_str());
}

void QtHostInterface::saveScreenshot()
{
  if (!isOnWorkerThread())
  {
    QMetaObject::invokeMethod(this, "saveScreenshot", Qt::QueuedConnection);
    return;
  }

  SaveScreenshot(nullptr, true, true);
}

void QtHostInterface::OnAchievementsRefreshed()
{
#ifdef WITH_CHEEVOS
  QString game_info;

  if (Cheevos::HasActiveGame())
  {
    game_info = tr("Game ID: %1\n"
                   "Game Title: %2\n"
                   "Game Developer: %3\n"
                   "Game Publisher: %4\n"
                   "Achievements: %5 (%6)\n\n")
                  .arg(Cheevos::GetGameID())
                  .arg(QString::fromStdString(Cheevos::GetGameTitle()))
                  .arg(QString::fromStdString(Cheevos::GetGameDeveloper()))
                  .arg(QString::fromStdString(Cheevos::GetGamePublisher()))
                  .arg(Cheevos::GetAchievementCount())
                  .arg(tr("%n points", "", Cheevos::GetMaximumPointsForGame()));

    const std::string& rich_presence_string = Cheevos::GetRichPresenceString();
    if (!rich_presence_string.empty())
      game_info.append(QString::fromStdString(rich_presence_string));
    else
      game_info.append(tr("Rich presence inactive or unsupported."));
  }
  else
  {
    game_info = tr("Game not loaded or no RetroAchievements available.");
  }

  emit achievementsLoaded(Cheevos::GetGameID(), game_info, Cheevos::GetAchievementCount(),
                          Cheevos::GetMaximumPointsForGame());
#endif
}

void QtHostInterface::OnDisplayInvalidated()
{
  renderDisplay();
}

void QtHostInterface::doBackgroundControllerPoll()
{
  PollAndUpdate();
}

void QtHostInterface::createBackgroundControllerPollTimer()
{
  DebugAssert(!m_background_controller_polling_timer);
  m_background_controller_polling_timer = new QTimer(this);
  m_background_controller_polling_timer->setSingleShot(false);
  m_background_controller_polling_timer->setTimerType(Qt::CoarseTimer);
  connect(m_background_controller_polling_timer, &QTimer::timeout, this, &QtHostInterface::doBackgroundControllerPoll);
}

void QtHostInterface::destroyBackgroundControllerPollTimer()
{
  delete m_background_controller_polling_timer;
  m_background_controller_polling_timer = nullptr;
}

void QtHostInterface::startBackgroundControllerPollTimer()
{
  if (m_background_controller_polling_timer->isActive())
    return;

  m_background_controller_polling_timer->start(BACKGROUND_CONTROLLER_POLLING_INTERVAL);
}

void QtHostInterface::stopBackgroundControllerPollTimer()
{
  if (!m_background_controller_polling_timer->isActive())
    return;

  m_background_controller_polling_timer->stop();
}

void QtHostInterface::createThread()
{
  m_original_thread = QThread::currentThread();
  m_worker_thread = new Thread(this);
  m_worker_thread->start();
  moveToThread(m_worker_thread);
}

void QtHostInterface::stopThread()
{
  Assert(!isOnWorkerThread());

  QMetaObject::invokeMethod(this, "doStopThread", Qt::QueuedConnection);
  m_worker_thread->wait();
}

void QtHostInterface::doStopThread()
{
  m_shutdown_flag.store(true);
  m_worker_thread_event_loop->quit();
}

void QtHostInterface::threadEntryPoint()
{
  m_worker_thread_event_loop = new QEventLoop();

  // set up controller interface and immediate poll to pick up the controller attached events
  m_worker_thread->setInitResult(initializeOnThread());

  // TODO: Event which flags the thread as ready
  while (!m_shutdown_flag.load())
  {
    if (System::IsRunning())
    {
      if (m_display_all_frames)
        System::RunFrame();
      else
        System::RunFrames();

      InputManager::PollSources();
      if (m_frame_step_request)
      {
        m_frame_step_request = false;
        PauseSystem(true);
      }

      renderDisplay();

      System::UpdatePerformanceCounters();

      if (m_throttler_enabled)
        System::Throttle();
    }
    else
    {
      // we want to keep rendering the UI when paused and fullscreen UI is enabled
      if (!FullscreenUI::IsInitialized() || !System::IsValid())
      {
        // wait until we have a system before running
        m_worker_thread_event_loop->exec();
        continue;
      }

      renderDisplay();
    }

    m_worker_thread_event_loop->processEvents(QEventLoop::AllEvents);
    PollAndUpdate();
  }

  shutdownOnThread();

  delete m_worker_thread_event_loop;
  m_worker_thread_event_loop = nullptr;
  if (s_settings_save_timer)
  {
    s_settings_save_timer.reset();
    QtHost::SaveSettings();
  }

  // move back to UI thread
  moveToThread(m_original_thread);
}

void QtHostInterface::renderDisplay()
{
  ImGuiManager::RenderOSD();
  m_display->Render();
  ImGuiManager::NewFrame();
}

void QtHostInterface::wakeThread()
{
  if (isOnWorkerThread())
    m_worker_thread_event_loop->quit();
  else
    QMetaObject::invokeMethod(m_worker_thread_event_loop, "quit", Qt::QueuedConnection);
}

static std::string GetFontPath(const char* name)
{
#ifdef _WIN32
  PWSTR folder_path;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Fonts, 0, nullptr, &folder_path)))
    return StringUtil::StdStringFromFormat("C:\\Windows\\Fonts\\%s", name);

  std::string font_path(StringUtil::WideStringToUTF8String(folder_path));
  CoTaskMemFree(folder_path);
  font_path += "\\";
  font_path += name;
  return font_path;
#else
  return name;
#endif
}

void QtHostInterface::setImGuiFont()
{
  std::string language(GetStringSettingValue("Main", "Language", ""));

  std::string path;
  const ImWchar* range = nullptr;
#ifdef _WIN32
  if (language == "ja")
  {
    path = GetFontPath("msgothic.ttc");
    range = ImGui::GetIO().Fonts->GetGlyphRangesJapanese();
  }
  else if (language == "zh-cn")
  {
    path = GetFontPath("msyh.ttc");
    range = ImGui::GetIO().Fonts->GetGlyphRangesChineseSimplifiedCommon();
  }
#endif

#if 0
  if (!path.empty())
    ImGuiFullscreen::SetFontFilename(std::move(path));
  if (range)
    ImGuiFullscreen::SetFontGlyphRanges(range);
#endif
  if (!path.empty())
    Panic("Fixme");
  if (range)
    Panic("Fixme");
}

TinyString QtHostInterface::TranslateString(const char* context, const char* str,
                                            const char* disambiguation /*= nullptr*/, int n /*= -1*/) const
{
  const QByteArray bytes(qApp->translate(context, str, disambiguation, n).toUtf8());
  return TinyString(bytes.constData(), bytes.size());
}

std::string QtHostInterface::TranslateStdString(const char* context, const char* str,
                                                const char* disambiguation /*= nullptr*/, int n /*= -1*/) const
{
  return qApp->translate(context, str, disambiguation, n).toStdString();
}

QtHostInterface::Thread::Thread(QtHostInterface* parent) : QThread(parent), m_parent(parent) {}

QtHostInterface::Thread::~Thread() = default;

void QtHostInterface::Thread::run()
{
  m_parent->threadEntryPoint();
}

void QtHostInterface::Thread::setInitResult(bool result)
{
  m_init_result.store(result);
  m_init_event.Signal();
}

bool QtHostInterface::Thread::waitForInit()
{
  while (!m_init_event.TryWait(100))
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

  return m_init_result.load();
}

void Host::OnInputDeviceConnected(const std::string_view& identifier, const std::string_view& device_name)
{

}

void Host::OnInputDeviceDisconnected(const std::string_view& identifier)
{

}

std::optional<std::vector<u8>> Host::ReadResourceFile(const char* filename)
{
  //const std::string path(Path::Combine(EmuFolders::Resources, filename));
  const std::string path(QtHostInterface::GetInstance()->GetProgramDirectoryRelativePath("resources" FS_OSPATH_SEPARATOR_STR "%s", filename));
  std::optional<std::vector<u8>> ret(FileSystem::ReadBinaryFile(path.c_str()));
  if (!ret.has_value())
    Log_ErrorPrintf("Failed to read resource file '%s'", filename);
  return ret;
}

std::optional<std::string> Host::ReadResourceFileToString(const char* filename)
{
  const std::string path(QtHostInterface::GetInstance()->GetProgramDirectoryRelativePath("resources" FS_OSPATH_SEPARATOR_STR "%s", filename));
  std::optional<std::string> ret(FileSystem::ReadFileToString(path.c_str()));
  if (!ret.has_value())
    Log_ErrorPrintf("Failed to read resource file to string '%s'", filename);
  return ret;
}

void QtHost::SetBaseBoolSettingValue(const char* section, const char* key, bool value)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->SetBoolValue(section, key, value);
  QueueSettingsSave();
}

void QtHost::SetBaseIntSettingValue(const char* section, const char* key, int value)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->SetIntValue(section, key, value);
  QueueSettingsSave();
}

void QtHost::SetBaseFloatSettingValue(const char* section, const char* key, float value)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->SetFloatValue(section, key, value);
  QueueSettingsSave();
}

void QtHost::SetBaseStringSettingValue(const char* section, const char* key, const char* value)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->SetStringValue(section, key, value);
  QueueSettingsSave();
}

void QtHost::SetBaseStringListSettingValue(const char* section, const char* key, const std::vector<std::string>& values)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->SetStringList(section, key, values);
  QueueSettingsSave();
}

bool QtHost::AddBaseValueToStringList(const char* section, const char* key, const char* value)
{
  auto lock = Host::GetSettingsLock();
  if (!s_base_settings_interface->AddToStringList(section, key, value))
    return false;

  QueueSettingsSave();
  return true;
}

bool QtHost::RemoveBaseValueFromStringList(const char* section, const char* key, const char* value)
{
  auto lock = Host::GetSettingsLock();
  if (!s_base_settings_interface->RemoveFromStringList(section, key, value))
    return false;

  QueueSettingsSave();
  return true;
}

void QtHost::RemoveBaseSettingValue(const char* section, const char* key)
{
  auto lock = Host::GetSettingsLock();
  s_base_settings_interface->DeleteValue(section, key);
  QueueSettingsSave();
}

void QtHost::SaveSettings()
{
  AssertMsg(!QtHostInterface::GetInstance()->isOnWorkerThread(), "Saving should happen on the UI thread.");

  {
    auto lock = Host::GetSettingsLock();
    if (!s_base_settings_interface->Save())
      Log_ErrorPrintf("Failed to save settings.");
  }

  s_settings_save_timer->deleteLater();
  s_settings_save_timer.release();
}

void QtHost::QueueSettingsSave()
{
  if (s_settings_save_timer)
    return;

  s_settings_save_timer = std::make_unique<QTimer>();
  s_settings_save_timer->connect(s_settings_save_timer.get(), &QTimer::timeout, SaveSettings);
  s_settings_save_timer->setSingleShot(true);
  s_settings_save_timer->start(SETTINGS_SAVE_DELAY);
}

BEGIN_HOTKEY_LIST(g_host_hotkeys)
END_HOTKEY_LIST()
