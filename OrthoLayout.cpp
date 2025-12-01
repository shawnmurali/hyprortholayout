#include <algorithm>
#include <ranges>
#include <optional>
#include <tuple>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/LayoutManager.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/xwayland/XWayland.hpp>
#include <hyprland/src/desktop/Workspace.hpp>

#include <hyprutils/string/ConstVarList.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include "OrthoLayout.hpp"
#include "Utils.hpp"

std::optional<SNodeLookupResult> COrthoLayout::getNodeFromWindow(PHLWINDOW pWindow)
{
    for (auto &[ws, nodes] : m_mainStackByWorkspace)
    {
        for (auto &nd : nodes)
        {
            if (nd.pWindow.lock() == pWindow)
                return SNodeLookupResult(&nd, ws, ORTHOSTATUS_MAIN);
        }
    }

    for (auto &[ws, nodes] : m_secondaryStackByWorkspace)
    {
        for (auto &nd : nodes)
        {
            if (nd.pWindow.lock() == pWindow)
                return SNodeLookupResult(&nd, ws, ORTHOSTATUS_SECONDARY);
        }
    }

    return std::nullopt;
}

int COrthoLayout::getNodeCountOnWorkspace(const WORKSPACEID &ws)
{
    return getSecondaryStackSize(ws) + getMainStackSize(ws);
}

int COrthoLayout::getSecondaryStackSize(const WORKSPACEID &ws)
{
    return m_secondaryStackByWorkspace[ws].size();
}

int COrthoLayout::getMainStackSize(const WORKSPACEID &ws)
{
    return m_mainStackByWorkspace[ws].size();
}

std::optional<std::vector<double>> parseOverrideWeights(CVarList tokens, size_t start, size_t end)
{
    std::vector<double> overrideWeights;

    for (int i = std::max(start, size_t(0)); i < std::min(end, tokens.size()); ++i)
    {
        try
        {
            float overrideWeight = std::stof(std::string{tokens[i]});
            overrideWeights.push_back(overrideWeight);
        }
        catch (const std::invalid_argument &e)
        {
            Debug::log(ERR, "layoutmsg overrideweight passed a non-float{}", e.what());
            return std::nullopt;
        }
        catch (const std::out_of_range &e)
        {
            Debug::log(ERR, "layoutmsg overrideweight passed a float outofrange {}", e.what());
            return std::nullopt;
        }
    }

    return overrideWeights;
}

std::optional<std::vector<double>> parseOverrideWeights(CVarList tokens)
{
    return parseOverrideWeights(tokens, size_t(0), tokens.size());
}

SOrthoWorkspaceData *COrthoLayout::getOrthoWorkspaceData(const WORKSPACEID &ws)
{
    auto it = m_orthoWorkspaceDataByWorkspace.find(ws);
    if (it != m_orthoWorkspaceDataByWorkspace.end())
    {
        return &it->second;
    }

    // create on the fly if it doesn't exist yet
    static auto PMAINSIDE = CConfigValue<std::string>("plugin:ortho:main_stack_side");
    static auto PMAINPERCENT = CConfigValue<Hyprlang::FLOAT>("plugin:ortho:main_stack_percent");
    static auto PMAINSTACKMIN = CConfigValue<Hyprlang::INT>("plugin:ortho:main_stack_min");
    static auto PMAINSTACKOVERRIDES = CConfigValue<Hyprlang::STRING>("plugin:ortho:main_weight_overrides");

    SOrthoWorkspaceData workspaceData;
    // comes in as quoted csv
    auto weights = std::string(*PMAINSTACKOVERRIDES);
    const auto RESULT = parseOverrideWeights(CVarList(weights.substr(1, weights.length() - 2), 0, ','));

    if (RESULT.has_value())
    {
        workspaceData.overrideMainWeights = true;
        workspaceData.mainWeightOverrides = *RESULT;
        Debug::log(LOG, "Successfully parsed override weights.");
    }
    else
    {
        Debug::log(ERR, "Error parsing main override weights.");
    }

    if (*PMAINSIDE == "right")
        workspaceData.mainSide = MAIN_SIDE_RIGHT;
    else
        workspaceData.mainSide = MAIN_SIDE_LEFT;

    workspaceData.percMainStack = std::clamp(*PMAINPERCENT, 0.1f, 0.9f);
    workspaceData.workspaceID = ws;
    workspaceData.mainStackMin = *PMAINSTACKMIN <= 0 ? 1 : *PMAINSTACKMIN;
    m_orthoWorkspaceDataByWorkspace[ws] = workspaceData;
    return &m_orthoWorkspaceDataByWorkspace[ws];
}

