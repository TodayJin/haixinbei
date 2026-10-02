#include "kk_ui_internal.h"

const KK_UI_PageRoute *KK_UI_GetRoute(KK_UI_PageId page)
{
    if (kk_ui.app == NULL || page == KK_UI_PAGE_NONE ||
        page > kk_ui.app->route_count) {
        return NULL;
    }
    return &kk_ui.app->routes[page - 1U];
}

KK_UI_PageType KK_UI_CurrentPageType(void)
{
    const KK_UI_PageRoute *route = KK_UI_GetRoute(kk_ui.current.page);
    return route != NULL ? route->type : KK_UI_PAGE_HOME;
}

void KK_UI_ResetPageState(KK_UI_PageState *state, KK_UI_PageId page)
{
    const KK_UI_PageRoute *route;

    state->page = page;
    state->selected = 0U;
    state->top = 0U;
    state->scroll_q8 = 0;
    state->focus_q8 = 0;
    route = KK_UI_GetRoute(page);
    if (route != NULL && route->type == KK_UI_PAGE_MENU) {
        KK_UI_MenuRepairFocus(state);
    }
}

/* ---- 应用层请求的"下一次进入某页的初始选中项"（一次性） ---- */
static uint16_t s_entry_selection;
static uint8_t s_entry_selection_pending;

void KK_UI_SetEntrySelection(uint16_t index)
{
    s_entry_selection = index;
    s_entry_selection_pending = 1U;
}

/* 该页的 home 配置（只有 KK_UI_PAGE_HOME 的页有 item 列表）。 */
static const KK_UI_HomePage *kk_ui_home_page_of(KK_UI_PageId page)
{
    const KK_UI_PageRoute *route = KK_UI_GetRoute(page);

    if (route == NULL || route->type != KK_UI_PAGE_HOME || kk_ui.app == NULL ||
        route->index >= kk_ui.app->home_page_count) {
        return NULL;
    }
    return &kk_ui.app->home_pages[route->index];
}

static void kk_ui_start_transition(const KK_UI_PageState *outgoing,
                                   bool reverse, uint32_t now)
{
    kk_ui.transition.outgoing = *outgoing;
    kk_ui.transition.started = now;
    kk_ui.transition.duration = (kk_ui.next_page_ms != 0U) ?
                                kk_ui.next_page_ms : KK_UI_PAGE_MS;
    kk_ui.transition.active = 1U;
    kk_ui.transition.reverse = reverse ? 1U : 0U;
    kk_ui.transition.leave_custom = 0U;
    kk_ui.blocked_keys |= (uint8_t)((kk_ui.keys[0].stable ? KK_UI_KEY_UP : 0U) |
                           (kk_ui.keys[1].stable ? KK_UI_KEY_DOWN : 0U) |
                           (kk_ui.keys[2].stable ? KK_UI_KEY_OK : 0U));
#if KK_UI_ENABLE_CUSTOM
    {
        const KK_UI_PageRoute *route = KK_UI_GetRoute(outgoing->page);
        if (route != NULL && route->type == KK_UI_PAGE_CUSTOM) {
            kk_ui.transition.leave_custom = 1U;
        }
    }
#endif
    kk_ui.dirty = 1U;
}

/* 压栈前进。reverse=false 新页从右边滑入，true 从左边滑入——
 * 应用层用 KK_UI_NavigateToDirected 让动画方向跟按键一致。 */
