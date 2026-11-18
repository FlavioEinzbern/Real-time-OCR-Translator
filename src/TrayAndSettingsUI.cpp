#include "TrayAndSettingsUI.h"

namespace ChromaTranslate {

    enum TrayCommandId : UINT {
        CMD_TRIGGER_NOW = 2001,
        CMD_TOGGLE_CONTINUOUS = 2002,
        CMD_TOGGLE_LOCK_RESIZE = 2003,
        CMD_CLEAR_OVERLAY = 2004,
        CMD_MODE_CUSTOM_REGION = 2005,
        CMD_OPEN_SETTINGS_UI = 2006,
        CMD_EXIT_APP = 2007,

        CMD_OCR_WINRT = 2050,
        CMD_OCR_PADDLE = 2051,

        CMD_SRC_AUTO = 2100,
        CMD_SRC_JA = 2101,
        CMD_SRC_ZH = 2102,
        CMD_SRC_KO = 2103,
        CMD_SRC_EN = 2104,
        CMD_SRC_PT = 2105,

        CMD_TGT_EN = 2200,
        CMD_TGT_PT = 2201,
        CMD_TGT_ES = 2202,
        CMD_TGT_FR = 2203,
        CMD_TGT_DE = 2204,

        CMD_WIN_BASE = 3000,
        CMD_MON_BASE = 3500
    };

    enum SettingsControlId : int {
        IDC_BTN_SAVE_APPLY = 4001,
        IDC_BTN_TOGGLE_LOCK = 4002,
        IDC_BTN_CLOSE = 4003
    };

    TrayAndSettingsUI::TrayAndSettingsUI(
        HINSTANCE hInstance,
        HWND overlayHwnd,
        TranslatorConfig& config,
        PipelineSyncState& syncState,
        SimpleCapture& capture,
        HotKeyManager& hotkeys)
        : m_hInstance(hInstance)
        , m_overlayHwnd(overlayHwnd)
        , m_config(config)
        , m_sync(syncState)
        , m_capture(capture)
        , m_hotkeys(hotkeys)
    {
        m_uiFont = CreateFontW(
            -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    }

    TrayAndSettingsUI::~TrayAndSettingsUI() {
        RemoveTrayIcon();
        if (m_uiFont) DeleteObject(m_uiFont);
    }

    void TrayAndSettingsUI::InitializeTrayIcon() {
        if (m_trayAdded) return;

        m_nid.cbSize = sizeof(NOTIFYICONDATAW);
        m_nid.hWnd = m_overlayHwnd;
        m_nid.uID = 1;
        m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        m_nid.uCallbackMessage = WM_TRAYICON;
        m_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(m_nid.szTip, L"ChromaTranslate (Right-click for Quick Config / UI)");

        Shell_NotifyIconW(NIM_ADD, &m_nid);
        m_trayAdded = true;
    }

    void TrayAndSettingsUI::RemoveTrayIcon() {
        if (!m_trayAdded) return;
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_trayAdded = false;
    }

    void TrayAndSettingsUI::ApplyOverlayClickThroughState(bool unlockedInteractive) {
        m_sync.overlayInteractive.store(unlockedInteractive, std::memory_order_relaxed);

        LONG_PTR exStyle = GetWindowLongPtrW(m_overlayHwnd, GWL_EXSTYLE);
        if (unlockedInteractive) {
            if (m_config.captureMode == CaptureMode::WindowLock) {
                RECT r{};
                GetWindowRect(m_overlayHwnd, &r);
                m_config.captureMode = CaptureMode::CustomRegion;
                m_config.regionLeft = r.left;
                m_config.regionTop = r.top;
                m_config.regionWidth = std::max<int>(100, r.right - r.left);
                m_config.regionHeight = std::max<int>(60, r.bottom - r.top);
                m_capture.UpdateConfig(m_config);
                m_capture.SwitchToCustomRegion();
            }
            exStyle &= ~WS_EX_TRANSPARENT;
            SetWindowLongPtrW(m_overlayHwnd, GWL_EXSTYLE, exStyle);
        } else {
            exStyle |= WS_EX_TRANSPARENT;
            SetWindowLongPtrW(m_overlayHwnd, GWL_EXSTYLE, exStyle);

            if (m_capture.GetCaptureMode() == CaptureMode::CustomRegion) {
                RECT r{};
                if (GetWindowRect(m_overlayHwnd, &r)) {
                    m_config.captureMode = CaptureMode::CustomRegion;
                    m_config.regionLeft = r.left;
                    m_config.regionTop = r.top;
                    m_config.regionWidth = std::max<int>(64, r.right - r.left);
                    m_config.regionHeight = std::max<int>(48, r.bottom - r.top);
                }
            }
            m_capture.OnOverlayMovedOrResized();
            BroadcastConfigUpdate();
        }

        m_capture.RenderTranslationOverlay();
    }

    void TrayAndSettingsUI::HandleTrayMessage(LPARAM lParam) {
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayContextMenu();
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            OpenSettingsWindow();
        }
    }