std::string COrthoLayout::getLayoutName()
{
    return "OrthoStack";
}

SOrthoNodeData *COrthoLayout::getMainStackTop(const WORKSPACEID &ws)
{

    auto mainStack = m_mainStackByWorkspace[ws];

    if (mainStack.empty())
    {
        return nullptr;
    }

    return &mainStack.back();
}

SOrthoNodeData *COrthoLayout::getSecondaryStackTop(const WORKSPACEID &ws)
{

    auto secondaryStack = m_secondaryStackByWorkspace[ws];

    if (secondaryStack.empty())
    {
        return nullptr;
    }

    return &secondaryStack.back();
}

void COrthoLayout::onWindowCreatedTiling(PHLWINDOW pWindow, eDirection direction)
{
    if (pWindow->m_isFloating)
        return;

    const auto PMONITOR = pWindow->m_monitor.lock();
    const auto PWORKSPACEID = pWindow->workspaceID();

    static auto PMAINSTACKMIN = CConfigValue<Hyprlang::INT>("plugin:ortho:main_stack_min");
    const int mainStackMinimum = *PMAINSTACKMIN >= 1 ? *PMAINSTACKMIN : 1;

    const auto PORTHOWORKSPACEDATA = getOrthoWorkspaceData(PWORKSPACEID);

    SOrthoNodeData node{
        .pWindow = pWindow,
    };

    // add to mainStack if not yet satisfied
    if (getMainStackSize(PWORKSPACEID) < mainStackMinimum)
    {
        m_mainStackByWorkspace[PWORKSPACEID].push_back(node);
    }
    else
    {
        m_secondaryStackByWorkspace[PWORKSPACEID].push_back(node);
    }
    recalculateMonitor(pWindow->monitorID());
    pWindow->m_workspace->updateWindows();
}

void COrthoLayout::onWindowRemovedTiling(PHLWINDOW pWindow)
{
    auto result = getNodeFromWindow(pWindow);
    if (!result.has_value())
    {
        return;
    }

    const auto &[nd, ws, status] = *result;

    auto &MAINSTACK = m_mainStackByWorkspace[ws];
    auto &SECONDARYSTACK = m_secondaryStackByWorkspace[ws];
    static auto PMAINSTACKMIN = CConfigValue<Hyprlang::INT>("plugin:ortho:main_stack_min");
    pWindow->m_ruleApplicator->resetProps(Desktop::Rule::RULE_PROP_ALL, Desktop::Types::PRIORITY_LAYOUT);
    pWindow->updateWindowData();

    if (status == ORTHOSTATUS_MAIN)
    {
        std::erase(MAINSTACK, *nd);
    }
    else
    {
        std::erase(SECONDARYSTACK, *nd);
    }

    if (pWindow->isFullscreen())
        g_pCompositor->setWindowFullscreenInternal(pWindow, FSMODE_NONE);

    if (status == ORTHOSTATUS_MAIN && MAINSTACK.size() < *PMAINSTACKMIN && !SECONDARYSTACK.empty())
    {
        MAINSTACK.push_back(SECONDARYSTACK.back());
        SECONDARYSTACK.pop_back();
    }
    recalculateMonitor(pWindow->monitorID());
    pWindow->m_workspace->updateWindows();
}

void COrthoLayout::recalculateMonitor(const MONITORID &monid)
{
    const auto PMONITOR = g_pCompositor->getMonitorFromID(monid);

    if (!PMONITOR || !PMONITOR->m_activeWorkspace)
        return;

    g_pHyprRenderer->damageMonitor(PMONITOR);

    if (PMONITOR->m_activeSpecialWorkspace)
        calculateWorkspace(PMONITOR->m_activeSpecialWorkspace);

    calculateWorkspace(PMONITOR->m_activeWorkspace);

#ifndef NO_XWAYLAND
    CBox box = g_pCompositor->calculateX11WorkArea();
    if (!g_pXWayland || !g_pXWayland->m_wm)
        return;
    g_pXWayland->m_wm->updateWorkArea(box.x, box.y, box.w, box.h);
#endif
}

