#pragma once

#include <vector>
#include <list>
#include <unordered_map>
#include <any>
#include <hyprland/src/layout/IHyprLayout.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/managers/HookSystemManager.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/varlist/VarList.hpp>

enum eFullscreenMode : int8_t;

//orientation determines which side of the screen the main stack resides
enum eMainSide : uint8_t {
    MAIN_SIDE_LEFT = 0,
    MAIN_SIDE_RIGHT
};

enum eOrthoStatus {
    ORTHOSTATUS_MAIN,
    ORTHOSTATUS_SECONDARY,
};

struct SOrthoNodeData {
    // many traits inferred from membership
    PHLWINDOWREF pWindow;

    Vector2D     position;
    Vector2D     size;

    bool         ignoreFullscreenChecks = false;

    bool         operator==(const SOrthoNodeData& rhs) const {
        return pWindow.lock() == rhs.pWindow.lock();
    }
    double weight = 1;
};

struct SOrthoWorkspaceData {
    // workspace inferred from membership
    double      percMainStack = 0.5;
    int         mainStackMin  = 1;
    WORKSPACEID workspaceID   = WORKSPACE_INVALID;
    eMainSide   mainSide      = MAIN_SIDE_LEFT;

    bool        operator==(const SOrthoWorkspaceData& rhs) const {
        return workspaceID == rhs.workspaceID;
    }
};

class COrthoLayout : public IHyprLayout {
  public:
    virtual void                     onWindowCreatedTiling(PHLWINDOW, eDirection direction = DIRECTION_DEFAULT);
    virtual void                     onWindowRemovedTiling(PHLWINDOW);
    virtual bool                     isWindowTiled(PHLWINDOW);
    virtual void                     recalculateMonitor(const MONITORID&);
    virtual void                     recalculateWindow(PHLWINDOW);
    virtual void                     resizeActiveWindow(const Vector2D&, eRectCorner corner = CORNER_NONE, PHLWINDOW pWindow = nullptr);
    virtual void                     fullscreenRequestForWindow(PHLWINDOW pWindow, const eFullscreenMode CURRENT_EFFECTIVE_MODE, const eFullscreenMode EFFECTIVE_MODE);
    virtual std::any                 layoutMessage(SLayoutMessageHeader, std::string);
    virtual SWindowRenderLayoutHints requestRenderHints(PHLWINDOW);
    virtual void                     switchWindows(PHLWINDOW, PHLWINDOW);
    virtual void                     moveWindowTo(PHLWINDOW, const std::string& dir, bool silent);
    virtual void                     alterSplitRatio(PHLWINDOW, float, bool);
    virtual std::string              getLayoutName();
    virtual void                     replaceWindowDataWith(PHLWINDOW from, PHLWINDOW to);
    virtual Vector2D                 predictSizeForNewWindowTiled();
    virtual PHLWINDOW                getNextWindowCandidate(PHLWINDOW pWindow);

    virtual void                     onEnable();
    virtual void                     onDisable();

    // Returns whether the given window is marked as master in this layout.
    bool isWindowInMainStack(PHLWINDOW pWindow);

  private:
    std::unordered_map<WORKSPACEID, SOrthoWorkspaceData>                  m_orthoWorkspaceDataByWorkspace;
    std::unordered_map<WORKSPACEID, std::vector<SOrthoNodeData>>          m_mainStackByWorkspace;
    std::unordered_map<WORKSPACEID, std::vector<SOrthoNodeData>>          m_secondaryStackByWorkspace;

    bool                                                                  m_forceWarps = false;
    bool                                                                  inMain(SOrthoNodeData*);
    void                                                                  applyNodeDataToWindow(SOrthoNodeData*, const WORKSPACEID& ws);
    std::optional<std::tuple<SOrthoNodeData*, WORKSPACEID, eOrthoStatus>> getNodeFromWindow(PHLWINDOW pWindow);
    int                                                                   getNodeCountOnWorkspace(const WORKSPACEID& ws);
    int                                                                   getSecondaryStackSize(const WORKSPACEID& ws);
    int                                                                   getMainStackSize(const WORKSPACEID& ws);
    SOrthoNodeData*                                                       getOrthoNodeOnWorkspace(const WORKSPACEID&);
    SOrthoWorkspaceData*                                                  getOrthoWorkspaceData(const WORKSPACEID&);
    void                                                                  calculateWorkspace(PHLWORKSPACE);
    SOrthoNodeData*                                                       getMainStackTop(const WORKSPACEID& ws);

    SOrthoNodeData*                                                       getSecondaryStackTop(const WORKSPACEID& ws);

    friend struct SOrthoNodeData;
    friend struct SOrthoWorkspaceData;
};

template <typename CharT>
struct std::formatter<SOrthoNodeData*, CharT> : std::formatter<CharT> {
    template <typename FormatContext>
    auto format(const SOrthoNodeData* const& node, FormatContext& ctx) const {
        auto out = ctx.out();
        if (!node)
            return std::format_to(out, "[Node nullptr]");
        std::format_to(out, "[Node {:x}:, pos: {:j2}, size: {:j2}", rc<uintptr_t>(node), node->position, node->size);
        if (!node->pWindow.expired())
            std::format_to(out, ", window: {:x}", node->pWindow.lock());
        return std::format_to(out, "]");
    }
};