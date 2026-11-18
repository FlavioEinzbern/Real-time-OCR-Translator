#pragma once
#include "TranslationTypes.h"

namespace ChromaTranslate {

    enum HotkeyActionId : int {
        HK_SINGLE_TRANSLATE = 1001,
        HK_TOGGLE_CONTINUOUS = 1002,
        HK_CLEAR_OVERLAY = 1003,
        HK_TOGGLE_LOCK = 1004
    };

    static constexpr UINT WM_APPLY_CLICKTHROUGH_STATE = WM_USER + 102;

    class HotKeyManager {
    public:
        HotKeyManager(HWND overlayHwnd, const TranslatorConfig& config, PipelineSyncState& syncState);
        ~HotKeyManager();

        bool RegisterAll();
        void UnregisterAll();
        void UpdateConfig(const TranslatorConfig& newConfig);
        void HandleHotKeyMessage(WPARAM wParam);

    private:
        HWND m_hwnd;
        TranslatorConfig m_config;
        PipelineSyncState& m_sync;
        bool m_registered = false;
    };

} // namespace ChromaTranslate