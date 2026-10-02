#ifndef KK_UI_H
#define KK_UI_H

#include "kk_ui_config.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t KK_UI_PageId;
typedef uint16_t KK_UI_EventId;

#define KK_UI_PAGE_NONE  ((KK_UI_PageId)0U)
#define KK_UI_EVENT_NONE ((KK_UI_EventId)0U)

typedef enum {
    KK_UI_OK = 0,
    KK_UI_INVALID_ARGUMENT,
    KK_UI_NOT_INITIALIZED,
    KK_UI_CONFIG_ERROR,
    KK_UI_BUSY,
    KK_UI_QUEUE_FULL,
    KK_UI_NAVIGATION_FULL,
    KK_UI_UNSUPPORTED,
    KK_UI_DISPLAY_ERROR,
    KK_UI_DISPLAY_TIMEOUT,
    KK_UI_NOT_ALLOWED
} KK_UI_Status;

typedef struct {
    KK_UI_Status code;
    KK_UI_PageId page;
    uint16_t index;
} KK_UI_ErrorInfo;

enum {
    KK_UI_KEY_UP = 1U << 0,
    KK_UI_KEY_DOWN = 1U << 1,
    KK_UI_KEY_OK = 1U << 2
};

typedef struct {
    uint8_t keys;
    int16_t encoder_delta; /* Positive is Down; negative is Up. */
} KK_UI_Input;

typedef enum {
    KK_UI_INPUT_UP = 0,
    KK_UI_INPUT_DOWN,
    KK_UI_INPUT_OK
} KK_UI_InputAction;

typedef enum {
    KK_UI_INPUT_PRESS = 0,
    KK_UI_INPUT_REPEAT,
    KK_UI_INPUT_ENCODER
} KK_UI_InputSource;

typedef struct {
    KK_UI_InputAction action;
    KK_UI_InputSource source;
    uint16_t steps;
} KK_UI_InputEvent;

typedef enum {
    KK_UI_PAGE_HOME = 0,
    KK_UI_PAGE_MENU,
    KK_UI_PAGE_INFO,
    KK_UI_PAGE_CUSTOM
} KK_UI_PageType;

typedef struct {
    KK_UI_PageType type;
    uint16_t index;
} KK_UI_PageRoute;

typedef struct {
    const char *label;
    const uint8_t *icon_xbm_32x32;
    KK_UI_PageId target_page;
} KK_UI_HomeItem;

typedef struct {
    const KK_UI_HomeItem *items;
    uint16_t item_count;
} KK_UI_HomePage;

typedef enum {
    KK_UI_MENU_PAGE = 0,
    KK_UI_MENU_ACTION,
    KK_UI_MENU_INT,
    KK_UI_MENU_BOOL,
    KK_UI_MENU_CONFIRM
} KK_UI_MenuItemType;

typedef struct {
    const char *label;
    KK_UI_MenuItemType type;
    uint16_t ref;
} KK_UI_MenuItem;

typedef struct {
    const char *title;
    const KK_UI_MenuItem *items;
    uint8_t *item_states; /* Optional packed 2-bit state table. */
    uint16_t item_count;
} KK_UI_MenuPage;

typedef enum {
    KK_UI_ITEM_NORMAL = 0,
    KK_UI_ITEM_HIDDEN = 1,
    KK_UI_ITEM_DISABLED = 2,
    KK_UI_ITEM_RESERVED = 3
} KK_UI_ItemState;

typedef struct {
    const char *name;
    const char *value;
} KK_UI_InfoRow;

typedef struct {
    const char *title; /* NULL or empty omits the title bar. */
    const KK_UI_InfoRow *rows;
    uint16_t row_count;
} KK_UI_InfoPage;

typedef struct {
    const char *title;
    int32_t *value;
    int32_t minimum;
    int32_t maximum;
    uint32_t step;
    const char *unit;
    KK_UI_EventId changed_event;
    /* value 是 ×10^decimals 的整数刻度，只影响显示：0 = 原样显示（默认，
     * 老代码不写这个字段也能用），2 = 显示两位小数（37500 -> "375.00"）。 */
    uint8_t decimals;
    /* 非 NULL = 这个整数其实是"枚举下标"，菜单行和编辑框都不显示数字，
     * 显示 names[value - minimum]（例：灰度接口 0/1/2/3 -> 自动/I2C/串口/数字）。
     * NULL = 老行为（数字 + unit）；老代码不写这个字段也能用。 */
    const char *const *names;
} KK_UI_IntBinding;