static KK_UI_Status kk_ui_navigate_push(KK_UI_PageId page, bool reverse,
                                        uint32_t now)
{
    const KK_UI_PageRoute *route = KK_UI_GetRoute(page);
    KK_UI_PageState outgoing;
    uint16_t entry = s_entry_selection;
    uint8_t has_entry = s_entry_selection_pending;

    /* 成败都消费掉，避免残留到下一次无关的导航 */
    s_entry_selection_pending = 0U;

    if (route == NULL) {
        return KK_UI_INVALID_ARGUMENT;
    }
    if (kk_ui.transition.active != 0U) {
        return KK_UI_BUSY;
    }
    if (kk_ui.stack_count >= KK_UI_NAV_DEPTH) {
        KK_UI_RecordError(KK_UI_NAVIGATION_FULL, kk_ui.current.page, page);
        kk_ui.reject_flash = 2U;
        kk_ui.dirty = 1U;
        return KK_UI_NAVIGATION_FULL;
    }
    /* 2026-09-30 放宽：允许「自定义页 -> 自定义页」。
     * 应用里确实存在这种两级自定义流程：模块测试页 PAGE_CUSTOM_TEST
     * ──中键──> 电机测试页 PAGE_CUSTOM_MOTOR（见 App/Src/app_ui.c）。
     * 框架本身已经具备该能力：压栈时 kk_ui_start_transition() 会把旧页标记成
     * leave_custom（kk_ui_nav.c:68-75），过渡结束后回调 KK_UI_CustomOnLeave
     * （kk_ui_nav.c:200-202）；弹栈按类型无关处理，栈深由 KK_UI_NAV_DEPTH 管。
     * 原来这里直接 return KK_UI_NOT_ALLOWED，会把这种跳转静默吞掉 ——
     * 表现就是"按中键没反应"（app_ui.c 里 (void) 丢掉了返回值）。 */
    outgoing = kk_ui.current;
    kk_ui.stack[kk_ui.stack_count++] = outgoing;
    KK_UI_ResetPageState(&kk_ui.current, page);
    if (has_entry != 0U) {
        const KK_UI_HomePage *home = kk_ui_home_page_of(page);
        if (home != NULL && entry < home->item_count) {
            kk_ui.current.selected = entry;
        }
    }
    kk_ui.home_anim.active = 0U;
    kk_ui.list_anim.active = 0U;
    kk_ui_start_transition(&outgoing, reverse, now);
#if KK_UI_ENABLE_CUSTOM
    if (route->type == KK_UI_PAGE_CUSTOM) {
        kk_ui.in_callback = 1U;
        KK_UI_CustomOnEnter(page);
        kk_ui.in_callback = 0U;
    }
#endif
    return KK_UI_OK;
}

KK_UI_Status KK_UI_NavigateTo(KK_UI_PageId page, uint32_t now)
{
    return kk_ui_navigate_push(page, false, now);
}

KK_UI_Status KK_UI_NavigateToDirected(KK_UI_PageId page, bool reverse,
                                      uint32_t now)
{
    return kk_ui_navigate_push(page, reverse, now);
}

/* 弹栈回退。默认 reverse=true（被弹出的旧页从左边滑回来）。 */
static KK_UI_Status kk_ui_navigate_pop(bool reverse, uint32_t now)
{
    KK_UI_PageState outgoing;
    const KK_UI_PageRoute *route;

    if (kk_ui.transition.active != 0U) {
        return KK_UI_BUSY;
    }
    if (kk_ui.stack_count == 0U) {
        return KK_UI_NOT_ALLOWED;
    }
    outgoing = kk_ui.current;
    kk_ui.current = kk_ui.stack[--kk_ui.stack_count];
    if (KK_UI_CurrentPageType() == KK_UI_PAGE_MENU) {
        KK_UI_MenuRepairFocus(&kk_ui.current);
    }
    kk_ui.home_anim.active = 0U;
    kk_ui.list_anim.active = 0U;
    kk_ui_start_transition(&outgoing, reverse, now);
#if KK_UI_ENABLE_CUSTOM
    route = KK_UI_GetRoute(kk_ui.current.page);
    if (route != NULL && route->type == KK_UI_PAGE_CUSTOM) {
        kk_ui.in_callback = 1U;
        KK_UI_CustomOnEnter(kk_ui.current.page);
        kk_ui.in_callback = 0U;
    }
#else
    (void)route;
#endif
    return KK_UI_OK;
}