void COrthoLayout::calculateWorkspace(PHLWORKSPACE pWorkspace)
{
    const auto PMONITOR = pWorkspace->m_monitor.lock();
    const auto WSSIZE = PMONITOR->m_size - PMONITOR->m_reservedTopLeft - PMONITOR->m_reservedBottomRight;
    const auto WSPOS = PMONITOR->m_position + PMONITOR->m_reservedTopLeft;
    const auto WS = pWorkspace->m_id;
    const auto WORKSPACEDATA = getOrthoWorkspaceData(WS);
    const bool BISRIGHT = WORKSPACEDATA->mainSide == MAIN_SIDE_RIGHT;
    const bool BOVERRIDEMAIN = WORKSPACEDATA->overrideMainWeights;
    const auto OVERRIDEWEIGHTS = WORKSPACEDATA->mainWeightOverrides;
    if (!PMONITOR)
        return;

    if (pWorkspace->m_hasFullscreenWindow)
    {
        // massive hack from the fullscreen func
        const auto PFULLWINDOW = pWorkspace->getFullscreenWindow();

        if (pWorkspace->m_fullscreenMode == FSMODE_FULLSCREEN)
        {
            *PFULLWINDOW->m_realPosition = PMONITOR->m_position;
            *PFULLWINDOW->m_realSize = PMONITOR->m_size;
        }
        else if (pWorkspace->m_fullscreenMode == FSMODE_MAXIMIZED)
        {
            SOrthoNodeData fakeNode;
            fakeNode.pWindow = PFULLWINDOW;
            fakeNode.position = PMONITOR->m_position + PMONITOR->m_reservedTopLeft;
            fakeNode.size = PMONITOR->m_size - PMONITOR->m_reservedTopLeft - PMONITOR->m_reservedBottomRight;
            PFULLWINDOW->m_position = fakeNode.position;
            PFULLWINDOW->m_size = fakeNode.size;
            fakeNode.ignoreFullscreenChecks = true;

            applyNodeDataToWindow(&fakeNode, pWorkspace->m_id);
        }

        // if has fullscreen, don't calculate the rest
        return;
    }
    auto &MAINSTACK = m_mainStackByWorkspace[WS];
    auto &SECONDARYSTACK = m_secondaryStackByWorkspace[WS];

    if (MAINSTACK.empty())
        return;

    // calculate the main stack
    double widthToSplit = SECONDARYSTACK.empty() ? WSSIZE.x : WSSIZE.x * WORKSPACEDATA->percMainStack;
    double totalWeight = 0.0;             // each master is guaranteed to have a weight greater than 0
    double remainingWidth = widthToSplit; // take care of rounding errors in the last window

    if (!BOVERRIDEMAIN)
    {
        for (auto &nd : MAINSTACK)
        {
            totalWeight += nd.weight;
        }
    }
    else
    {
        for (int i = 0; i < std::min(OVERRIDEWEIGHTS.size(), MAINSTACK.size()); ++i)
        {
            totalWeight += OVERRIDEWEIGHTS[i];
        }
        if (OVERRIDEWEIGHTS.size() < MAINSTACK.size())
            totalWeight += MAINSTACK.size() - OVERRIDEWEIGHTS.size();
    }
    // bottom of main stack is right next to the secondary stack
    // iteration is happening in reverse stack order
    // start drawing from the inside

    double nextX = BISRIGHT ? WSSIZE.x - widthToSplit : widthToSplit;
    auto weights_it = OVERRIDEWEIGHTS.begin();

    for (auto &nd : MAINSTACK)
    {
        const double WEIGHT = !BOVERRIDEMAIN ? nd.weight : weights_it == OVERRIDEWEIGHTS.end() ? 1
                                                                                               : *weights_it;
        const double WIDTH = std::min(widthToSplit * WEIGHT / totalWeight, remainingWidth);

        if (!BISRIGHT)
            nextX -= WIDTH;

        nd.size = Vector2D(WIDTH, WSSIZE.y);
        nd.position = WSPOS + Vector2D(nextX, 0.0);

        if (BISRIGHT)
            nextX += WIDTH;

        remainingWidth -= WIDTH;
        applyNodeDataToWindow(&nd, pWorkspace->m_id);
        if (weights_it != OVERRIDEWEIGHTS.end())
            ++weights_it;
    }

    if (SECONDARYSTACK.empty())
        return;

    totalWeight = 0.0;

    for (auto &nd : SECONDARYSTACK)
    {
        totalWeight += nd.weight;
    }

    // secondary stack is top of stack on top of screen
    // start drawing from the bottom
    nextX = BISRIGHT ? 0 : widthToSplit;
    double nextY = WSSIZE.y;
    const double WIDTH = WSSIZE.x - widthToSplit;
    const double remainingHeight = WSSIZE.y;
    for (auto &nd : SECONDARYSTACK)
    {
        const double HEIGHT = std::min(WSSIZE.y * nd.weight / totalWeight, remainingHeight);
        nextY -= HEIGHT;

        nd.size = Vector2D(WIDTH, HEIGHT);
        nd.position = WSPOS + Vector2D(nextX, nextY);

        applyNodeDataToWindow(&nd, pWorkspace->m_id);
    }
}

