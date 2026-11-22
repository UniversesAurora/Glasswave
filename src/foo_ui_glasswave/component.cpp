#include "pch.h"
#include "gfx/composition_host.h"
#include "ui/art_cache.h"
#include "ui/main_window.h"
#include "win7/win7_resources.h"

DECLARE_COMPONENT_VERSION(
    "Glasswave", "0.1.1",
    "Glasswave - a Windows 7 Aero Glass / Windows Media Player 12 / Windows Media Center style user interface for foobar2000.\n"
    "\n"
    "The glass is scoped to this window: Windows.UI.Composition HostBackdropBrush plus the Win7 colorization recipe.\n"
    "No system theme changes, no DWM injection or hooking.\n"
    "\n"
    "Requires Windows 11 (build 22000 or later).");

VALIDATE_COMPONENT_FILENAME("foo_ui_glasswave.dll");

namespace {

// {B6A3E5F1-4C2D-4E7A-9F1B-3D6C8A2E5F71}
constexpr GUID kGlasswaveUiGuid = {0xb6a3e5f1, 0x4c2d, 0x4e7a, {0x9f, 0x1b, 0x3d, 0x6c, 0x8a, 0x2e, 0x5f, 0x71}};

class GlasswaveUserInterface : public user_interface_v2 {
public:
    const char* get_name() override { return "Glasswave"; }

    HWND init(HookProc_t hook) override {
        try {
            m_window = std::make_unique<glasswave::ui::MainWindow>();
            return m_window->Create(hook);
        } catch (const winrt::hresult_error& e) {
            m_window.reset();
            pfc::string8 msg = "Glasswave failed to initialize: ";
            msg += pfc::stringcvt::string_utf8_from_wide(e.message().c_str());
            throw pfc::exception(msg);
        }
    }

    void shutdown() override {
        if (m_window) {
            m_window->Destroy();
            m_window.reset();
        }
        glasswave::ui::ArtCache::Shutdown();     // stops the album art thread
        glasswave::win7::Resources::Shutdown();  // device bitmaps before the device
        glasswave::gfx::CompositionHost::Shutdown();
    }

    void activate() override {
        if (HWND h = Hwnd()) {
            if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
            else ShowWindow(h, SW_SHOW);
            SetForegroundWindow(h);
        }
    }

    void hide() override {
        if (HWND h = Hwnd()) ShowWindow(h, SW_MINIMIZE);
    }

    bool is_visible() override {
        HWND h = Hwnd();
        return h && IsWindowVisible(h) && !IsIconic(h);
    }

    GUID get_guid() override { return kGlasswaveUiGuid; }

    // TODO(M1): WMP 12 has no status bar; show these as transient hints in the transport band.
    void override_statusbar_text(const char*) override {}
    void revert_statusbar_text() override {}
    void show_now_playing() override {}

    bool query_capability(const GUID&) override { return false; }

private:
    HWND Hwnd() const { return m_window ? m_window->Hwnd() : nullptr; }

    std::unique_ptr<glasswave::ui::MainWindow> m_window;
};

user_interface_factory<GlasswaveUserInterface> g_glasswaveUiFactory;

}  // namespace
