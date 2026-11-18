#include "pch.h"
#include "TranslationTypes.h"
#include "HotKeyManager.h"
#include "SimpleCapture.h"
#include "PaddleOcrEngine.h"
#include "TranslationRouter.h"
#include "TrayAndSettingsUI.h"

using namespace ChromaTranslate;

static constexpr UINT WM_OVERLAY_UPDATED = WM_USER + 101;
static constexpr UINT_PTR TIMER_SYNC_WINDOW = 1;

HWND PromptUserForTargetWindow(HWND ignoreHwnd) {
    auto windows = EnumerateCapturableWindows(ignoreHwnd);
    if (windows.empty()) return nullptr;

    HMENU hMenu = CreatePopupMenu();
    for (size_t i = 0; i < windows.size(); ++i) {
        AppendMenuW(hMenu, MF_STRING, i + 1, windows[i].title.c_str());
    }

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(ignoreHwnd);
    int selection = TrackPopupMenu(
        hMenu,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
        pt.x, pt.y, 0, ignoreHwnd, nullptr);

    DestroyMenu(hMenu);
    if (selection <= 0 || static_cast<size_t>(selection) > windows.size()) {
        return nullptr;
    }
    return windows[selection - 1].hwnd;
}

void OcrAndTranslationWorker(
    HWND overlayHwnd,
    TranslatorConfig config,
    PipelineSyncState& sync)
{
    PaddleOcrEngine ocrEngine(config);
    TranslationRouter translator(config);

    while (!sync.shutdownRequested.load(std::memory_order_relaxed)) {
        StagedFrame frame;
        {
            std::unique_lock<std::mutex> lock(sync.frameMutex);
            sync.frameCv.wait(lock, [&] {
                return sync.hasNewFrame || sync.shutdownRequested.load(std::memory_order_relaxed);
            });

            if (sync.shutdownRequested.load(std::memory_order_relaxed)) break;

            frame = std::move(sync.latestFrame);
            sync.hasNewFrame = false;
        }

        // Apply live configuration updates from System Tray or Settings UI
        if (sync.configUpdated.exchange(false, std::memory_order_acq_rel)) {
            std::lock_guard<std::mutex> lock(sync.configMutex);
            ocrEngine.UpdateConfig(sync.sharedConfig);
            translator.UpdateConfig(sync.sharedConfig);
        }

        std::vector<TextBlock> detectedBlocks = ocrEngine.ProcessFrame(frame);

        if (!detectedBlocks.empty()) {
            translator.TranslateBlocks(detectedBlocks);
        }

        {
            std::lock_guard<std::mutex> lock(sync.overlayMutex);
            sync.activeBlocks = std::move(detectedBlocks);
            sync.overlayDirty.store(true, std::memory_order_release);
        }
        PostMessageW(overlayHwnd, WM_OVERLAY_UPDATED, 0, 0);
    }
}

struct AppContext {
    HotKeyManager* hotkeys = nullptr;
    SimpleCapture* capture = nullptr;
    TrayAndSettingsUI* trayUi = nullptr;
    PipelineSyncState* sync = nullptr;
};

LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<AppContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCHITTEST: {
        if (!ctx || !ctx->sync || !ctx->sync->overlayInteractive.load(std::memory_order_relaxed)) {
            return HTTRANSPARENT;
        }
        POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT rc{};
        GetWindowRect(hwnd, &rc);

        const int border = 10;
        bool left = (pt.x < rc.left + border);
        bool right = (pt.x >= rc.right - border);
        bool top = (pt.y < rc.top + border);
        bool bottom = (pt.y >= rc.bottom - border);

        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        return HTCAPTION; // Drag anywhere inside the box to move
    }

    case WM_SIZE:
    case WM_EXITSIZEMOVE:
        if (ctx && ctx->capture) {
            ctx->capture->OnOverlayMovedOrResized();
        }
        return 0;

    case WM_HOTKEY:
        if (ctx && ctx->hotkeys) {
            ctx->hotkeys->HandleHotKeyMessage(wParam);
            if (ctx->sync && ctx->sync->overlayDirty.exchange(false, std::memory_order_acq_rel) && ctx->capture) {
                ctx->capture->RenderTranslationOverlay();
            }
        }
        return 0;

    case WM_APPLY_CLICKTHROUGH_STATE:
        if (ctx && ctx->trayUi) {
            ctx->trayUi->ApplyOverlayClickThroughState(wParam != 0);
        }
        return 0;

    case WM_TRAYICON:
        if (ctx && ctx->trayUi) {
            ctx->trayUi->HandleTrayMessage(lParam);
        }
        return 0;

    case WM_OVERLAY_UPDATED:
        if (ctx && ctx->capture && ctx->sync) {
            if (ctx->sync->overlayDirty.exchange(false, std::memory_order_acq_rel)) {
                ctx->capture->RenderTranslationOverlay();
            }
        }
        return 0;

    case WM_TIMER:
        if (wParam == TIMER_SYNC_WINDOW && ctx && ctx->capture) {
            ctx->capture->SyncOverlayPosition();
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    TranslatorConfig config = TranslatorConfig::LoadFromIni(L".\\config.ini");
    PipelineSyncState syncState;
    syncState.sharedConfig = config;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_SIZEALL);
    wc.lpszClassName = L"ChromaTranslateOverlayClass";
    RegisterClassExW(&wc);

    DWORD exStyle = WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    HWND overlayHwnd = CreateWindowExW(
        exStyle,
        wc.lpszClassName,
        L"ChromaTranslate Overlay",
        WS_POPUP,
        config.regionLeft, config.regionTop, config.regionWidth, config.regionHeight,
        nullptr, nullptr, hInstance, nullptr);

    SetWindowDisplayAffinity(overlayHwnd, WDA_EXCLUDEFROMCAPTURE);
    ShowWindow(overlayHwnd, SW_SHOW);

    HotKeyManager hotkeys(overlayHwnd, config, syncState);
    hotkeys.RegisterAll();

    SimpleCapture capture(overlayHwnd, config, syncState);
    TrayAndSettingsUI trayUi(hInstance, overlayHwnd, config, syncState, capture, hotkeys);
    trayUi.InitializeTrayIcon();

    AppContext appCtx{ &hotkeys, &capture, &trayUi, &syncState };
    SetWindowLongPtrW(overlayHwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&appCtx));

    // Initialize capture according to saved CaptureMode
    if (config.captureMode == CaptureMode::WindowLock) {
        HWND targetHwnd = PromptUserForTargetWindow(overlayHwnd);
        if (targetHwnd) {
            capture.SwitchToWindow(targetHwnd);
        } else {
            capture.SwitchToCustomRegion();
            trayUi.ApplyOverlayClickThroughState(true);
        }
    } else if (config.captureMode == CaptureMode::MonitorFull) {
        capture.SwitchToMonitor(nullptr);
    } else {
        capture.SwitchToCustomRegion();
    }

    SetTimer(overlayHwnd, TIMER_SYNC_WINDOW, 100, nullptr);

    std::thread workerThread(OcrAndTranslationWorker, overlayHwnd, config, std::ref(syncState));

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    KillTimer(overlayHwnd, TIMER_SYNC_WINDOW);
    trayUi.RemoveTrayIcon();
    syncState.shutdownRequested.store(true, std::memory_order_relaxed);
    syncState.frameCv.notify_all();
    if (workerThread.joinable()) {
        workerThread.join();
    }
    capture.Close();

    return 0;
}