void COrthoLayout::applyNodeDataToWindow(SOrthoNodeData *pNode, const WORKSPACEID &ws)
{
    PHLMONITOR PMONITOR = nullptr;

    if (g_pCompositor->isWorkspaceSpecial(ws))
    {
        for (auto const &m : g_pCompositor->m_monitors)
        {
            if (m->activeSpecialWorkspaceID() == ws)
            {
                PMONITOR = m;
                break;
            }
        }
    }
    else
        PMONITOR = g_pCompositor->getWorkspaceByID(ws)->m_monitor.lock();

    if (!PMONITOR)
    {
        Debug::log(ERR, "Orphaned Node {}!!", pNode);
        return;
    }

    // for gaps outer
    const bool DISPLAYLEFT = STICKS(pNode->position.x, PMONITOR->m_position.x + PMONITOR->m_reservedTopLeft.x);
    const bool DISPLAYRIGHT = STICKS(pNode->position.x + pNode->size.x, PMONITOR->m_position.x + PMONITOR->m_size.x - PMONITOR->m_reservedBottomRight.x);
    const bool DISPLAYTOP = STICKS(pNode->position.y, PMONITOR->m_position.y + PMONITOR->m_reservedTopLeft.y);
    const bool DISPLAYBOTTOM = STICKS(pNode->position.y + pNode->size.y, PMONITOR->m_position.y + PMONITOR->m_size.y - PMONITOR->m_reservedBottomRight.y);

    const auto PWINDOW = pNode->pWindow.lock();
    // get specific gaps and rules for this workspace,
    // if user specified them in config
    const auto WORKSPACERULE = g_pConfigManager->getWorkspaceRuleFor(PWINDOW->m_workspace);

    if (PWINDOW->isFullscreen() && !pNode->ignoreFullscreenChecks)
        return;

    PWINDOW->m_ruleApplicator->resetProps(Desktop::Rule::RULE_PROP_ALL, Desktop::Types::PRIORITY_LAYOUT);
    PWINDOW->updateWindowData();

    static auto PANIMATE = CConfigValue<Hyprlang::INT>("misc:animate_manual_resizes");
    static auto PGAPSINDATA = CConfigValue<Hyprlang::CUSTOMTYPE>("general:gaps_in");
    static auto PGAPSOUTDATA = CConfigValue<Hyprlang::CUSTOMTYPE>("general:gaps_out");
    auto *PGAPSIN = sc<CCssGapData *>((PGAPSINDATA.ptr())->getData());
    auto *PGAPSOUT = sc<CCssGapData *>((PGAPSOUTDATA.ptr())->getData());

    auto gapsIn = WORKSPACERULE.gapsIn.value_or(*PGAPSIN);
    auto gapsOut = WORKSPACERULE.gapsOut.value_or(*PGAPSOUT);

    if (!validMapped(PWINDOW))
    {
        Debug::log(ERR, "Node {} holding invalid {}!!", pNode, PWINDOW);
        return;
    }

    PWINDOW->m_size = pNode->size;
    PWINDOW->m_position = pNode->position;

    PWINDOW->updateWindowDecos();

    auto calcPos = PWINDOW->m_position;
    auto calcSize = PWINDOW->m_size;

    const auto OFFSETTOPLEFT = Vector2D(sc<double>(DISPLAYLEFT ? gapsOut.m_left : gapsIn.m_left), sc<double>(DISPLAYTOP ? gapsOut.m_top : gapsIn.m_top));

    const auto OFFSETBOTTOMRIGHT = Vector2D(sc<double>(DISPLAYRIGHT ? gapsOut.m_right : gapsIn.m_right), sc<double>(DISPLAYBOTTOM ? gapsOut.m_bottom : gapsIn.m_bottom));

    calcPos = calcPos + OFFSETTOPLEFT;
    calcSize = calcSize - OFFSETTOPLEFT - OFFSETBOTTOMRIGHT;

    const auto RESERVED = PWINDOW->getFullWindowReservedArea();
    calcPos = calcPos + RESERVED.topLeft;
    calcSize = calcSize - (RESERVED.topLeft + RESERVED.bottomRight);

    Vector2D availableSpace = calcSize;

    static auto PCLAMP_TILED = CConfigValue<Hyprlang::INT>("misc:size_limits_tiled");

    if (*PCLAMP_TILED)
    {
        const auto borderSize = PWINDOW->getRealBorderSize();
        Vector2D monitorAvailable = PMONITOR->m_size - PMONITOR->m_reservedTopLeft - PMONITOR->m_reservedBottomRight -
                                    Vector2D{(double)(gapsOut.m_left + gapsOut.m_right), (double)(gapsOut.m_top + gapsOut.m_bottom)} - Vector2D{2.0 * borderSize, 2.0 * borderSize};

        Vector2D minSize = PWINDOW->m_ruleApplicator->minSize().valueOr(Vector2D{MIN_WINDOW_SIZE, MIN_WINDOW_SIZE}).clamp(Vector2D{0, 0}, monitorAvailable);
        Vector2D maxSize = PWINDOW->isFullscreen() ? Vector2D{INFINITY, INFINITY} : PWINDOW->m_ruleApplicator->maxSize().valueOr(Vector2D{INFINITY, INFINITY}).clamp(Vector2D{0, 0}, monitorAvailable);
        calcSize = calcSize.clamp(minSize, maxSize);

        calcPos += (availableSpace - calcSize) / 2.0;

        calcPos.x = std::clamp(calcPos.x, PMONITOR->m_position.x + PMONITOR->m_reservedTopLeft.x + gapsOut.m_left + borderSize,
                               PMONITOR->m_size.x + PMONITOR->m_position.x - PMONITOR->m_reservedBottomRight.x - gapsOut.m_right - calcSize.x - borderSize);
        calcPos.y = std::clamp(calcPos.y, PMONITOR->m_position.y + PMONITOR->m_reservedTopLeft.y + gapsOut.m_top + borderSize,
                               PMONITOR->m_size.y + PMONITOR->m_position.y - PMONITOR->m_reservedBottomRight.y - gapsOut.m_bottom - calcSize.y - borderSize);
    }

    if (PWINDOW->onSpecialWorkspace() && !PWINDOW->isFullscreen())
    {
        CBox wb = {calcPos + (calcSize - calcSize) / 2.f, calcSize};
        wb.round(); // avoid rounding mess

        *PWINDOW->m_realPosition = wb.pos();
        *PWINDOW->m_realSize = wb.size();
    }
    else
    {
        CBox wb = {calcPos, calcSize};
        wb.round(); // avoid rounding mess

        *PWINDOW->m_realPosition = wb.pos();
        *PWINDOW->m_realSize = wb.size();
    }

    if (m_forceWarps && !*PANIMATE)
    {
        g_pHyprRenderer->damageWindow(PWINDOW);

        PWINDOW->m_realPosition->warp();
        PWINDOW->m_realSize->warp();

        g_pHyprRenderer->damageWindow(PWINDOW);
    }

    PWINDOW->updateWindowDecos();
}

