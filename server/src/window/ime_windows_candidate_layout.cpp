// 候选窗定位与裁剪：光标锚点、四分之一屏宿主尺寸、装饰偏移、上下翻转与贴边收回、
// 裁剪记忆与 SetWindowRgn，以及 KeepCandidateCardInsideHostAndMonitor。
#include "window/ime_windows_internal.h"
#include "global/globals.h"
#include "config/ime_config.h"
#include "ipc/ipc.h"
#include "defines/globals.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include "webview2/windows_webview2.h"
#include "utils/window_utils.h"
#include "log/candidate_diag_log.h"

using namespace ime_windows_detail;

namespace ime_windows_detail
{
POINT GetCandidateLayoutCaret()
{
    POINT caret{Global::Point[0], Global::Point[1]};
    if (GetConfiguredCandidateWindowFollowCursor() || caret.y == Global::INVALID_Y)
    {
        return caret;
    }
    if (!g_candidate_session_anchor_valid)
    {
        g_candidate_session_anchor = caret;
        g_candidate_session_anchor_valid = true;
        CAND_DIAG_LOGF(L"candidate-position anchor-captured caret=({},{})", caret.x, caret.y);
    }
    return g_candidate_session_anchor;
}

// Stable WebView2 host = half monitor width × half height (≈ 1/4 screen area).
// Keep this size across create / hide / show so put_Bounds and CSS margins stay
// consistent; only the card slides via MarginLeft/MarginTop + SetWindowRgn.
std::pair<int, int> ComputeQuarterScreenHostPixels(const MonitorCoordinates &monitor)
{
    const int hostWidthPx = (std::max)(1, (monitor.right - monitor.left) / 2);
    const int hostHeightPx = (std::max)(1, (monitor.bottom - monitor.top) / 2);
    return {hostWidthPx, hostHeightPx};
}

// One-line geometry audit for candidate misplacement. Logs only coordinates /
// sizes / deltas — never input text. card_screen is host origin + CSS margin.
void LogCandidatePositionAudit(const wchar_t *stage, POINT rawCaret, POINT layoutCaret, POINT properPos, int hostX,
                               int hostY, int hostWidthPx, int hostHeightPx, FLOAT scale, double contentWidthDip,
                               double contentHeightDip, const MonitorCoordinates &monitor, int marginLeftBefore,
                               int marginTopBefore)
{
    if (!::DiagnosticLog::IsEnabled())
    {
        return;
    }
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    const int cardScreenX = hostX + static_cast<int>(std::lround(Global::MarginLeft * static_cast<double>(scale)));
    const int cardScreenY = hostY + static_cast<int>(std::lround(Global::MarginTop * static_cast<double>(scale)));
    RECT hostRect{};
    if (::global_hwnd && IsWindow(::global_hwnd))
    {
        GetWindowRect(::global_hwnd, &hostRect);
    }
    CAND_DIAG_LOGF(L"candidate-pos-audit stage={} raw_caret=({},{}) layout_caret=({},{}) proper=({},{}) "
                   L"host_desired=({},{} {}x{}) host_actual=({},{},{}x{}) margin=({},{}) "
                   L"margin_before=({},{}) card_screen=({},{}) delta_card_minus_caret=({},{}) "
                   L"content_dip=({:.1f},{:.1f}) scale={:.3f} monitor=({},{})-({},{})",
                   stage, rawCaret.x, rawCaret.y, layoutCaret.x, layoutCaret.y, properPos.x, properPos.y, hostX, hostY,
                   hostWidthPx, hostHeightPx, hostRect.left, hostRect.top, hostRect.right - hostRect.left,
                   hostRect.bottom - hostRect.top, Global::MarginLeft, Global::MarginTop, marginLeftBefore,
                   marginTopBefore, cardScreenX, cardScreenY, cardScreenX - layoutCaret.x, cardScreenY - layoutCaret.y,
                   contentWidthDip, contentHeightDip, static_cast<double>(scale), monitor.left, monitor.top,
                   monitor.right, monitor.bottom);
}

double GetCandidateDecorationTopDip()
{
    return GetActiveCandidateSkinDecorationTopDip();
}
} // namespace ime_windows_detail