KK_UI_Status KK_UI_NavigateBack(uint32_t now)
{
    return kk_ui_navigate_pop(true, now);
}

KK_UI_Status KK_UI_NavigateBackDirected(bool reverse, uint32_t now)
{
    return kk_ui_navigate_pop(reverse, now);
}

void KK_UI_AnimateNavigation(uint32_t now)
{
    if (kk_ui.transition.active != 0U) {
        uint32_t elapsed = now - kk_ui.transition.started;
        kk_ui.dirty = 1U;
        if (elapsed >= kk_ui.transition.duration) {
            kk_ui.transition.active = 0U;
            /* 落页就解除按键屏蔽：长按的方向键必须继续重复下去。
             * 用户 m02517：图标菜单里一直按住左键，滑回表盘后就停住了——
             * 因为 kk_ui_process_input() 在 transition 期间会把按住的键写进
             * blocked_keys，且只在松手时才清。触发这次导航的那一次 press
             * 早就被消费掉了，这里放开只影响 repeat。 */
            kk_ui.blocked_keys = 0U;
#if KK_UI_ENABLE_CUSTOM
            if (kk_ui.transition.leave_custom != 0U) {
                kk_ui.in_callback = 1U;
                KK_UI_CustomOnLeave(kk_ui.transition.outgoing.page);
                kk_ui.in_callback = 0U;
            }
#endif
        }
    }
}

void KK_UI_DrawPage(const KK_UI_PageState *state, int16_t x_offset,
                    int16_t clip_x, uint16_t clip_width, uint32_t now)
{
    const KK_UI_PageRoute *route = KK_UI_GetRoute(state->page);
    (void)now;

    if (route == NULL || clip_width == 0U) {
        return;
    }
    switch (route->type) {
#if KK_UI_ENABLE_HOME
    case KK_UI_PAGE_HOME:
        KK_UI_HomeDraw(state, x_offset, clip_x, clip_width);
        break;
#endif
#if KK_UI_ENABLE_MENU
    case KK_UI_PAGE_MENU:
        KK_UI_MenuDraw(state, x_offset, clip_x, clip_width);
        break;
#endif
#if KK_UI_ENABLE_INFO
    case KK_UI_PAGE_INFO:
        KK_UI_InfoDraw(state, x_offset, clip_x, clip_width);
        break;
#endif
#if KK_UI_ENABLE_CUSTOM
    case KK_UI_PAGE_CUSTOM:
        kk_ui.in_callback = 1U;
        KK_UI_CustomOnDraw(state->page, x_offset, clip_x, clip_width);
        kk_ui.in_callback = 0U;
        break;
#endif
    default:
        break;
    }
}

/* 过渡几何（用户 m04708）：
 *   全工程只有表盘页自己有圆角外框（app_ui.c draw_splash_page，带 s_dx）。
 *   出去/进来两页都按整屏宽度滑（KK_UI_SCREEN_WIDTH-1 = 127 px），而且合缝
 *   就取"表盘那圈框的右边线"——框的左右边缘分别在 x=1+off 和 x=126+off，
 *   所以把合缝定在 current_x 时，框右边线刚好落在裁剪窗口内的最后一列
 *   (current_x-1)，框永远不会被切开；反过来那一页紧贴它 1px 开始，中间不留
 *   黑缝。于是翻页看起来就是"表盘的框滑过去、菜单从框右边露出来"，边界就是
 *   那个框本身，而不是一条黑线从中间把框切断。
 *   （原实现是 32px 视差 + 合缝取进来那页的左边缘，结果出去的框右半边被裁掉、
 *    看着像被黑线切开。） */