typedef struct {
    const char *title;
    bool *value;
    KK_UI_EventId changed_event;
} KK_UI_BoolBinding;

typedef struct {
    const char *text;
    KK_UI_EventId confirmed_event;
    KK_UI_EventId cancelled_event;
} KK_UI_ConfirmDesc;

typedef struct {
    const uint8_t *home_font;
    const uint8_t *title_font;
    const uint8_t *body_font;
} KK_UI_Fonts;

typedef struct {
    const char *return_text;
    const char *cancel_text;
    const char *confirm_text;
    const char *on_text;
    const char *off_text;
    const char *message_title;
} KK_UI_Texts;

typedef struct {
    KK_UI_PageId root_page;
    const KK_UI_PageRoute *routes;
    uint16_t route_count;
    const KK_UI_HomePage *home_pages;
    uint16_t home_page_count;
    const KK_UI_MenuPage *menu_pages;
    uint16_t menu_page_count;
    const KK_UI_InfoPage *info_pages;
    uint16_t info_page_count;
    uint16_t custom_page_count;
    const KK_UI_IntBinding *int_bindings;
    uint16_t int_binding_count;
    const KK_UI_BoolBinding *bool_bindings;
    uint16_t bool_binding_count;
    const KK_UI_ConfirmDesc *confirm_descs;
    uint16_t confirm_desc_count;
    KK_UI_Fonts fonts;
    KK_UI_Texts texts;
} KK_UI_App;

KK_UI_Status KK_UI_Init(const KK_UI_App *app);
KK_UI_Status KK_UI_Update(uint32_t now_ms, KK_UI_Input input);
void KK_UI_Invalidate(void);
KK_UI_Status KK_UI_RecoverDisplay(void);

bool KK_UI_PollEvent(KK_UI_EventId *out_event);
bool KK_UI_PollError(KK_UI_ErrorInfo *out_error);
KK_UI_Status KK_UI_SetMenuItemState(KK_UI_PageId page,
                                    uint16_t item_index,
                                    KK_UI_ItemState state);

KK_UI_Status KK_UI_OpenIntEditor(uint16_t binding_index);
KK_UI_Status KK_UI_OpenBoolEditor(uint16_t binding_index);
KK_UI_Status KK_UI_OpenConfirm(uint16_t confirm_index);
KK_UI_Status KK_UI_ShowMessage(const char *text, KK_UI_EventId acknowledged_event);
KK_UI_Status KK_UI_ShowToast(const char *text, uint32_t duration_ms);

KK_UI_Status KK_UI_CustomRequestClose(void);
KK_UI_Status KK_UI_CustomFinish(KK_UI_EventId event);

/* ---- 公共导航 API ----
 * 以前只在内部头 kk_ui_internal.h 里声明，应用侧靠 extern 自己写一份；放到
 * 公共头里，签名改了编译器能抓到（审计 agent 4）。 */
KK_UI_Status KK_UI_NavigateTo(KK_UI_PageId page, uint32_t now);
KK_UI_Status KK_UI_NavigateToDirected(KK_UI_PageId page, bool reverse,
                                      uint32_t now);
KK_UI_Status KK_UI_NavigateBack(uint32_t now);
KK_UI_Status KK_UI_NavigateBackDirected(bool reverse, uint32_t now);
/* 指定"下一次进入某页"时的初始选中项（一次性，导航时消费） */
void KK_UI_SetEntrySelection(uint16_t index);

/* ---- 可选的 weak 钩子 ----
 * 定义在 kk_ui.c / kk_ui_home.c 里，应用写强定义覆盖；这里给原型，
 * 让编译器交叉校验签名（审计 agent 4）。 */
void KK_UI_OnKeyPress(uint8_t keys, uint32_t now);
bool KK_UI_HomeOnMove(int16_t direction, uint16_t selected,
                      KK_UI_InputSource source);

#if KK_UI_ENABLE_CUSTOM
/* Fixed application adapter. The application defines these five symbols. */
void KK_UI_CustomOnEnter(KK_UI_PageId page);
void KK_UI_CustomOnLeave(KK_UI_PageId page);
void KK_UI_CustomOnInput(KK_UI_PageId page, KK_UI_InputEvent event);
bool KK_UI_CustomOnTick(KK_UI_PageId page, uint32_t now_ms);
void KK_UI_CustomOnDraw(KK_UI_PageId page, int16_t x_offset,
                        int16_t clip_x, uint16_t clip_width);
#endif

#ifdef __cplusplus
}
#endif

#endif