namespace
{
double GetCandidateDecorationWidthDip()
{
    return GetActiveCandidateSkinDecorationWidthDip();
}

double GetCandidateOuterTopDip()
{
    return GetActiveCandidateSkinOuterTopDip();
}

double GetCandidateOuterRightDip()
{
    return GetActiveCandidateSkinOuterRightDip();
}

double GetCandidateOuterBottomDip()
{
    return GetActiveCandidateSkinOuterBottomDip();
}

double GetCandidateOuterLeftDip()
{
    return GetActiveCandidateSkinOuterLeftDip();
}
} // namespace

namespace ime_windows_detail
{
int GetCandidatePackingMarginTopDip()
{
    // MarginTop includes the permanent transparent inset that lets Chromium
    // rasterize the shadow above the card. Only the remainder is placement
    // slack accumulated while clamping the stable host to a monitor edge.
    return (std::max)(0, Global::MarginTop - ::CANDIDATE_SHADOW_PAD_TOP);
}

std::pair<double, double> AddCandidateDecorationToSize(const std::pair<double, double> &cardSize)
{
    const double decorationTopDip = GetCandidateDecorationTopDip();
    std::pair<double, double> size = cardSize;
    if (decorationTopDip > 0.0)
    {
        size = {(std::max)(size.first, GetCandidateDecorationWidthDip()), size.second + decorationTopDip};
    }
    // v2 挂件挂在卡片外沿：窗口在卡片四周再各留出一块透明空白，立绘才不会被窗口边界裁掉。
    size.first += GetCandidateOuterLeftDip() + GetCandidateOuterRightDip();
    size.second += GetCandidateOuterTopDip() + GetCandidateOuterBottomDip();
    return size;
}

int GetCandidateOuterTopPx(int cardAnchorY, int packingTopDip, FLOAT scale)
{
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    const double outerOffsetDip =
        static_cast<double>(packingTopDip) - GetCandidateDecorationTopDip() - GetCandidateOuterTopDip();
    return cardAnchorY + static_cast<int>(std::lround(outerOffsetDip * static_cast<double>(scale)));
}

int GetCandidateOuterLeftPx(FLOAT scale)
{
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    return static_cast<int>(std::lround(GetCandidateOuterLeftDip() * static_cast<double>(scale)));
}

void RememberCandidateClip(const std::pair<double, double> &decoratedSize, FLOAT scale, int caretX, int caretY)
{
    g_has_last_candidate_clip = decoratedSize.first > 1.0 && decoratedSize.second > 1.0;
    if (g_has_last_candidate_clip)
    {
        g_last_candidate_clip_size = decoratedSize;
        g_last_candidate_clip_scale = scale > 0.0f ? scale : 1.0f;
    }
    g_last_placed_caret_x = caretX;
    g_last_placed_caret_y = caretY;
}

void RememberCandidateClipSize(const std::pair<double, double> &decoratedSize, FLOAT scale)
{
    g_has_last_candidate_clip = decoratedSize.first > 1.0 && decoratedSize.second > 1.0;
    if (g_has_last_candidate_clip)
    {
        g_last_candidate_clip_size = decoratedSize;
        g_last_candidate_clip_scale = scale > 0.0f ? scale : 1.0f;
    }
}

void RememberCandidateFlip(int cardTopPx, int caretY)
{
    g_candidate_placed_above_caret = cardTopPx + 8 < caretY;
}
} // namespace ime_windows_detail

namespace
{
void RememberCandidateClipEnvelope(double extraTopDip, const std::pair<double, double> &decoratedSize)
{
    extraTopDip = (std::max)(0.0, extraTopDip);
    g_has_candidate_clip_envelope = decoratedSize.first > 1.0 && decoratedSize.second > 1.0;
    if (!g_has_candidate_clip_envelope)
    {
        return;
    }
    g_clip_envelope_left_dip = static_cast<double>(Global::MarginLeft);
    g_clip_envelope_right_dip = g_clip_envelope_left_dip + decoratedSize.first;
    g_clip_envelope_top_dip = static_cast<double>(Global::MarginTop) - extraTopDip;
    g_clip_envelope_bottom_dip = static_cast<double>(Global::MarginTop) + decoratedSize.second;
}
} // namespace