void KK_UI_DrawScene(uint32_t now)
{
    OLED_Clear();
    OLED_ResetClipWindow();
    OLED_SetDrawMode(OLED_DRAW_SET);
    OLED_SetBackgroundMode(OLED_BG_TRANSPARENT);
    OLED_SetFontDirection(OLED_ROTATION_0);
    OLED_SetFontPosition(OLED_FONT_POS_TOP);
    OLED_SetFontRefHeight(OLED_FONT_REF_ALL);

    if (kk_ui.transition.active != 0U) {
        uint16_t progress = KK_UI_EaseQ12(now - kk_ui.transition.started,
                                         kk_ui.transition.duration);
        int16_t boundary;
        int16_t outgoing_x;
        int16_t current_x;

        /* slide 0..127：出去那页向左滑的像素数。进来那页的位置由 slide 直接
         * 推出（不用各自取整），保证 boundary-1 恰好是表盘框的右边缘那一列：
         * 框的右边缘 = 1 + outgoing_x + (126-1) = outgoing_x + 126。 */
        int16_t slide = (int16_t)(((int32_t)(KK_UI_SCREEN_WIDTH - 1) *
                                   progress) / KK_UI_Q12_ONE);

        if (kk_ui.transition.reverse == 0U) {
            /* 新页从右边滑入：整屏 127px，与合缝同速。 */
            outgoing_x = (int16_t)(-slide);
            current_x = (int16_t)((KK_UI_SCREEN_WIDTH - 1) - slide);
            boundary = current_x;
            if (boundary > 0) {
                KK_UI_DrawPage(&kk_ui.transition.outgoing, outgoing_x, 0,
                               (uint16_t)boundary, now);
            }
            if (boundary < KK_UI_SCREEN_WIDTH) {
                KK_UI_DrawPage(&kk_ui.current, current_x, boundary,
                               (uint16_t)(KK_UI_SCREEN_WIDTH - boundary), now);
            }
            /* 只复位裁剪窗口——后面的拒绝白框、对话框、Toast 都要全屏画。 */
            OLED_ResetClipWindow();
        } else {
            /* 新页从左边滑入（表盘从左边滑回来）：合缝同样是它的框右边线，
             * boundary-1 就是框右边缘那一列，所以框是完整滑进来的。 */
            outgoing_x = slide;
            boundary = slide;
            current_x = (int16_t)(slide - (KK_UI_SCREEN_WIDTH - 1));
            if (boundary > 0) {
                KK_UI_DrawPage(&kk_ui.current, current_x, 0,
                               (uint16_t)boundary, now);
            }
            if (boundary < KK_UI_SCREEN_WIDTH) {
                KK_UI_DrawPage(&kk_ui.transition.outgoing, outgoing_x,
                               boundary,
                               (uint16_t)(KK_UI_SCREEN_WIDTH - boundary), now);
            }
            OLED_ResetClipWindow();
        }
    } else {
        KK_UI_DrawPage(&kk_ui.current, 0, 0, KK_UI_SCREEN_WIDTH, now);
    }

    /* 外框不在这里画（用户 m04657/m04708）：全工程只有表盘页自己有框
     * （app_ui.c draw_splash_page()，带 s_dx）。翻页时它整页滑走/滑进来，
     * 合缝就取它的框右边线（见上面的过渡几何），所以框永远是完整的、
     * 也不会让菜单页平时带一圈框。 */

    if (kk_ui.reject_flash > 1U) {
        OLED_SetDrawMode(OLED_DRAW_XOR);
        OLED_DrawRFrame(1, 1, 126U, 62U, 3U);
        OLED_SetDrawMode(OLED_DRAW_SET);
        kk_ui.reject_flash = 1U;
        kk_ui.dirty = 1U;
    } else if (kk_ui.reject_flash != 0U) {
        kk_ui.reject_flash = 0U;
    }
    KK_UI_DialogDraw();
    KK_UI_ToastDraw();
    OLED_ResetClipWindow();
    OLED_SetDrawMode(OLED_DRAW_SET);
}