bool COrthoLayout::isWindowTiled(PHLWINDOW pWindow)
{
    return getNodeFromWindow(pWindow).has_value();
}

// TODO: maybe, I think this may be doable in a way that makes sense if we use the deltas to affect weights
void COrthoLayout::resizeActiveWindow(const Vector2D &pixResize, eRectCorner corner, PHLWINDOW pWindow) {}

void COrthoLayout::fullscreenRequestForWindow(PHLWINDOW pWindow, const eFullscreenMode CURRENT_EFFECTIVE_MODE, const eFullscreenMode EFFECTIVE_MODE)
{
    const auto PMONITOR = pWindow->m_monitor.lock();
    const auto PWORKSPACE = pWindow->m_workspace;

    // save position and size if floating
    if (pWindow->m_isFloating && CURRENT_EFFECTIVE_MODE == FSMODE_NONE)
    {
        pWindow->m_lastFloatingSize = pWindow->m_realSize->goal();
        pWindow->m_lastFloatingPosition = pWindow->m_realPosition->goal();
        pWindow->m_position = pWindow->m_realPosition->goal();
        pWindow->m_size = pWindow->m_realSize->goal();
    }

    if (EFFECTIVE_MODE == FSMODE_NONE)
    {
        // if it got its fullscreen disabled, set back its node if it had one
        const auto result = getNodeFromWindow(pWindow);
        if (result.has_value())
        {
            const auto &[nd, ws, _] = *result;
            applyNodeDataToWindow(nd, ws);
        }
        else
        {
            // get back its' dimensions from position and size
            *pWindow->m_realPosition = pWindow->m_lastFloatingPosition;
            *pWindow->m_realSize = pWindow->m_lastFloatingSize;

            pWindow->m_ruleApplicator->resetProps(Desktop::Rule::RULE_PROP_ALL, Desktop::Types::PRIORITY_LAYOUT);
            pWindow->updateWindowData();
        }
    }
    else
    {
        // apply new pos and size being monitors' box
        if (EFFECTIVE_MODE == FSMODE_FULLSCREEN)
        {
            *pWindow->m_realPosition = PMONITOR->m_position;
            *pWindow->m_realSize = PMONITOR->m_size;
        }
        else
        {
            // This is a massive hack.
            // We make a fake "only" node and apply
            // To keep consistent with the settings without C+P code

            SOrthoNodeData fakeNode;
            fakeNode.pWindow = pWindow;
            fakeNode.position = PMONITOR->m_position + PMONITOR->m_reservedTopLeft;
            fakeNode.size = PMONITOR->m_size - PMONITOR->m_reservedTopLeft - PMONITOR->m_reservedBottomRight;
            pWindow->m_position = fakeNode.position;
            pWindow->m_size = fakeNode.size;
            fakeNode.ignoreFullscreenChecks = true;

            applyNodeDataToWindow(&fakeNode, pWindow->workspaceID());
        }
    }

    g_pCompositor->changeWindowZOrder(pWindow, true);
}