namespace ime_windows_detail
{
bool CandidateCardFitsClipEnvelope(const std::pair<double, double> &decoratedSize)
{
    if (!g_has_candidate_clip_envelope || decoratedSize.first <= 1.0 || decoratedSize.second <= 1.0)
    {
        return false;
    }
    const double left = static_cast<double>(Global::MarginLeft);
    const double top = static_cast<double>(Global::MarginTop);
    const double right = left + decoratedSize.first;
    const double bottom = top + decoratedSize.second;
    constexpr double kSlop = 1.5;
    return left + kSlop >= g_clip_envelope_left_dip && top + kSlop >= g_clip_envelope_top_dip &&
           right <= g_clip_envelope_right_dip + kSlop && bottom <= g_clip_envelope_bottom_dip + kSlop;
}

double EstimateVerticalPageHeightDip(double currentHeightDip)
{
    const int pageSize = (std::max)(1, GetConfiguredCandidatePageSize());
    const Global::CandidatePageSnapshotPtr candidatePage = Global::LoadCandidatePageSnapshot();
    int visible = candidatePage->page_count;
    if (visible < 1)
    {
        visible = candidatePage->page_item_count;
    }
    if (visible < 1)
    {
        visible = 1;
    }
    return currentHeightDip * (static_cast<double>(pageSize) / static_cast<double>(visible));
}
} // namespace ime_windows_detail

namespace
{
void ClearCandidateClipState()
{
    g_has_last_candidate_clip = false;
    g_has_candidate_clip_envelope = false;
    g_candidate_placed_above_caret = false;
    g_last_candidate_card_size = {};
}
} // namespace

namespace ime_windows_detail
{
// Re-anchor an already-placed card after its painted size changed.
//
// A content-only update changes the card's width *and* its height, so the
// position placement asked for moves with it. Clamping alone cannot follow:
// MarginLeft only ever shrinks, so a card parked flush against the right screen
// edge kept its old left edge while narrowing; and a card that grew past the
// work-area bottom was pushed straight up by KeepCandidateCardInsideHostAndMonitor
// — onto the very text line it should have flipped above. The old incremental
// "MarginTop -= height delta" bookkeeping could not fix that either: it kept the
// bottom glued to wherever the card already was, accumulating every earlier
// clamp instead of re-deriving the anchor.
//
// So ask AdjustCandidateWindowPosition for the whole decision again, using the
// size that was actually painted — the only height the flip test may trust.
void ReanchorCandidateHostToPaintedCard(const POINT &layoutCaret, const std::pair<double, double> &cardSize,
                                        const MonitorCoordinates &coordinates, int hostHeightPx, FLOAT scale,
                                        int &hostX, int &hostY)
{
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    auto anchorPos = std::make_shared<std::pair<int, int>>();
    AdjustCandidateWindowPosition(&layoutCaret, cardSize, anchorPos, scale, cardSize.first);
    RememberCandidateFlip(anchorPos->second, layoutCaret.y);

    const int desiredOuterTopPx = GetCandidateOuterTopPx(anchorPos->second, GetCandidatePackingMarginTopDip(), scale);
    const int shadowTopPx = static_cast<int>(std::lround(::CANDIDATE_SHADOW_PAD_TOP * static_cast<double>(scale)));
    const int edgePadPx = static_cast<int>(std::lround(2.0 * static_cast<double>(scale)));
    // The stable host stays put while its margin can still carry the card to the
    // anchor. Only a card that wants to sit above the host — a flip, typically —
    // needs the host itself to move.
    if (desiredOuterTopPx < hostY + shadowTopPx)
    {
        hostY = desiredOuterTopPx - shadowTopPx;
        if (hostY + hostHeightPx > coordinates.bottom)
        {
            hostY = coordinates.bottom - hostHeightPx - edgePadPx;
        }
        if (hostY < coordinates.top)
        {
            hostY = coordinates.top + edgePadPx;
        }
    }
    Global::MarginLeft =
        (std::max)(0, static_cast<int>(std::lround((anchorPos->first - hostX) / static_cast<double>(scale))));
    Global::MarginTop = GetCandidateOuterMarginDip(desiredOuterTopPx, hostY, scale);
}

bool IsCandidateHostPaintedVisible(HWND hwnd)
{
    if (!hwnd || !::is_global_wnd_cand_shown || Global::Point[1] == Global::INVALID_Y)
    {
        return false;
    }
    RECT windowRect{};
    if (!GetWindowRect(hwnd, &windowRect) || windowRect.top == Global::INVALID_Y)
    {
        return false;
    }
    return !IsHostWindowCloaked(hwnd) && MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL) != nullptr;
}