    void TrayAndSettingsUI::ShowTrayContextMenu() {
        HMENU hMenu = CreatePopupMenu();

        bool isCont = m_sync.continuousMode.load(std::memory_order_relaxed);
        bool isUnlocked = m_sync.overlayInteractive.load(std::memory_order_relaxed);
        CaptureMode capMode = m_capture.GetCaptureMode();

        AppendMenuW(hMenu, MF_STRING, CMD_TRIGGER_NOW, L"Translate Screen Now\tAlt+T");
        AppendMenuW(hMenu, MF_STRING | (isCont ? MF_CHECKED : 0), CMD_TOGGLE_CONTINUOUS, L"Continuous Real-Time Mode\tAlt+R");
        AppendMenuW(hMenu, MF_STRING | (isUnlocked ? MF_CHECKED : 0), CMD_TOGGLE_LOCK_RESIZE, L"Unlock Overlay (Move / Resize Box)\tAlt+L");
        AppendMenuW(hMenu, MF_STRING, CMD_CLEAR_OVERLAY, L"Clear Overlay Text\tAlt+C");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

        HMENU hCapMenu = CreatePopupMenu();
        m_cachedWindows = EnumerateCapturableWindows(m_overlayHwnd);
        m_cachedMonitors = EnumerateMonitors();

        HMENU hWinSub = CreatePopupMenu();
        for (size_t i = 0; i < std::min<size_t>(m_cachedWindows.size(), 40); ++i) {
            UINT flags = MF_STRING;
            if (capMode == CaptureMode::WindowLock && m_capture.GetTargetWindow() == m_cachedWindows[i].hwnd) {
                flags |= MF_CHECKED;
            }
            AppendMenuW(hWinSub, flags, CMD_WIN_BASE + i, m_cachedWindows[i].title.c_str());
        }

        HMENU hMonSub = CreatePopupMenu();
        for (size_t i = 0; i < m_cachedMonitors.size(); ++i) {
            UINT flags = MF_STRING;
            if (capMode == CaptureMode::MonitorFull && m_capture.GetTargetMonitor() == m_cachedMonitors[i].hmon) {
                flags |= MF_CHECKED;
            }
            AppendMenuW(hMonSub, flags, CMD_MON_BASE + i, m_cachedMonitors[i].name.c_str());
        }

        AppendMenuW(hCapMenu, MF_POPUP | (capMode == CaptureMode::WindowLock ? MF_CHECKED : 0),
            reinterpret_cast<UINT_PTR>(hWinSub), L"Window Lock (Select Window)");
        AppendMenuW(hCapMenu, MF_POPUP | (capMode == CaptureMode::MonitorFull ? MF_CHECKED : 0),
            reinterpret_cast<UINT_PTR>(hMonSub), L"Full Monitor (Select Display)");
        AppendMenuW(hCapMenu, MF_STRING | (capMode == CaptureMode::CustomRegion ? MF_CHECKED : 0),
            CMD_MODE_CUSTOM_REGION, L"Custom Resizable Region");
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hCapMenu), L"Capture Mode");

        HMENU hOcrMenu = CreatePopupMenu();
        AppendMenuW(hOcrMenu, MF_STRING | (m_config.ocrBackend == OcrBackend::WindowsMediaOcr ? MF_CHECKED : 0),
            CMD_OCR_WINRT, L"Windows.Media.Ocr (Recommended - Multi-Language)");
        AppendMenuW(hOcrMenu, MF_STRING | (m_config.ocrBackend == OcrBackend::PaddleOnnx ? MF_CHECKED : 0),
            CMD_OCR_PADDLE, L"PaddleOCR PP-OCRv4 (ONNX Local)");
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hOcrMenu), L"OCR Engine");

        HMENU hSrcMenu = CreatePopupMenu();
        AppendMenuW(hSrcMenu, MF_STRING | (m_config.sourceLang == L"auto" ? MF_CHECKED : 0), CMD_SRC_AUTO, L"Auto-Detect (auto)");
        AppendMenuW(hSrcMenu, MF_STRING | (m_config.sourceLang == L"ja" ? MF_CHECKED : 0), CMD_SRC_JA, L"Japanese (ja)");
        AppendMenuW(hSrcMenu, MF_STRING | (m_config.sourceLang == L"zh-CN" ? MF_CHECKED : 0), CMD_SRC_ZH, L"Chinese (zh-CN)");
        AppendMenuW(hSrcMenu, MF_STRING | (m_config.sourceLang == L"ko" ? MF_CHECKED : 0), CMD_SRC_KO, L"Korean (ko)");
        AppendMenuW(hSrcMenu, MF_STRING | (m_config.sourceLang == L"en" ? MF_CHECKED : 0), CMD_SRC_EN, L"English (en)");
        AppendMenuW(hSrcMenu, MF_STRING | (m_config.sourceLang == L"pt" ? MF_CHECKED : 0), CMD_SRC_PT, L"Portuguese (pt)");
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSrcMenu), L"Source Language");

        HMENU hTgtMenu = CreatePopupMenu();
        AppendMenuW(hTgtMenu, MF_STRING | (m_config.targetLang == L"en" ? MF_CHECKED : 0), CMD_TGT_EN, L"English (en)");
        AppendMenuW(hTgtMenu, MF_STRING | (m_config.targetLang == L"pt" ? MF_CHECKED : 0), CMD_TGT_PT, L"Portuguese (pt)");
        AppendMenuW(hTgtMenu, MF_STRING | (m_config.targetLang == L"es" ? MF_CHECKED : 0), CMD_TGT_ES, L"Spanish (es)");
        AppendMenuW(hTgtMenu, MF_STRING | (m_config.targetLang == L"fr" ? MF_CHECKED : 0), CMD_TGT_FR, L"French (fr)");
        AppendMenuW(hTgtMenu, MF_STRING | (m_config.targetLang == L"de" ? MF_CHECKED : 0), CMD_TGT_DE, L"German (de)");
        AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hTgtMenu), L"Target Language");

        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, CMD_OPEN_SETTINGS_UI, L"Open Configuration UI...");
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(hMenu, MF_STRING, CMD_EXIT_APP, L"Exit ChromaTranslate");

        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(m_overlayHwnd);
        UINT cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, m_overlayHwnd, nullptr);
        DestroyMenu(hMenu);

        if (cmd == 0) return;

        switch (cmd) {
        case CMD_TRIGGER_NOW:
            m_sync.triggerSingleCapture.store(true, std::memory_order_relaxed);
            break;
        case CMD_TOGGLE_CONTINUOUS:
            m_sync.continuousMode.store(!isCont, std::memory_order_relaxed);
            if (!isCont) m_sync.triggerSingleCapture.store(true, std::memory_order_relaxed);
            break;
        case CMD_TOGGLE_LOCK_RESIZE:
            ApplyOverlayClickThroughState(!isUnlocked);
            break;
        case CMD_CLEAR_OVERLAY: {
            std::lock_guard<std::mutex> lock(m_sync.overlayMutex);
            m_sync.activeBlocks.clear();
            m_capture.RenderTranslationOverlay();
            break;
        }
        case CMD_MODE_CUSTOM_REGION:
            m_config.captureMode = CaptureMode::CustomRegion;
            m_capture.UpdateConfig(m_config);
            m_capture.SwitchToCustomRegion();
            ApplyOverlayClickThroughState(true);
            BroadcastConfigUpdate();
            break;
        case CMD_OPEN_SETTINGS_UI:
            OpenSettingsWindow();
            break;
        case CMD_EXIT_APP:
            PostMessageW(m_overlayHwnd, WM_CLOSE, 0, 0);
            break;

        case CMD_OCR_WINRT:  m_config.ocrBackend = OcrBackend::WindowsMediaOcr; BroadcastConfigUpdate(); break;
        case CMD_OCR_PADDLE: m_config.ocrBackend = OcrBackend::PaddleOnnx;      BroadcastConfigUpdate(); break;

        case CMD_SRC_AUTO: m_config.sourceLang = L"auto";  BroadcastConfigUpdate(); break;
        case CMD_SRC_JA:   m_config.sourceLang = L"ja";    BroadcastConfigUpdate(); break;
        case CMD_SRC_ZH:   m_config.sourceLang = L"zh-CN"; BroadcastConfigUpdate(); break;
        case CMD_SRC_KO:   m_config.sourceLang = L"ko";    BroadcastConfigUpdate(); break;
        case CMD_SRC_EN:   m_config.sourceLang = L"en";    BroadcastConfigUpdate(); break;
        case CMD_SRC_PT:   m_config.sourceLang = L"pt";    BroadcastConfigUpdate(); break;

        case CMD_TGT_EN:   m_config.targetLang = L"en";    BroadcastConfigUpdate(); break;
        case CMD_TGT_PT:   m_config.targetLang = L"pt";    BroadcastConfigUpdate(); break;
        case CMD_TGT_ES:   m_config.targetLang = L"es";    BroadcastConfigUpdate(); break;
        case CMD_TGT_FR:   m_config.targetLang = L"fr";    BroadcastConfigUpdate(); break;
        case CMD_TGT_DE:   m_config.targetLang = L"de";    BroadcastConfigUpdate(); break;

        default:
            if (cmd >= CMD_WIN_BASE && cmd < CMD_WIN_BASE + m_cachedWindows.size()) {
                m_config.captureMode = CaptureMode::WindowLock;
                ApplyOverlayClickThroughState(false);
                m_capture.SwitchToWindow(m_cachedWindows[cmd - CMD_WIN_BASE].hwnd);
                BroadcastConfigUpdate();
            } else if (cmd >= CMD_MON_BASE && cmd < CMD_MON_BASE + m_cachedMonitors.size()) {
                m_config.captureMode = CaptureMode::MonitorFull;
                ApplyOverlayClickThroughState(false);
                m_capture.SwitchToMonitor(m_cachedMonitors[cmd - CMD_MON_BASE].hmon);
                BroadcastConfigUpdate();
            }
            break;
        }
    }

    void TrayAndSettingsUI::BroadcastConfigUpdate() {
        m_config.SaveToIni(L".\\config.ini");
        m_capture.UpdateConfig(m_config);
        {
            std::lock_guard<std::mutex> lock(m_sync.configMutex);
            m_sync.sharedConfig = m_config;
            m_sync.configUpdated.store(true, std::memory_order_release);
        }
        m_sync.triggerSingleCapture.store(true, std::memory_order_relaxed);
    }

    void TrayAndSettingsUI::OpenSettingsWindow() {
        if (m_settingsHwnd && IsWindow(m_settingsHwnd)) {
            ShowWindow(m_settingsHwnd, SW_SHOW);
            SetForegroundWindow(m_settingsHwnd);
            PopulateSettingsControls(m_settingsHwnd);
            return;
        }

        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(WNDCLASSEXW);
            wc.lpfnWndProc = SettingsWndProc;
            wc.hInstance = m_hInstance;
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            wc.lpszClassName = L"ChromaTranslateSettingsClass";
            RegisterClassExW(&wc);
            classRegistered = true;
        }

        m_settingsHwnd = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
            L"ChromaTranslateSettingsClass",
            L"ChromaTranslate - Configuration",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, 440, 480,
            nullptr, nullptr, m_hInstance, this);

        ShowWindow(m_settingsHwnd, SW_SHOW);
        SetForegroundWindow(m_settingsHwnd);
    }

    LRESULT CALLBACK TrayAndSettingsUI::SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<TrayAndSettingsUI*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<TrayAndSettingsUI*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));

            auto makeLabel = [&](const wchar_t* text, int x, int y, int w, int h) {
                HWND hCtrl = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, hwnd, nullptr, self->m_hInstance, nullptr);
                SendMessageW(hCtrl, WM_SETFONT, reinterpret_cast<WPARAM>(self->m_uiFont), TRUE);
            };
            auto makeEdit = [&](int x, int y, int w, int h) {
                HWND hCtrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, x, y, w, h, hwnd, nullptr, self->m_hInstance, nullptr);
                SendMessageW(hCtrl, WM_SETFONT, reinterpret_cast<WPARAM>(self->m_uiFont), TRUE);
                return hCtrl;
            };
            auto makeCombo = [&](int x, int y, int w, int h, DWORD style = CBS_DROPDOWN) {
                HWND hCtrl = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_HASSTRINGS | WS_VSCROLL | style, x, y, w, h, hwnd, nullptr, self->m_hInstance, nullptr);
                SendMessageW(hCtrl, WM_SETFONT, reinterpret_cast<WPARAM>(self->m_uiFont), TRUE);
                return hCtrl;
            };

            makeLabel(L"Source Language Code:", 20, 20, 180, 22);
            self->m_cmbSourceLang = makeCombo(210, 18, 190, 160);
            for (const wchar_t* s : { L"auto", L"ja", L"zh-CN", L"ko", L"en", L"pt", L"es", L"fr" })
                SendMessageW(self->m_cmbSourceLang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));

            makeLabel(L"Target Language Code:", 20, 56, 180, 22);
            self->m_cmbTargetLang = makeCombo(210, 54, 190, 160);
            for (const wchar_t* s : { L"en", L"pt", L"es", L"fr", L"de", L"ja" })
                SendMessageW(self->m_cmbTargetLang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));

            makeLabel(L"Capture Mode:", 20, 92, 180, 22);
            self->m_cmbCaptureMode = makeCombo(210, 90, 190, 120, CBS_DROPDOWNLIST);
            SendMessageW(self->m_cmbCaptureMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Window Lock (HWND)"));
            SendMessageW(self->m_cmbCaptureMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Full Monitor (HMONITOR)"));
            SendMessageW(self->m_cmbCaptureMode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Custom Resizable Region"));

            makeLabel(L"OCR Engine:", 20, 128, 180, 22);
            self->m_cmbOcrBackend = makeCombo(210, 126, 190, 120, CBS_DROPDOWNLIST);
            SendMessageW(self->m_cmbOcrBackend, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Windows.Media.Ocr (WinRT)"));
            SendMessageW(self->m_cmbOcrBackend, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"PaddleOCR PP-OCRv4 (ONNX)"));

            makeLabel(L"Continuous Poll Rate (ms):", 20, 168, 180, 22);
            self->m_edtPollMs = makeEdit(210, 166, 190, 24);

            makeLabel(L"Luma Diff Threshold (1-4096):", 20, 204, 180, 22);
            self->m_edtDiffCells = makeEdit(210, 202, 190, 24);

            makeLabel(L"Furigana Filter Height (%):", 20, 240, 180, 22);
            self->m_edtFurigana = makeEdit(210, 238, 190, 24);

            makeLabel(L"ONNX CPU Threads:", 20, 276, 180, 22);
            self->m_edtThreads = makeEdit(210, 274, 190, 24);

            self->m_chkContinuous = CreateWindowExW(0, L"BUTTON", L"Enable Continuous Real-Time Mode (Alt+R)",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 20, 314, 380, 24, hwnd, nullptr, self->m_hInstance, nullptr);
            SendMessageW(self->m_chkContinuous, WM_SETFONT, reinterpret_cast<WPARAM>(self->m_uiFont), TRUE);

            self->m_chkUnlockOverlay = CreateWindowExW(0, L"BUTTON", L"Unlock Overlay for Moving & Resizing (Alt+L)",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 20, 344, 380, 24, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_TOGGLE_LOCK)), self->m_hInstance, nullptr);
            SendMessageW(self->m_chkUnlockOverlay, WM_SETFONT, reinterpret_cast<WPARAM>(self->m_uiFont), TRUE);

            HWND btnSave = CreateWindowExW(0, L"BUTTON", L"Save & Apply Live",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 20, 386, 185, 34, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_SAVE_APPLY)), self->m_hInstance, nullptr);
            SendMessageW(btnSave, WM_SETFONT, reinterpret_cast<WPARAM>(self->m_uiFont), TRUE);

            HWND btnClose = CreateWindowExW(0, L"BUTTON", L"Close",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 215, 386, 185, 34, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_CLOSE)), self->m_hInstance, nullptr);
            SendMessageW(btnClose, WM_SETFONT, reinterpret_cast<WPARAM>(self->m_uiFont), TRUE);

            self->PopulateSettingsControls(hwnd);
            return 0;
        }

        case WM_COMMAND:
            if (!self) break;
            switch (LOWORD(wParam)) {
            case IDC_BTN_TOGGLE_LOCK: {
                bool checked = (SendMessageW(self->m_chkUnlockOverlay, BM_GETCHECK, 0, 0) == BST_CHECKED);
                self->ApplyOverlayClickThroughState(checked);
                break;
            }
            case IDC_BTN_SAVE_APPLY:
                self->SaveFromSettingsControls(hwnd);
                break;
            case IDC_BTN_CLOSE:
                DestroyWindow(hwnd);
                break;
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (self) self->m_settingsHwnd = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void TrayAndSettingsUI::PopulateSettingsControls(HWND) {
        SetWindowTextW(m_cmbSourceLang, m_config.sourceLang.c_str());
        SetWindowTextW(m_cmbTargetLang, m_config.targetLang.c_str());
        SendMessageW(m_cmbCaptureMode, CB_SETCURSEL, static_cast<WPARAM>(m_config.captureMode), 0);
        SendMessageW(m_cmbOcrBackend, CB_SETCURSEL, static_cast<WPARAM>(m_config.ocrBackend), 0);

        SetWindowTextW(m_edtPollMs, std::to_wstring(m_config.continuousPollMs).c_str());
        SetWindowTextW(m_edtDiffCells, std::to_wstring(m_config.diffThresholdCells).c_str());
        SetWindowTextW(m_edtFurigana, std::to_wstring(m_config.furiganaHeightRatioPercent).c_str());
        SetWindowTextW(m_edtThreads, std::to_wstring(m_config.cpuThreads).c_str());

        SendMessageW(m_chkContinuous, BM_SETCHECK,
            m_sync.continuousMode.load(std::memory_order_relaxed) ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(m_chkUnlockOverlay, BM_SETCHECK,
            m_sync.overlayInteractive.load(std::memory_order_relaxed) ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    void TrayAndSettingsUI::SaveFromSettingsControls(HWND) {
        wchar_t buf[256]{};
        GetWindowTextW(m_cmbSourceLang, buf, 256);
        m_config.sourceLang = buf;

        GetWindowTextW(m_cmbTargetLang, buf, 256);
        m_config.targetLang = buf;

        int selMode = static_cast<int>(SendMessageW(m_cmbCaptureMode, CB_GETCURSEL, 0, 0));
        if (selMode >= 0 && selMode <= 2) {
            CaptureMode newMode = static_cast<CaptureMode>(selMode);
            if (newMode != m_config.captureMode) {
                m_config.captureMode = newMode;
                m_capture.UpdateConfig(m_config);
                if (newMode == CaptureMode::MonitorFull) {
                    m_capture.SwitchToMonitor(nullptr);
                } else if (newMode == CaptureMode::CustomRegion) {
                    m_capture.SwitchToCustomRegion();
                }
            }
        }

        int selOcr = static_cast<int>(SendMessageW(m_cmbOcrBackend, CB_GETCURSEL, 0, 0));
        if (selOcr >= 0 && selOcr <= 1) {
            m_config.ocrBackend = static_cast<OcrBackend>(selOcr);
        }

        GetWindowTextW(m_edtPollMs, buf, 256);
        m_config.continuousPollMs = std::max<uint32_t>(100, _wtoi(buf));

        GetWindowTextW(m_edtDiffCells, buf, 256);
        m_config.diffThresholdCells = std::clamp<uint32_t>(_wtoi(buf), 1, 4096);

        GetWindowTextW(m_edtFurigana, buf, 256);
        m_config.furiganaHeightRatioPercent = std::clamp<int>(_wtoi(buf), 0, 90);

        GetWindowTextW(m_edtThreads, buf, 256);
        m_config.cpuThreads = std::clamp<int>(_wtoi(buf), 1, 8);

        bool contChecked = (SendMessageW(m_chkContinuous, BM_GETCHECK, 0, 0) == BST_CHECKED);
        m_sync.continuousMode.store(contChecked, std::memory_order_relaxed);

        bool unlockChecked = (SendMessageW(m_chkUnlockOverlay, BM_GETCHECK, 0, 0) == BST_CHECKED);
        ApplyOverlayClickThroughState(unlockChecked);

        BroadcastConfigUpdate();
    }

} // namespace ChromaTranslate