void COrthoLayout::recalculateWindow(PHLWINDOW pWindow)
{
    const auto result = getNodeFromWindow(pWindow);
    if (!result.has_value())
        return;
    recalculateMonitor(pWindow->monitorID());
}

SWindowRenderLayoutHints COrthoLayout::requestRenderHints(PHLWINDOW pWindow)
{
    SWindowRenderLayoutHints hints;
    return hints;
}

void COrthoLayout::moveWindowTo(PHLWINDOW pWindow, const std::string &dir, bool silent)
{
    if (!isDirection(dir))
        return;

    const auto PWINDOW2 = g_pCompositor->getWindowInDirection(pWindow, dir[0]);

    if (!PWINDOW2)
        return;

    pWindow->setAnimationsToMove();

    if (pWindow->m_workspace != PWINDOW2->m_workspace)
    {
        // if different monitors, send to monitor
        onWindowRemovedTiling(pWindow);
        pWindow->moveToWorkspace(PWINDOW2->m_workspace);
        pWindow->m_monitor = PWINDOW2->m_monitor;
        if (!silent)
        {
            const auto pMonitor = pWindow->m_monitor.lock();
            Desktop::focusState()->rawMonitorFocus(pMonitor);
        }
        onWindowCreatedTiling(pWindow);
    }
    else
    {
        // if same monitor, switch windows
        switchWindows(pWindow, PWINDOW2);
        if (silent)
            Desktop::focusState()->fullWindowFocus(PWINDOW2);
    }

    pWindow->updateGroupOutputs();
    if (!pWindow->m_groupData.pNextWindow.expired())
    {
        PHLWINDOW next = pWindow->m_groupData.pNextWindow.lock();
        while (next != pWindow)
        {
            next->updateToplevel();
            next = next->m_groupData.pNextWindow.lock();
        }
    }
}

void COrthoLayout::switchWindows(PHLWINDOW pWindowA, PHLWINDOW pWindowB)
{
    // windows should be valid, insallah
    const auto resultA = getNodeFromWindow(pWindowA);
    const auto resultB = getNodeFromWindow(pWindowB);

    if (!resultA.has_value() || !resultB.has_value())
        return;

    const auto &[ndA, wsA, statusA] = *resultA;
    const auto &[ndB, wsB, statusB] = *resultB;

    // references to the underlying vectors for modification
    auto &stackA = (statusA == ORTHOSTATUS_MAIN) ? m_mainStackByWorkspace[wsA] : m_secondaryStackByWorkspace[wsA];
    auto &stackB = (statusB == ORTHOSTATUS_MAIN) ? m_mainStackByWorkspace[wsB] : m_secondaryStackByWorkspace[wsB];

    // find indices of the nodes inside their vectors

    const auto NODEITA = std::ranges::find(stackA, *ndA);
    const auto NODEITB = std::ranges::find(stackB, *ndB);

    if (NODEITA == stackA.end() || NODEITB == stackB.end())
        return;

    const size_t idxA = std::distance(stackA.begin(), NODEITA);
    const size_t idxB = std::distance(stackB.begin(), NODEITB);

    pWindowA->setAnimationsToMove();
    pWindowB->setAnimationsToMove();

    // perform swap/move between containers
    // minimum stack size is invariant across swaps.
    std::swap(stackA[idxA], stackB[idxB]);

    // recalc/damage
    recalculateMonitor(pWindowA->monitorID());
    if (wsA != wsB)
        recalculateMonitor(pWindowB->monitorID());

    g_pHyprRenderer->damageWindow(pWindowA);
    g_pHyprRenderer->damageWindow(pWindowB);
}