bool CandidateCaretNeedsStableFollow(HWND hwnd, POINT caret)
{
    if (g_last_placed_caret_x == Global::INVALID_Y || g_last_placed_caret_y == Global::INVALID_Y)
    {
        return true;
    }

    POINT previous{g_last_placed_caret_x, g_last_placed_caret_y};
    if (MonitorFromPoint(previous, MONITOR_DEFAULTTONEAREST) != MonitorFromPoint(caret, MONITOR_DEFAULTTONEAREST))
    {
        return true;
    }

    // Following every horizontal glyph advance makes Chromium/Electron hosts
    // continuously remeasure, move the inner card, and replace its window
    // region. Keep the card anchored on the current text line; a real line
    // change or vertical scroll still follows after the settle timer.
    FLOAT scale = QueryCandidateHalfScreenDipLimitsForPoint(hwnd, caret).scale;
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    const int lineChangeThresholdPx = (std::max)(1, static_cast<int>(std::lround(12.0 * static_cast<double>(scale))));
    const int visibleLineThresholdPx =
        (std::max)(lineChangeThresholdPx, static_cast<int>(std::lround(48.0 * static_cast<double>(scale))));
    const int thresholdPx = IsCandidateHostPaintedVisible(hwnd) ? visibleLineThresholdPx : lineChangeThresholdPx;
    return std::abs(caret.y - previous.y) >= thresholdPx;
}

int GetCandidateOuterMarginDip(int desiredOuterTopPx, int hostY, FLOAT scale)
{
    if (scale <= 0.0f)
    {
        scale = 1.0f;
    }
    // The stable host is clamped to the monitor before this margin is applied.
    // Near the monitor's top edge, a decoration can make desiredOuterTopPx
    // negative even though hostY has already been clamped to the screen. A
    // negative CSS margin then pushes the whole decorated candidate back above
    // the monitor and defeats that native clamp. Keep the outer content at the
    // host's top edge instead; the card moves down only by the amount necessary
    // to keep its decoration visible.
    return (std::max)(0, static_cast<int>(std::lround((desiredOuterTopPx - hostY) / static_cast<double>(scale))));
}

void ClearCandidateWindowRegion(HWND hwnd)
{
    if (hwnd)
    {
        // A null region restores the full host rectangle. This is needed while
        // the candidate context menu temporarily grows beyond the card.
        SetWindowRgn(hwnd, nullptr, TRUE);
    }
}

