#define WLR_USE_UNSTABLE

#include <unistd.h>

#include <hyprland/src/includes.hpp>
#include <sstream>

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Window.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#undef private

#include <hyprutils/string/VarList.hpp>
using namespace Hyprutils::String;

#include "globals.hpp"
#include "OrthoLayout.hpp"

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION()
{
    return HYPRLAND_API_VERSION;
}

UP<COrthoLayout> g_pOrthoLayout;

//

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle)
{
    PHANDLE = handle;

    const std::string HASH = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH)
    {
        HyprlandAPI::addNotification(PHANDLE, "[ortho] Failure in initialization: Version mismatch (headers ver is not equal to running hyprland ver)",
                                     CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hs] Version mismatch");
    }

    bool success = true;

    g_pOrthoLayout = makeUnique<COrthoLayout>();
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:ortho:main_stack_percent", Hyprlang::FLOAT{0.5F});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:ortho:main_stack_min", Hyprlang::INT{1});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:ortho:main_stack_side", Hyprlang::STRING{"left"});
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:ortho:main_weight_overrides", Hyprlang::STRING{""});
    HyprlandAPI::addLayout(PHANDLE, "ortho", g_pOrthoLayout.get());

    if (success)
        HyprlandAPI::addNotification(PHANDLE, "[ortho] Initialized successfully!", CHyprColor{0.2, 1.0, 0.2, 1.0}, 5000);
    else
    {
        HyprlandAPI::addNotification(PHANDLE, "[ortho] Failure in initialization: failed to register dispatchers", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[hs] Dispatchers failed");
    }

    return {"ortho", "A plugin to add orthogonal stacks to Hyprland", "Shawn", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT()
{
    HyprlandAPI::removeLayout(PHANDLE, g_pOrthoLayout.get());
    g_pOrthoLayout.reset();
}
