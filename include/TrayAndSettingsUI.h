#pragma once
#include "TranslationTypes.h"
#include "SimpleCapture.h"
#include "HotKeyManager.h"
#include <shellapi.h>

namespace ChromaTranslate {

    static constexpr UINT WM_TRAYICON = WM_USER + 200;

    class TrayAndSettingsUI {
    public:
        TrayAndSettingsUI(
            HINSTANCE hInstance,
            HWND overlayHwnd,
            TranslatorConfig& config,
            PipelineSyncState& syncState,
            SimpleCapture& capture,
            HotKeyManager& hotkeys);

        ~TrayAndSettingsUI();

        void InitializeTrayIcon();
        void RemoveTrayIcon();
        void HandleTrayMessage(LPARAM lParam);
        void OpenSettingsWindow();
        void ApplyOverlayClickThroughState(bool unlockedInteractive);

    private:
        void ShowTrayContextMenu();
        void BroadcastConfigUpdate();

        static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        void PopulateSettingsControls(HWND settingsHwnd);
        void SaveFromSettingsControls(HWND settingsHwnd);

    private:
        HINSTANCE m_hInstance = nullptr;
        HWND m_overlayHwnd = nullptr;
        HWND m_settingsHwnd = nullptr;
        HFONT m_uiFont = nullptr;

        TranslatorConfig& m_config;
        PipelineSyncState& m_sync;
        SimpleCapture& m_capture;
        HotKeyManager& m_hotkeys;

        NOTIFYICONDATAW m_nid{};
        bool m_trayAdded = false;

        std::vector<CaptureTargetWindow> m_cachedWindows;
        std::vector<CaptureTargetMonitor> m_cachedMonitors;

        HWND m_cmbSourceLang = nullptr;
        HWND m_cmbTargetLang = nullptr;
        HWND m_cmbCaptureMode = nullptr;
        HWND m_cmbOcrBackend = nullptr;
        HWND m_edtPollMs = nullptr;
        HWND m_edtDiffCells = nullptr;
        HWND m_edtFurigana = nullptr;
        HWND m_edtThreads = nullptr;
        HWND m_chkContinuous = nullptr;
        HWND m_chkUnlockOverlay = nullptr;
    };

} // namespace ChromaTranslate