void ClipCandidateWindowToContent(HWND hwnd, const std::pair<double, double> &containerSize, FLOAT scale,
                                  double extraTopDip)
{
    if (!hwnd)
    {
        return;
    }

    // Region math must use WebView2's rasterization scale, which also includes
    // Windows accessibility text scaling. GetDpiForWindow alone can therefore
    // be smaller than the scale used to paint CSS DIPs (for example 2.0 vs
    // 2.2), making the native region cut off the right/bottom of the card.
    FLOAT clipScale = GetWebViewRasterizationScale(hwnd);
    if (clipScale <= 0.0f)
    {
        clipScale = scale;
    }
    if (clipScale <= 0.0f)
    {
        return;
    }

    RECT client{};
    if (!GetClientRect(hwnd, &client))
    {
        return;
    }

    // Keep a large, stable WebView host (quarter-screen) to avoid resize flashes,
    // but remove its transparent reserve from the native window region. Child
    // windows (including WebView2) are clipped by the parent region, so points
    // outside the real candidate card fall through to the editor and preserve
    // its I-beam cursor.
    //
    // Use one axis-aligned box from the outer (decoration) top. An L-shaped
    // region that started at the opaque card top cut skin images that live in
    // padding-top; aligning the image strip to the clip's right edge also missed
    // the bitmap when the clip had grown wider than the card.
    constexpr double kRegionSafetyDip = 2.0;
    extraTopDip = (std::max)(0.0, extraTopDip);
    if (GetCandidateDecorationTopDip() > 0.0)
    {
        extraTopDip = (std::max)(extraTopDip, 8.0);
    }
    const int left = (std::max)(static_cast<int>(client.left),
                                static_cast<int>(std::floor((Global::MarginLeft - ::CANDIDATE_SHADOW_PAD_LEFT) *
                                                            static_cast<double>(clipScale))));
    const int top =
        (std::max)(static_cast<int>(client.top),
                   static_cast<int>(std::floor((Global::MarginTop - extraTopDip - ::CANDIDATE_SHADOW_PAD_TOP) *
                                               static_cast<double>(clipScale))));
    const int right = (std::min)(static_cast<int>(client.right),
                                 static_cast<int>(std::ceil((Global::MarginLeft + containerSize.first +
                                                             ::CANDIDATE_SHADOW_PAD_RIGHT + kRegionSafetyDip) *
                                                            static_cast<double>(clipScale))));
    const int bottom = (std::min)(static_cast<int>(client.bottom),
                                  static_cast<int>(std::ceil((Global::MarginTop + containerSize.second +
                                                              ::CANDIDATE_SHADOW_PAD_BOTTOM + kRegionSafetyDip) *
                                                             static_cast<double>(clipScale))));

    if (right <= left || bottom <= top)
    {
        WEBVIEW_DIAG_LOGF(L"ui-region invalid client=({},{}) margin=({},{}) content_dip=({:.2f},{:.2f}) "
                          L"input_scale={:.3f} hwnd_scale={:.3f} computed=({},{})-({},{}) -> cleared",
                          client.right, client.bottom, Global::MarginLeft, Global::MarginTop, containerSize.first,
                          containerSize.second, static_cast<double>(scale), static_cast<double>(clipScale), left, top,
                          right, bottom);
        ClearCandidateWindowRegion(hwnd);
        return;
    }

    HRGN region = CreateRectRgn(left, top, right, bottom);
    if (!region)
    {
        return;
    }
    HRGN currentRegion = CreateRectRgn(0, 0, 0, 0);
    if (currentRegion)
    {
        const int currentRegionType = GetWindowRgn(hwnd, currentRegion);
        if (currentRegionType != ERROR && EqualRgn(currentRegion, region))
        {
            DeleteObject(currentRegion);
            DeleteObject(region);
            RememberCandidateClipEnvelope(extraTopDip, containerSize);
            CAND_DIAG_LOGF(L"candidate-frame region-unchanged rect=({},{})-({},{}) redraw=skipped", left, top, right,
                           bottom);
            return;
        }
        DeleteObject(currentRegion);
    }
    // Do not ask USER32 to repaint synchronously here. During a content-only
    // update WebView2 has accepted the DOM mutation, but its new compositor
    // frame may not have reached the HWND yet. SetWindowRgn(..., TRUE) can then
    // expose a blank/old frame for one refresh. The WebView2 frame submission
    // (or the first uncloak) paints the newly clipped area naturally.
    // On success Windows owns the region handle; on failure it remains ours.
    SetLastError(0);
    const int regionResult = SetWindowRgn(hwnd, region, FALSE);
    const DWORD regionError = regionResult == 0 ? GetLastError() : ERROR_SUCCESS;
    if (regionResult == 0)
    {
        DeleteObject(region);
    }
    else
    {
        RememberCandidateClipEnvelope(extraTopDip, containerSize);
    }
    CAND_DIAG_LOGF(L"candidate-frame region-apply rect=({},{})-({},{}) redraw=false result={} gle={}", left, top, right,
                   bottom, regionResult, regionError);
    WEBVIEW_DIAG_LOGF(L"ui-region client=({},{}) margin=({},{}) content_dip=({:.2f},{:.2f}) "
                      L"input_scale={:.3f} hwnd_scale={:.3f} region=({},{})-({},{}) result={} gle={}",
                      client.right, client.bottom, Global::MarginLeft, Global::MarginTop, containerSize.first,
                      containerSize.second, static_cast<double>(scale), static_cast<double>(clipScale), left, top,
                      right, bottom, regionResult, regionError);
}