// TODO: allow for dynamic adjustment of the weights
void COrthoLayout::alterSplitRatio(PHLWINDOW pWindow, float ratio, bool exact) {}

// TODO: Consider loops and reverse
PHLWINDOW COrthoLayout::getNextWindowCandidate(PHLWINDOW pWindow)
{

    const auto result = getNodeFromWindow(pWindow);
    if (!result.has_value())
    {
        if (!Desktop::focusState()->monitor())
            return nullptr;
        const auto ws = Desktop::focusState()->monitor()->activeWorkspaceID();
        const auto &mainStack = m_mainStackByWorkspace[ws];
        if (mainStack.empty())
            return nullptr;
        return std::ranges::begin(mainStack)->pWindow.lock();
    }

    const auto &[nd, ws, status] = *result;

    const auto &mainStack = m_mainStackByWorkspace[ws];
    const auto &secondaryStack = m_secondaryStackByWorkspace[ws];

    const auto &firstPool = status == ORTHOSTATUS_MAIN ? mainStack : secondaryStack;
    const auto &secondPool = status == ORTHOSTATUS_MAIN ? secondaryStack : mainStack;

    const auto NODEIT = std::ranges::find(firstPool, *nd);
    if (NODEIT == firstPool.end())
    {
        return std::ranges::begin(firstPool)->pWindow.lock();
    }
    auto CANDIDATE = std::ranges::next(NODEIT);
    if (CANDIDATE == firstPool.end())
    {
        CANDIDATE = std::ranges::begin(secondPool);
        if (CANDIDATE == secondPool.end())
        {
            CANDIDATE = std::ranges::begin(firstPool);
        }
    }
    return CANDIDATE->pWindow.lock();
}

void COrthoLayout::replaceWindowDataWith(PHLWINDOW from, PHLWINDOW to)
{
    const auto result = getNodeFromWindow(from);
    if (!result.has_value())
        return;
    const auto &[nd, ws, _] = *result;
    nd->pWindow = to;
    applyNodeDataToWindow(nd, ws);
}

Vector2D COrthoLayout::predictSizeForNewWindowTiled()
{
    static auto PMAINSTACKMIN = CConfigValue<Hyprlang::INT>("plugin:ortho:main_stack_min");
    const int mainStackMinimum = *PMAINSTACKMIN >= 1 ? *PMAINSTACKMIN : 1;
    if (!Desktop::focusState()->monitor())
        return {};
    const auto WS = Desktop::focusState()->monitor()->m_activeWorkspace->m_id;
    const auto MAINSTACK = m_mainStackByWorkspace[WS];
    const auto SECONDARYSTACK = m_secondaryStackByWorkspace[WS];
    const auto WSDATA = m_orthoWorkspaceDataByWorkspace[WS];
    const auto MSIZE = Desktop::focusState()->monitor()->m_size;

    if (MAINSTACK.size() == 0)
    { // the workspace is empty as mainStackMin is at least 1
        return Desktop::focusState()->monitor()->m_size;
    }

    if (MAINSTACK.size() < mainStackMinimum)
    {
        const double HEIGHT = MSIZE.y;
        double totalWeight = 1; // assume new window has weight 1

        for (auto &nd : MAINSTACK)
        {
            totalWeight += nd.weight;
        }

        const double NPERC = 1 / totalWeight;
        const double WIDTH = MSIZE.x * WSDATA.percMainStack * NPERC;
        return Vector2D(WIDTH, HEIGHT);
    }
    else
    {
        const double WIDTH = MSIZE.x * (1 - WSDATA.percMainStack);
        double totalWeight = 1;

        for (auto &nd : SECONDARYSTACK)
        {
            totalWeight += nd.weight;
        }

        const double NPERC = 1 / totalWeight;
        double HEIGHT = MSIZE.y * NPERC;
        return Vector2D(WIDTH, HEIGHT);
    }
}

