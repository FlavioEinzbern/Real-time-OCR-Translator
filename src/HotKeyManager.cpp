#include "HotKeyManager.h"

namespace ChromaTranslate {

    HotKeyManager::HotKeyManager(HWND overlayHwnd, const TranslatorConfig& config, PipelineSyncState& syncState)
        : m_hwnd(overlayHwnd), m_config(config), m_sync(syncState) {}

    HotKeyManager::~HotKeyManager() {
        UnregisterAll();
    }

    bool HotKeyManager::RegisterAll() {
        if (m_registered) return true;

        UINT mods = m_config.hotkeyModifiers | MOD_NOREPEAT;
        bool ok1 = RegisterHotKey(m_hwnd, HK_SINGLE_TRANSLATE, mods, m_config.keySingleTranslate);
        bool ok2 = RegisterHotKey(m_hwnd, HK_TOGGLE_CONTINUOUS, mods, m_config.keyToggleContinuous);
        bool ok3 = RegisterHotKey(m_hwnd, HK_CLEAR_OVERLAY, mods, m_config.keyClearOverlay);
        bool ok4 = RegisterHotKey(m_hwnd, HK_TOGGLE_LOCK, mods, m_config.keyToggleLock);

        m_registered = (ok1 && ok2 && ok3 && ok4);
        return m_registered;
    }

    void HotKeyManager::UnregisterAll() {
        if (!m_registered) return;
        UnregisterHotKey(m_hwnd, HK_SINGLE_TRANSLATE);
        UnregisterHotKey(m_hwnd, HK_TOGGLE_CONTINUOUS);
        UnregisterHotKey(m_hwnd, HK_CLEAR_OVERLAY);
        UnregisterHotKey(m_hwnd, HK_TOGGLE_LOCK);
        m_registered = false;
    }

    void HotKeyManager::UpdateConfig(const TranslatorConfig& newConfig) {
        UnregisterAll();
        m_config = newConfig;
        RegisterAll();
    }

    void HotKeyManager::HandleHotKeyMessage(WPARAM wParam) {
        switch (static_cast<int>(wParam)) {
        case HK_SINGLE_TRANSLATE:
            m_sync.triggerSingleCapture.store(true, std::memory_order_relaxed);
            break;

        case HK_TOGGLE_CONTINUOUS: {
            bool newState = !m_sync.continuousMode.load(std::memory_order_relaxed);
            m_sync.continuousMode.store(newState, std::memory_order_relaxed);
            if (newState) {
                m_sync.triggerSingleCapture.store(true, std::memory_order_relaxed);
            }
            break;
        }

        case HK_CLEAR_OVERLAY: {
            std::lock_guard<std::mutex> lock(m_sync.overlayMutex);
            m_sync.activeBlocks.clear();
            m_sync.overlayDirty.store(true, std::memory_order_release);
            break;
        }

        case HK_TOGGLE_LOCK: {
            bool unlocked = !m_sync.overlayInteractive.load(std::memory_order_relaxed);
            m_sync.overlayInteractive.store(unlocked, std::memory_order_relaxed);
            PostMessageW(m_hwnd, WM_APPLY_CLICKTHROUGH_STATE, unlocked ? 1 : 0, 0);
            break;
        }
        default:
            break;
        }
    }

} // namespace ChromaTranslate