// After host clamp + MarginLeft/Top, keep the painted card inside both the
// quarter-screen HWND and the caret's monitor. Underestimated measure widths
// otherwise leave the last candidate hanging past the screen edge.
//
// minContentWidthDip is only a small slack for first-pass under-measure — never
// pass half-monitor maxWidthDip here. That made cardRight always overflow when
// the caret is on the right half of the screen and pulled MarginLeft back to 0,
// parking the card on the host's left edge (screen center).
//
// Edge policy: opaque card stays ≥ edgePad (2 DIP) inside the monitor. If CSS
// margin cannot absorb the overflow, shift the host — do not leave the card
// hanging past the screen with MarginLeft already at 0.
void KeepCandidateCardInsideHostAndMonitor( //
    int &hostX,                             //
    int &hostY,                             //
    int hostWidthPx,                        //
    int hostHeightPx,                       //
    double contentWidthDip,                 //
    double contentHeightDip,                //
    FLOAT layoutScale,                      //
    const MonitorCoordinates &coordinates,  //
    double minContentWidthDip               //
)
{
    if (layoutScale <= 0.0f)
    {
        layoutScale = 1.0f;
    }
    const double scale = static_cast<double>(layoutScale);
    // Slack only — cap so a bogus half-screen floor cannot wipe MarginLeft.
    const double hostWidthDip = static_cast<double>(hostWidthPx) / scale;
    const double hostHeightDip = static_cast<double>(hostHeightPx) / scale;
    const double slackWidthDip =
        (std::min)((std::max)(0.0, minContentWidthDip - contentWidthDip), (std::max)(48.0, contentWidthDip * 0.25));
    const double clampWidthDip = (std::min)(contentWidthDip + slackWidthDip, hostWidthDip);

    const int intendedCardLeft = hostX + static_cast<int>(std::lround(Global::MarginLeft * scale));
    const int intendedCardTop = hostY + static_cast<int>(std::lround(Global::MarginTop * scale));
    const int edgePadPx = static_cast<int>(std::lround(2.0 * scale));

    auto maxMarginLeftDip = [&]() -> double { return hostWidthDip - contentWidthDip; };
    auto maxMarginTopDip = [&]() -> double { return hostHeightDip - contentHeightDip; };

    if (contentWidthDip < hostWidthDip - 0.5)
    {
        const double maxMarginLeft = maxMarginLeftDip();
        if (Global::MarginLeft > maxMarginLeft)
        {
            // Margin alone cannot carry the card to its anchor. Truncating it here
            // silently snapped the card back to the host's right end — a sideways
            // jump with no relation to the caret. Slide the stable host right by the
            // shortfall instead so the card keeps the position placement asked for.
            const int shortfallPx = static_cast<int>(std::lround((Global::MarginLeft - maxMarginLeft) * scale));
            Global::MarginLeft = static_cast<int>(std::floor(maxMarginLeft));
            hostX += shortfallPx;
            if (hostX + hostWidthPx > coordinates.right - edgePadPx)
            {
                hostX = coordinates.right - hostWidthPx - edgePadPx;
            }
            if (hostX < coordinates.left + edgePadPx)
            {
                hostX = coordinates.left + edgePadPx;
            }
        }
    }
    else
    {
        // Card fills the host: margin cannot create offset. Re-home the host so
        // its origin stays on the intended card position, then clamp.
        Global::MarginLeft = 0;
        hostX = intendedCardLeft;
        if (hostX + hostWidthPx > coordinates.right - edgePadPx)
        {
            hostX = coordinates.right - hostWidthPx - edgePadPx;
        }
        if (hostX < coordinates.left + edgePadPx)
        {
            hostX = coordinates.left + edgePadPx;
        }
    }
    if (contentHeightDip < hostHeightDip - 0.5)
    {
        const double maxMarginTop = maxMarginTopDip();
        if (Global::MarginTop > maxMarginTop)
        {
            // Same rule as MarginLeft above: margin alone cannot carry the card
            // down to its anchor, so slide the stable host instead of truncating
            // — truncation would snap the card up to the host's lower end, a
            // vertical jump with no relation to the caret.
            const int shortfallPx = static_cast<int>(std::lround((Global::MarginTop - maxMarginTop) * scale));
            Global::MarginTop = static_cast<int>(std::floor(maxMarginTop));
            hostY += shortfallPx;
            if (hostY + hostHeightPx > coordinates.bottom - edgePadPx)
            {
                hostY = coordinates.bottom - hostHeightPx - edgePadPx;
            }
            if (hostY < coordinates.top + edgePadPx)
            {
                hostY = coordinates.top + edgePadPx;
            }
        }
    }
    else
    {
        Global::MarginTop = 0;
        hostY = intendedCardTop;
        if (hostY + hostHeightPx > coordinates.bottom - edgePadPx)
        {
            hostY = coordinates.bottom - hostHeightPx - edgePadPx;
        }
        if (hostY < coordinates.top + edgePadPx)
        {
            hostY = coordinates.top + edgePadPx;
        }
    }

    auto syncCardEdges = [&](int &outLeft, int &outTop, int &outRight, int &outBottom) {
        outLeft = hostX + static_cast<int>(std::lround(Global::MarginLeft * scale));
        outTop = hostY + static_cast<int>(std::lround(Global::MarginTop * scale));
        outRight = outLeft + static_cast<int>(std::ceil(clampWidthDip * scale));
        outBottom = outTop + static_cast<int>(std::ceil(contentHeightDip * scale));
    };

    int cardLeft = 0;
    int cardTop = 0;
    int cardRight = 0;
    int cardBottom = 0;
    syncCardEdges(cardLeft, cardTop, cardRight, cardBottom);
    const int monitorRightLimit = coordinates.right - edgePadPx;
    const int monitorLeftLimit = coordinates.left + edgePadPx;
    const int monitorBottomLimit = coordinates.bottom - edgePadPx;
    const int monitorTopLimit = coordinates.top + edgePadPx;

    // Prefer sliding the card via MarginLeft/Top. If margin is already exhausted,
    // shift the quarter-screen host so the opaque card still clears the edge.
    if (cardRight > monitorRightLimit)
    {
        const int overflowPx = cardRight - monitorRightLimit;
        const int marginPx = static_cast<int>(std::lround(Global::MarginLeft * scale));
        if (marginPx >= overflowPx)
        {
            const int reduceDip = static_cast<int>(std::ceil(static_cast<double>(overflowPx) / scale));
            Global::MarginLeft = (std::max)(0, Global::MarginLeft - reduceDip);
        }
        else
        {
            Global::MarginLeft = 0;
            hostX -= (overflowPx - marginPx);
            if (hostX < coordinates.left + edgePadPx)
            {
                hostX = coordinates.left + edgePadPx;
            }
        }
        syncCardEdges(cardLeft, cardTop, cardRight, cardBottom);
        // Final hard clamp: even with MarginLeft=0, keep the card's right edge in.
        if (cardRight > monitorRightLimit)
        {
            hostX -= (cardRight - monitorRightLimit);
            if (hostX < coordinates.left + edgePadPx)
            {
                hostX = coordinates.left + edgePadPx;
            }
            syncCardEdges(cardLeft, cardTop, cardRight, cardBottom);
        }
    }
    if (cardLeft < monitorLeftLimit)
    {
        const int deficitPx = monitorLeftLimit - cardLeft;
        const int addDip = static_cast<int>(std::ceil(static_cast<double>(deficitPx) / scale));
        Global::MarginLeft += addDip;
        if (contentWidthDip < hostWidthDip - 0.5)
        {
            const double maxMarginLeft = maxMarginLeftDip();
            if (Global::MarginLeft > maxMarginLeft)
            {
                // Margin cannot push the card further right inside the host —
                // slide the host instead.
                const int capped = static_cast<int>(std::floor(maxMarginLeft));
                const int unusedDip = Global::MarginLeft - capped;
                Global::MarginLeft = (std::max)(0, capped);
                hostX += static_cast<int>(std::lround(unusedDip * scale));
                if (hostX + hostWidthPx > coordinates.right - edgePadPx)
                {
                    hostX = coordinates.right - hostWidthPx - edgePadPx;
                }
            }
        }
        syncCardEdges(cardLeft, cardTop, cardRight, cardBottom);
        if (cardLeft < monitorLeftLimit)
        {
            hostX += (monitorLeftLimit - cardLeft);
            if (hostX + hostWidthPx > coordinates.right - edgePadPx)
            {
                hostX = coordinates.right - hostWidthPx - edgePadPx;
            }
            syncCardEdges(cardLeft, cardTop, cardRight, cardBottom);
        }
    }
    if (cardBottom > monitorBottomLimit)
    {
        const int overflowPx = cardBottom - monitorBottomLimit;
        const int marginPx = static_cast<int>(std::lround(Global::MarginTop * scale));
        if (marginPx >= overflowPx)
        {
            const int reduceDip = static_cast<int>(std::ceil(static_cast<double>(overflowPx) / scale));
            Global::MarginTop = (std::max)(0, Global::MarginTop - reduceDip);
        }
        else
        {
            Global::MarginTop = 0;
            hostY -= (overflowPx - marginPx);
            if (hostY < coordinates.top + edgePadPx)
            {
                hostY = coordinates.top + edgePadPx;
            }
        }
        syncCardEdges(cardLeft, cardTop, cardRight, cardBottom);
        if (cardBottom > monitorBottomLimit)
        {
            hostY -= (cardBottom - monitorBottomLimit);
            if (hostY < coordinates.top + edgePadPx)
            {
                hostY = coordinates.top + edgePadPx;
            }
            syncCardEdges(cardLeft, cardTop, cardRight, cardBottom);
        }
    }
    if (cardTop < monitorTopLimit)
    {
        const int deficitPx = monitorTopLimit - cardTop;
        const int addDip = static_cast<int>(std::ceil(static_cast<double>(deficitPx) / scale));
        Global::MarginTop += addDip;
        if (contentHeightDip < hostHeightDip - 0.5)
        {
            const double maxMarginTop = maxMarginTopDip();
            if (Global::MarginTop > maxMarginTop)
            {
                const int capped = static_cast<int>(std::floor(maxMarginTop));
                const int unusedDip = Global::MarginTop - capped;
                Global::MarginTop = (std::max)(0, capped);
                hostY += static_cast<int>(std::lround(unusedDip * scale));
                if (hostY + hostHeightPx > coordinates.bottom - edgePadPx)
                {
                    hostY = coordinates.bottom - hostHeightPx - edgePadPx;
                }
            }
        }
        syncCardEdges(cardLeft, cardTop, cardRight, cardBottom);
        if (cardTop < monitorTopLimit)
        {
            hostY += (monitorTopLimit - cardTop);
            if (hostY + hostHeightPx > coordinates.bottom - edgePadPx)
            {
                hostY = coordinates.bottom - hostHeightPx - edgePadPx;
            }
        }
    }
}
} // namespace ime_windows_detail
