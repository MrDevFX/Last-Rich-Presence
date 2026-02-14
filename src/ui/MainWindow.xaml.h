#pragma once

#include "MainWindow.g.h"
#include "MediaDetector.h"
#include "PresenceManager.h"

#include <deque>

namespace winrt::Last_Rich_Presence::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow() = default;
        ~MainWindow();

        void InitWindow();

        // XAML event handlers
        void OnEnableToggled(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnNavSelectionChanged(winrt::Microsoft::UI::Xaml::Controls::NavigationView const& sender,
                                   winrt::Microsoft::UI::Xaml::Controls::NavigationViewSelectionChangedEventArgs const& args);
        void OnTimestampToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnSourceToggled(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnSourceDebugToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnPausedToggled(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnAlbumArtToggled(winrt::Windows::Foundation::IInspectable const& sender,
                               winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnCloseToTrayToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                  winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnLaunchOnStartupToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                      winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnStartMinimizedToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnTrayLeftClickToggleToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                          winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnSensitiveKeywordFilterToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnStrictBrowserPrivacyToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnSuppressBrowserArtToggled(winrt::Windows::Foundation::IInspectable const& sender,
                                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnClearDiagnosticsClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                       winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnExportDiagnosticsJsonClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnResetSettingsClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                    winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnExportSettingsJsonClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnImportSettingsJsonClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                         winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void OnApplyBlockedAppSitesClicked(winrt::Windows::Foundation::IInspectable const& sender,
                                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const& e);

    private:
        void ShutdownWindow();
        void InitializeSystemTray();
        void CleanupSystemTray();
        void ShowTrayContextMenu();
        void ShowWindowFromTray();
        void HideWindowToTray();
        void ToggleWindowVisibilityFromTray();
        void ApplyLaunchOnStartupState(bool enabled, bool userInitiated);
        void HandleTrayMenuCommand(uint32_t commandId);
        bool TryHandleTrayWindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);
        static LRESULT CALLBACK TrayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
        void OnMediaChanged(const MediaInfo& info);
        void UpdateUI(const MediaInfo& info);
        void StartProgressTimer();
        void StopProgressTimer();
        bool IsMotionEnabled() const;
        void TransitionElementVisibility(winrt::Microsoft::UI::Xaml::FrameworkElement const& element, bool show, double offsetY);
        void SetSongProgress(double progressPercent);
        void UpdateSongWaveClip(double progressPercent);
        void SetSongWaveActive(bool active);
        void SetHomeMiniProgress(double progressPercent);
        void UpdateHomeMiniWaveClip(double progressPercent);
        void SetHomeMiniWaveActive(bool active);
        void UpdateConnectionStatus();
        void SetLivePulseActive(bool active);
        void ShowTrackTransitionSkeleton();
        void HideTrackTransitionSkeleton();
        void UpdateSourceBadge(const MediaInfo& info);
        std::wstring FormatSourceDebugText(const MediaInfo& info) const;
        void AppendDiagnosticLog(const std::wstring& level, const std::wstring& component, const std::wstring& message);
        void RefreshDiagnosticsPanel();
        std::wstring BuildDiagnosticsSnapshotJson() const;
        std::wstring BuildSettingsSnapshotJson();
        void UpdateThumbnail(const MediaInfo& info);
        void ShowConnectionInfoBar(bool connected);
        void LoadSettings();
        void SaveSettings();
        static std::wstring FormatTime(int totalSeconds);

        std::shared_ptr<MediaDetector> m_mediaDetector;
        std::shared_ptr<PresenceManager> m_presence;
        std::shared_ptr<std::atomic<bool>> m_lifetimeToken;
        MediaInfo m_lastMedia;
        Microsoft::UI::Xaml::DispatcherTimer m_progressTimer{nullptr};
        Microsoft::UI::Xaml::DispatcherTimer m_connectionInfoBarTimer{nullptr};
        Microsoft::UI::Xaml::DispatcherTimer m_trackSkeletonTimer{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_connectionInfoBarFadeStoryboard{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_pageTransitionStoryboard{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_songProgressStoryboard{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_homeMiniProgressStoryboard{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_songWaveStoryboard{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_homeMiniWaveStoryboard{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_livePulseStoryboard{nullptr};
        Microsoft::UI::Xaml::Media::Animation::Storyboard m_trackSkeletonStoryboard{nullptr};
        Microsoft::UI::Dispatching::DispatcherQueue m_dispatcherQueue{nullptr};
        winrt::Windows::Foundation::IAsyncAction m_thumbnailUpdateTask{ nullptr };
        bool m_enabled{true};
        bool m_wasConnected{false};
        bool m_isInitializing{true};
        bool m_sourceDebugMode{false};
        bool m_isShuttingDown{false};
        bool m_exitRequested{false};
        bool m_hiddenToTray{false};
        bool m_trayIconAdded{false};
        bool m_trayIconOwned{false};
        bool m_closeToTrayOnClose{true};
        bool m_launchOnStartup{false};
        bool m_startMinimizedToTray{false};
        bool m_trayLeftClickToggles{true};
        bool m_sensitiveKeywordFilter{true};
        bool m_strictBrowserPrivacy{false};
        bool m_suppressBrowserAlbumArt{false};
        std::wstring m_blockedAppSiteTermsRaw;
        std::chrono::steady_clock::time_point m_lastTrayToggleAt{};
        HWND m_windowHandle{ nullptr };
        WNDPROC m_originalWndProc{ nullptr };
        HICON m_trayIconHandle{ nullptr };
        std::deque<std::wstring> m_diagnosticLines;
        std::wstring m_lastMergeState;
        std::wstring m_lastUiTrackKey;
        std::wstring m_lastUiSourceKey;
        std::wstring m_activePageTag{L"Home"};
        std::chrono::steady_clock::time_point m_lastPresencePushAt{};
        bool m_lastPresencePushPlaying{false};
        bool m_reduceMotionRequested{false};
        uint64_t m_connectionInfoBarVersion{0};
    };
}

namespace winrt::Last_Rich_Presence::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow>
    {
    };
}