void COrthoLayout::onEnable()
{
    m_configCallback = g_pHookSystem->hookDynamic("configReloaded", [this](void *hk, SCallbackInfo &info, std::any param) {}); // TODO load orientation and layout overrides
    for (auto const &w : g_pCompositor->m_windows)
    {
        if (w->m_isFloating || !w->m_isMapped || w->isHidden())
            continue;

        onWindowCreatedTiling(w);
    }
}

void COrthoLayout::onDisable()
{
    m_mainStackByWorkspace.clear();
    m_orthoWorkspaceDataByWorkspace.clear();
    m_secondaryStackByWorkspace.clear();
}

bool COrthoLayout::inMain(SOrthoNodeData *nd)
{
    for (auto &[ws, nodes] : m_mainStackByWorkspace)
    {
        const auto IT = std::ranges::find(nodes, *nd);
        if (IT != nodes.end())
            return true;
    }
    return false;
}

// TODO: Mostly things to do with setting the main stack factor and weight of active window
std::any COrthoLayout::layoutMessage(SLayoutMessageHeader header, std::string message)
{
    auto switchToWindow = [&](PHLWINDOW PWINDOWTOCHANGETO)
    {
        if (!validMapped(PWINDOWTOCHANGETO))
            return;

        Desktop::focusState()->fullWindowFocus(PWINDOWTOCHANGETO);
        g_pCompositor->warpCursorTo(PWINDOWTOCHANGETO->middle());

        g_pInputManager->m_forcedFocus = PWINDOWTOCHANGETO;
        g_pInputManager->simulateMouseMovement();
        g_pInputManager->m_forcedFocus.reset();
    };
    CVarList vars(message, 0, ' ');

    if (vars.size() < 1 || vars[0].empty())
    {
        Debug::log(ERR, "layoutmsg called without params");
        return 0;
    }

    auto command = vars[0];

    if (command == "adjustweight")
        return messageAdjustWeight(header, vars);
    if (command == "overridemainweights")
        return messageOverrideMainWeights(header, vars);
    return 0;
}

std::any COrthoLayout::messageAdjustWeight(SLayoutMessageHeader header, CVarList vars)
{
    const auto PWINDOW = header.pWindow;
    if (!PWINDOW)
        return 0;
    const auto RESULT = getNodeFromWindow(PWINDOW);
    if (!RESULT.has_value())
        return 0;
    auto &[nd, _, __] = *RESULT;

    if (vars.size() == 0)
    {
        Debug::log(ERR, "layoutmsg adjustweight called without params");
    }

    if (vars.size() == 2)
    {
        try
        {
            float adjustment = std::stof(vars[1]);
            nd->weight += adjustment;
            recalculateMonitor(header.pWindow->monitorID());
        }
        catch (const std::invalid_argument &e)
        {
            Debug::log(ERR, "layoutmsg adjustweight called without number {}", e.what());
            return 0;
        }
        catch (const std::out_of_range &e)
        {
            Debug::log(ERR, "layoutmsg adjustweight called without outofrange {}", e.what());
            return 0;
        }
    }
    else if (vars.size() == 3)
    {
        if (vars[1] != "exact")
        {
            Debug::log(ERR, "layoutmsg called with invalid specifier");
            return 0;
        }
        try
        {
            float newWeight = std::stof(vars[2]);
            nd->weight = newWeight;
            recalculateMonitor(header.pWindow->monitorID());
        }
        catch (const std::invalid_argument &e)
        {
            Debug::log(ERR, "layoutmsg adjustweight called without number {}", e.what());
            return 0;
        }
        catch (const std::out_of_range &e)
        {
            Debug::log(ERR, "layoutmsg adjustweight called without outofrange {}", e.what());
            return 0;
        }
    }
    else
    {
        Debug::log(ERR, "layoutmsg adjustweight called with too many params");
    }
    return 0;
}

std::any COrthoLayout::messageOverrideMainWeights(SLayoutMessageHeader header, CVarList vars)
{
    if (vars.size() == 1)
    {
        Debug::log(ERR, "layoutmsg overridemainweights called without params");
    }

    const auto WS = header.pWindow->m_workspace->m_id;
    const auto MAINSIZE = m_mainStackByWorkspace[WS].size();
    auto WSDATA = getOrthoWorkspaceData(WS);

    const auto RESULT = parseOverrideWeights(vars, size_t(1), vars.size());
    if (RESULT.has_value())
    {
        WSDATA->overrideMainWeights = true;
        WSDATA->mainWeightOverrides = *RESULT;
    }

    recalculateMonitor(header.pWindow->monitorID());
    return 0;
}
