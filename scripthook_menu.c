/* A shared menu, one root owned by the API. Every plugin
 * registers a submenu, so sixteen addons cost sixteen rows. */
#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#define SH_BUILD 1
#include "scripthook.h"
#include "scripthook_menu_overlay.h"

#ifndef SH_MENU_OVERLAY_ENABLED
#define SH_MENU_OVERLAY_ENABLED 0
#endif

extern void ShMenuSuppressKeys(int on, int toggleVk);
extern void ShLangCopyMenu(const char *owner, const char *key,
                           char *dst, size_t cap);
extern void ShLangCopySort(const char *owner, const char *key,
                           char *dst, size_t cap);
extern int ShLangCopyEnglish(const char *owner, const char *key,
                              char *dst, size_t cap);
extern int ShLangHasDeclared(const char *owner, const char *key);
extern int ShLangFormatMenuV(char *dst, size_t cap, const char *en,
                              const char *tr, va_list ap);

#define MENUS       128
#define ITEMS       96
#define LABEL       48
#define VISIBLE     12
#define TICK_MS     40
#define OPTS        64

/* Geometry in HUD pixels. */
#define MENU_X      20.0f
#define MENU_Y      20.0f
#define MENU_W      520.0f
#define PAD         20.0f
#define TITLE_H     44.0f
#define ROW_H       34.0f
#define BAR_DY      -5.0f
#define BAR_H       27.0f
#define VALUE_W     190.0f
#define CREDIT_X     280.0f
#define CREDIT_W     245.0f
#define BODY_SCALE   1.20f
#define HEADER_SCALE 1.60f
#define CREDIT_SCALE 0.85f

#define C_TITLE     0xFFFFFFu
#define C_ROW       0xFFFFFFu
#define C_SEL       0xFFFFFFu
#define C_FOOT      0xFFFFFFu
#define C_STATUS    0xA0E6A0u
#define C_BAR       0x505050u

enum { IT_ACTION = 0, IT_SUB, IT_TOGGLE, IT_NUMBER, IT_LIST };

typedef struct {
    int      used;
    int      kind;
    char     label[LABEL];
    uint32_t sub;
    int      value;
    float    num, lo, hi, step;
    const char *opts[OPTS];
    int      nopts;
    int      rootPlugin;
    ShMenuFn fn;
    void    *user;
} Item;

typedef struct {
    int      used;
    char     title[LABEL];
    char     owner[48];
    char     hint[384];
    char     status[96];
    uint32_t parent;
    int      sel;
    int      top;
    int      count;
    Item     items[ITEMS];
} Menu;

/* What is on screen, so only differences are pushed. */
typedef ShMenuOverlayRow RowView;
typedef ShMenuOverlayView View;

static Menu g_menus[MENUS];
static uint32_t g_root = 0;
static uint32_t g_settings = 0;
static int g_scaleChoice = 1;
static const float g_scaleMultiplier[4] = {0.85f, 1.0f, 1.25f, 1.50f};
static const char *g_scaleOptions[4] = {"Small", "Default", "Large", "XLarge"};
static int g_fontChoice = 0;
static const char *g_fontOptions[6] = {
    "Segoe UI", "Arial", "Tahoma", "Verdana", "Trebuchet MS", "Georgia"
};
static int g_fontSizeChoice = 5;
static const char *g_fontSizeOptions[8] = {
    "16", "18", "20", "22", "24", "26", "28", "30"
};
static const int g_fontSizes[8] = {16, 18, 20, 22, 24, 26, 28, 30};
static int g_textColourChoice = 0;
static const char *g_textColourOptions[8] = {
    "White", "Light Gray", "Green", "Cyan", "Yellow", "Orange", "Red",
    "Custom"
};
static const unsigned g_textColours[7] = {
    0xFFFFFFu, 0xD2D2D2u, 0x80E080u, 0x80E8FFu,
    0xFFE070u, 0xFFA040u, 0xFF7070u
};
static int g_customTextColour[3] = {255, 255, 255};
static int g_backgroundOpacityChoice = 9;
static const char *g_backgroundOpacityOptions[11] = {
    "0%", "10%", "20%", "30%", "40%", "50%",
    "60%", "70%", "80%", "90%", "100%"
};
static const unsigned g_backgroundAlpha[11] = {
    0, 26, 51, 77, 102, 128, 153, 179, 204, 230, 255
};
static int g_accentColourChoice = 8;
static const char *g_accentColourOptions[9] = {
    "White", "Light Gray", "Green", "Cyan", "Yellow", "Orange", "Red",
    "Custom", "Default"
};
static int g_customAccentColour[3] = {58, 80, 96};
static int g_openOnStartupChoice = 0;
static const char *g_yesNoOptions[2] = {"Yes", "No"};
static volatile uint32_t g_current = 0;
static volatile int g_open = 0;
static volatile int g_key = VK_F4;
static volatile LONG g_greeted = 0;
static volatile LONG g_playing = 0;
static volatile LONG g_autoOpenPending = 0;
static volatile int g_started = 0;
static volatile LONG g_overlayReady = 0;
static CRITICAL_SECTION g_lock;
static volatile int g_lockReady = 0;

/* Native widget ids, valid for one UI generation. */
static struct {
    int      built, gen, shown;
    int      scaleChoice;
    uint32_t panel, title, credit, bar, hint, footer, status;
    uint32_t name[VISIBLE], value[VISIBLE];
    View     drawn;
} g_ui;

extern void ShSetError(int err);
extern int ShPluginHidden(const char *owner);
extern int ShGhostSettingsGetStr(const char *key, char *out, int size);
extern int ShGhostSettingsSetStr(const char *key, const char *value);

static void Lock(void) { if (g_lockReady) EnterCriticalSection(&g_lock); }
static void Unlock(void) { if (g_lockReady) LeaveCriticalSection(&g_lock); }

static Menu *MenuOf(uint32_t h) {
    if (h == 0 || h > MENUS) return NULL;
    if (!g_menus[h - 1].used) return NULL;
    return &g_menus[h - 1];
}

static uint32_t NewMenu(const char *title, uint32_t parent,
                        const char *owner) {
    int i;

    for (i = 0; i < MENUS; i++) {
        if (g_menus[i].used) continue;
        memset(&g_menus[i], 0, sizeof(g_menus[i]));
        g_menus[i].used = 1;
        g_menus[i].parent = parent;
        if (owner) {
            strncpy(g_menus[i].owner, owner, sizeof(g_menus[i].owner) - 1);
            g_menus[i].owner[sizeof(g_menus[i].owner) - 1] = 0;
        }
        if (title) {
            strncpy(g_menus[i].title, title, LABEL - 1);
            g_menus[i].title[LABEL - 1] = 0;
        }
        return (uint32_t)(i + 1);
    }
    return 0;
}

static void OwnerFromAddress(void *caller, char *out, size_t cap) {
    HMODULE module = NULL;
    char path[MAX_PATH];
    const char *name;
    size_t n;
    if (!cap) return;
    out[0] = 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)caller, &module) || !module) return;
    if (!GetModuleFileNameA(module, path, sizeof(path))) return;
    name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    if (!_stricmp(name, "dinput8.dll") || !_stricmp(name, "GRW.exe")) return;
    n = strlen(name);
    if (n > 4 && (!_stricmp(name + n - 4, ".asi") ||
                  !_stricmp(name + n - 4, ".dll"))) n -= 4;
    if (n >= cap) n = cap - 1;
    memcpy(out, name, n);
    out[n] = 0;
}

/* Free a menu and everything under it. Caller holds the
 * lock. Depth is bounded by MENUS, so recursion is safe. */
static void DropMenu(uint32_t h) {
    Menu *m = MenuOf(h);
    int i;

    if (!m) return;
    for (i = 0; i < m->count; i++)
        if (m->items[i].kind == IT_SUB && m->items[i].sub)
            DropMenu(m->items[i].sub);
    memset(m, 0, sizeof(*m));
}

static Item *NewItem(Menu *m, int kind, const char *label,
                     ShMenuFn fn, void *user) {
    Item *it;

    if (!m || m->count >= ITEMS) return NULL;
    it = &m->items[m->count++];
    memset(it, 0, sizeof(*it));
    it->used = 1;
    it->kind = kind;
    it->fn = fn;
    it->user = user;
    if (label) {
        strncpy(it->label, label, LABEL - 1);
        it->label[LABEL - 1] = 0;
    }
    return it;
}

/* Rendered text for the value side of a row. */
static void ValueText(const char *owner, const Item *it, char *out, int n) {
    out[0] = 0;
    if (it->kind == IT_SUB) snprintf(out, n, ">");
    else if (it->kind == IT_TOGGLE)
        snprintf(out, n, it->value ? "[on]" : "[off]");
    else if (it->kind == IT_NUMBER)
        snprintf(out, n, "< %.2f >", it->num);
    else if (it->kind == IT_LIST && it->nopts) {
        char option[28];
        ShLangCopyMenu(owner, it->opts[it->value % it->nopts],
                       option, sizeof(option));
        snprintf(out, n, "< %s >", option);
    }
}

/* Receivers run on the menu thread, so the lock is dropped
 * around the call and any API call is legal inside one.
 */
static void Fire(uint32_t menu, int idx, Item *it) {
    ShMenuFn fn = it->fn;
    void *user = it->user;
    int v = (it->kind == IT_NUMBER) ? (int)it->num : it->value;

    if (!fn) return;
    Unlock();
    fn(menu, (uint32_t)idx, v, user);
    Lock();
}

static int Pressed(int vk) {
    static unsigned char was[256];
    int d = (GetAsyncKeyState(vk) & 0x8000) != 0;
    int hit = d && !was[vk & 0xFF];
    was[vk & 0xFF] = (unsigned char)d;
    return hit;
}

/* Selection scrolls with the cursor, so a long menu shows a
 * window of rows rather than running off the screen.
 */
static void Scroll(Menu *m) {
    if (m->sel < m->top) m->top = m->sel;
    if (m->sel >= m->top + VISIBLE) m->top = m->sel - VISIBLE + 1;
    if (m->top < 0) m->top = 0;
}

static void ClampScroll(Menu *m) {
    int maxTop;
    if (m->count == 0) { m->sel = 0; m->top = 0; return; }
    if (m->sel < 0) m->sel = 0;
    if (m->sel >= m->count) m->sel = m->count - 1;
    maxTop = m->count > VISIBLE ? m->count - VISIBLE : 0;
    if (m->top > maxTop) m->top = maxTop;
    Scroll(m);
}

static int RootLabelCmp(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
        if (ca != cb) return (int)ca - (int)cb;
        if (!ca) return 0;
    }
}

/* Insert the newly registered plugin among plugin rows only.
 * Non-plugin root rows keep their slots; equal labels stay stable. */
static void InsertRootPlugin(Menu *root) {
    Item added = root->items[root->count - 1];
    int scan, slot = root->count - 1;

    /* The GhostHook-owned Settings row is fixed after every plugin row. */
    if (slot > 0 && root->items[slot - 1].sub == g_settings) {
        root->items[slot] = root->items[slot - 1];
        slot--;
    }

    for (scan = slot - 1; scan >= 0; scan--) {
        if (!root->items[scan].rootPlugin) continue;
        char prior[LABEL], next[LABEL];
        Menu *priorMenu = MenuOf(root->items[scan].sub);
        Menu *nextMenu = MenuOf(added.sub);
        ShLangCopySort(priorMenu ? priorMenu->owner : "",
                       root->items[scan].label, prior, sizeof(prior));
        ShLangCopySort(nextMenu ? nextMenu->owner : "",
                       added.label, next, sizeof(next));
        if (RootLabelCmp(prior, next) <= 0) break;
        root->items[slot] = root->items[scan];
        slot = scan;
    }
    root->items[slot] = added;
}

static int RootItemHidden(Menu *m, int index) {
    Menu *child;
    if (m != MenuOf(g_root) || !m->items[index].rootPlugin)
        return 0;
    child = MenuOf(m->items[index].sub);
    return child && ShPluginHidden(child->owner);
}

static int RootHasPlugin(const Menu *root) {
    int i;
    if (!root) return 0;
    for (i = 0; i < root->count; ++i)
        if (root->items[i].rootPlugin) return 1;
    return 0;
}

static void Navigate(void) {
    Menu *m = MenuOf(g_current);
    Item *it;
    int left, right, up, down, step, n;

    if (!m || m->count == 0) return;
    up = Pressed(VK_UP);
    down = Pressed(VK_DOWN);
    step = up ? -1 : down ? 1 : 0;
    if (step || RootItemHidden(m, m->sel)) {
        int next = m->sel;
        for (n = 0; n < m->count; ++n) {
            next = (next + (step ? step : 1) + m->count) % m->count;
            if (!RootItemHidden(m, next)) { m->sel = next; break; }
        }
    }
    Scroll(m);

    it = &m->items[m->sel];
    if (RootItemHidden(m, m->sel)) {
        if (Pressed(VK_BACK) || Pressed(VK_ESCAPE)) g_open = 0;
        return;
    }
    left = Pressed(VK_LEFT);
    right = Pressed(VK_RIGHT);
    if (left || right) {
        int dir = right ? 1 : -1;
        if (it->kind == IT_NUMBER) {
            it->num += it->step * dir;
            if (it->num < it->lo) it->num = it->lo;
            if (it->num > it->hi) it->num = it->hi;
            Fire(g_current, m->sel, it);
        } else if (it->kind == IT_LIST && it->nopts) {
            it->value = (it->value + it->nopts + dir) % it->nopts;
            Fire(g_current, m->sel, it);
        }
    }
    if (Pressed(VK_RETURN)) {
        if (it->kind == IT_SUB && it->sub) {
            g_current = it->sub;
        } else if (it->kind == IT_TOGGLE) {
            it->value = !it->value;
            Fire(g_current, m->sel, it);
        } else {
            Fire(g_current, m->sel, it);
        }
    }
    if (Pressed(VK_BACK) || Pressed(VK_ESCAPE)) {
        if (m->parent) g_current = m->parent;
        else g_open = 0;
    }
}

/* Snapshot of the current menu. Caller holds the lock. */
static void Capture(View *v) {
    Menu *m = MenuOf(g_current);
    int i;

    memset(v, 0, sizeof(*v));
    if (!m) return;
    v->isRoot = (g_current == g_root);
    ShLangCopyMenu(m->owner, m->title, v->title, sizeof(v->title));
    if (!v->isRoot) {
        if (m->hint[0])
            ShLangCopyMenu(m->owner, m->hint, v->hint, sizeof(v->hint));
        else if (m->title[0] == '@') {
            char hintKey[LABEL + 16];
            snprintf(hintKey, sizeof(hintKey), "%s.hint", m->title);
            if (ShLangHasDeclared(m->owner, hintKey))
                ShLangCopyMenu(m->owner, hintKey,
                               v->hint, sizeof(v->hint));
        }
    }
    ShLangCopyMenu(m->owner, m->status, v->status, sizeof(v->status));
    for (i = m->top; i < m->count && v->rows < VISIBLE; i++) {
        RowView *r = &v->row[v->rows];
        const char *owner = m->owner;
        if (RootItemHidden(m, i)) continue;
        if (m->items[i].kind == IT_SUB) {
            Menu *child = MenuOf(m->items[i].sub);
            if (child && child->owner[0]) owner = child->owner;
        }
        ShLangCopyMenu(owner, m->items[i].label,
                       r->name, sizeof(r->name));
        ValueText(m->owner, &m->items[i], r->value, sizeof(r->value));
        r->shown = 1;
        r->selected = (i == m->sel);
        if (r->selected) v->sel = v->rows;
        v->rows++;
    }
    if (m->count > VISIBLE)
        snprintf(v->footer, sizeof(v->footer), "%d of %d",
                 m->sel + 1, m->count);
}

static float RowY(int i) {
    return PAD + TITLE_H + ROW_H * (float)i;
}

static float PanelHeight(const View *v) {
    float h = PAD + TITLE_H + ROW_H * (float)v->rows + PAD;
    if (v->hint[0]) h += ROW_H;
    if (v->footer[0]) h += ROW_H;
    if (v->status[0]) h += ROW_H;
    return h;
}

static void DropWidgets(void) {
    if (g_ui.built && g_ui.gen == ShUiGen() && g_ui.panel)
        ShUiDestroy(g_ui.panel);
    memset(&g_ui, 0, sizeof(g_ui));
}

/* Every widget of the menu, or none of it. A single create
 * can fail when the game state flickers, and a menu missing
 * half its rows never repaired itself. */
static int Complete(void) {
    int i;

    if (!g_ui.panel || !g_ui.bar || !g_ui.title || !g_ui.credit) return 0;
    if (!g_ui.hint || !g_ui.footer || !g_ui.status) return 0;
    for (i = 0; i < VISIBLE; i++)
        if (!g_ui.name[i] || !g_ui.value[i]) return 0;
    return 1;
}

/* One creation per widget, hidden rows included, so later
 * updates are text and position only. */
static int BuildWidgets(void) {
    int i;
    int batch = 0, ok = 1;
    float panelScale[3] = {g_scaleMultiplier[g_scaleChoice],
                           g_scaleMultiplier[g_scaleChoice], 1.0f};
    float bodyScale[3] = { BODY_SCALE, BODY_SCALE, 1.0f };
    float headerScale[3] = { HEADER_SCALE, HEADER_SCALE, 1.0f };
    float creditScale[3] = { CREDIT_SCALE, CREDIT_SCALE, 1.0f };

    memset(&g_ui, 0, sizeof(g_ui));
    if (!ShUiReady()) return 0;
    g_ui.gen = ShUiGen();
    g_ui.panel = ShUiPanel(MENU_X, MENU_Y, MENU_W, 200.0f, 0x000000, 0.8f);
    if (!g_ui.panel) return 0;
    ShUiShow(g_ui.panel, 0);
    g_ui.bar = ShUiImage(g_ui.panel, PAD / 2, RowY(0) + BAR_DY,
                         MENU_W - PAD, BAR_H, C_BAR, 0.9f);
    g_ui.title = ShUiLabel(g_ui.panel, PAD, PAD, MENU_W - 2 * PAD,
                           TITLE_H, " ", C_TITLE);
    g_ui.credit = ShUiLabel(g_ui.panel, CREDIT_X, PAD + 12.0f, CREDIT_W,
                            TITLE_H, "Powered by Phiality's ScriptHook", C_TITLE);
    for (i = 0; i < VISIBLE; i++) {
        g_ui.name[i] = ShUiLabel(g_ui.panel, PAD + 8.0f, RowY(i),
                                 MENU_W - VALUE_W - PAD, ROW_H, " ",
                                 C_ROW);
        g_ui.value[i] = ShUiLabel(g_ui.panel, MENU_W - PAD - VALUE_W,
                                  RowY(i), VALUE_W, ROW_H, " ", C_ROW);
    }
    g_ui.hint = ShUiLabel(g_ui.panel, PAD, RowY(0), MENU_W - 2 * PAD,
                          ROW_H, " ", C_FOOT);
    g_ui.footer = ShUiLabel(g_ui.panel, PAD, RowY(0), MENU_W - 2 * PAD,
                            ROW_H, " ", C_FOOT);
    g_ui.status = ShUiLabel(g_ui.panel, PAD, RowY(0), MENU_W - 2 * PAD,
                            ROW_H, " ", C_STATUS);

    /* Destroying the panel takes the subtree with it, so the
     * next tick starts clean instead of leaking slots. */
    if (!Complete()) goto failed;

    if (!ShUiBegin()) goto failed;
    batch = 1;
    if (!ShUiSetV(g_ui.panel, SH_P_SCALE, panelScale, 3)) ok = 0;
    if (!ShUiSetV(g_ui.title, SH_P_SCALE, headerScale, 3)) ok = 0;
    if (!ShUiSetV(g_ui.credit, SH_P_SCALE, creditScale, 3)) ok = 0;
    if (!ShUiShow(g_ui.credit, 0)) ok = 0;
    for (i = 0; i < VISIBLE; i++) {
        if (!ShUiSetV(g_ui.name[i], SH_P_SCALE, bodyScale, 3)) ok = 0;
        if (!ShUiSetV(g_ui.value[i], SH_P_SCALE, bodyScale, 3)) ok = 0;
        if (!ShUiShow(g_ui.name[i], 0)) ok = 0;
        if (!ShUiShow(g_ui.value[i], 0)) ok = 0;
    }
    if (!ShUiSetV(g_ui.hint, SH_P_SCALE, bodyScale, 3)) ok = 0;
    if (!ShUiSetV(g_ui.footer, SH_P_SCALE, bodyScale, 3)) ok = 0;
    if (!ShUiSetV(g_ui.status, SH_P_SCALE, bodyScale, 3)) ok = 0;
    if (!ShUiShow(g_ui.hint, 0)) ok = 0;
    if (!ShUiShow(g_ui.footer, 0)) ok = 0;
    if (!ShUiShow(g_ui.status, 0)) ok = 0;
    if (!ok) goto failed;
    batch = 0;
    if (!ShUiCommit()) goto failed;

    g_ui.built = 1;
    g_ui.shown = 0;
    g_ui.scaleChoice = g_scaleChoice;
    return 1;

failed:
    if (batch) ShUiAbort();
    if (g_ui.panel) ShUiDestroy(g_ui.panel);
    memset(&g_ui, 0, sizeof(g_ui));
    return 0;
}

static void TryAutoOpen(void) {
    Menu *root;

    if (
#if SH_MENU_OVERLAY_ENABLED
        !InterlockedCompareExchange(&g_overlayReady, 0, 0) ||
#else
        !g_ui.built ||
#endif
        !InterlockedCompareExchange(&g_playing, 0, 0) ||
        !InterlockedCompareExchange(&g_autoOpenPending, 0, 0))
        return;
    if (g_openOnStartupChoice != 0) {
        InterlockedExchange(&g_autoOpenPending, 0);
        return;
    }
    if (InterlockedCompareExchange(&g_greeted, 0, 0)) {
        InterlockedExchange(&g_autoOpenPending, 0);
        return;
    }

    Lock();
    root = MenuOf(g_root);
    if (RootHasPlugin(root) &&
        InterlockedCompareExchange(&g_playing, 0, 0) &&
        InterlockedCompareExchange(&g_autoOpenPending, 0, 0) &&
        InterlockedCompareExchange(&g_greeted, 1, 0) == 0) {
        strncpy(root->status, "F4 opens this menu",
                sizeof(root->status) - 1);
        root->status[sizeof(root->status) - 1] = 0;
        InterlockedExchange(&g_autoOpenPending, 0);
        g_current = g_root;
        g_open = 1;
    }
    Unlock();
}

static void SetTextIf(uint32_t id, char *have, int cap,
                      const char *want) {
    if (strcmp(have, want) == 0) return;
    strncpy(have, want, cap - 1);
    have[cap - 1] = 0;
    ShUiSetText(id, want[0] ? want : " ");
}

/* Push the differences between the drawn view and v. */
static void Sync(const View *v) {
    View *d = &g_ui.drawn;
    float hintShift = v->hint[0] ? ROW_H : 0.0f;
    int hintChanged = (v->hint[0] != 0) != (d->hint[0] != 0);
    float y;
    int i;

    SetTextIf(g_ui.title, d->title, LABEL, v->title);
    if (v->isRoot != d->isRoot) {
        if (g_ui.credit)
            ShUiShow(g_ui.credit, v->isRoot);
        d->isRoot = v->isRoot;
    }
    for (i = 0; i < VISIBLE; i++) {
        const RowView *r = &v->row[i];
        RowView *dr = &d->row[i];

        if (r->shown != dr->shown) {
            ShUiShow(g_ui.name[i], r->shown);
            ShUiShow(g_ui.value[i], r->shown);
            dr->shown = r->shown;
        }
        if (!r->shown) continue;
        SetTextIf(g_ui.name[i], dr->name, LABEL, r->name);
        SetTextIf(g_ui.value[i], dr->value, sizeof(dr->value), r->value);
        if (r->selected != dr->selected) {
            ShUiSetColour(g_ui.name[i], r->selected ? C_SEL : C_ROW);
            ShUiSetColour(g_ui.value[i], r->selected ? C_SEL : C_ROW);
            dr->selected = r->selected;
        }
    }
    if (hintChanged)
        for (i = 0; i < VISIBLE; i++) {
            ShUiSetPos(g_ui.name[i], PAD + 8.0f, RowY(i) + hintShift);
            ShUiSetPos(g_ui.value[i], MENU_W - PAD - VALUE_W,
                       RowY(i) + hintShift);
        }
    if (v->sel != d->sel || v->rows != d->rows || hintChanged) {
        ShUiSetPos(g_ui.bar, PAD / 2,
                   RowY(v->sel) + hintShift + BAR_DY);
        d->sel = v->sel;
    }
    y = RowY(v->rows) + hintShift;
    if (v->rows != d->rows || strcmp(v->hint, d->hint) != 0 ||
        strcmp(v->footer, d->footer) != 0 ||
        strcmp(v->status, d->status) != 0) {
        if (v->hint[0]) ShUiSetPos(g_ui.hint, PAD, RowY(0) + 2.0f);
        if (v->footer[0]) {
            ShUiSetPos(g_ui.footer, PAD, y + 2.0f);
            y += ROW_H;
        }
        if (v->status[0]) ShUiSetPos(g_ui.status, PAD, y + 2.0f);
        if ((v->hint[0] != 0) != (d->hint[0] != 0))
            ShUiShow(g_ui.hint, v->hint[0] != 0);
        if ((v->footer[0] != 0) != (d->footer[0] != 0))
            ShUiShow(g_ui.footer, v->footer[0] != 0);
        if ((v->status[0] != 0) != (d->status[0] != 0))
            ShUiShow(g_ui.status, v->status[0] != 0);
        SetTextIf(g_ui.hint, d->hint, sizeof(d->hint), v->hint);
        SetTextIf(g_ui.footer, d->footer, sizeof(d->footer), v->footer);
        SetTextIf(g_ui.status, d->status, sizeof(d->status), v->status);
        ShUiSetSize(g_ui.panel, MENU_W, PanelHeight(v));
        d->rows = v->rows;
    }
}

/* Keys are polled here, the engine draws the result. */
static DWORD WINAPI MenuThread(LPVOID p) {
#if !SH_MENU_OVERLAY_ENABLED
    View v;
#endif
    (void)p;

    for (;;) {
        Sleep(TICK_MS);

#if SH_MENU_OVERLAY_ENABLED
        if (InterlockedCompareExchange(&g_overlayReady, 0, 0) && Pressed(g_key)) {
#else
        if (Pressed(g_key)) {
#endif
            g_open = !g_open;
            if (g_open) {
                g_current = g_root;
                InterlockedExchange(&g_greeted, 1);
            }
            InterlockedExchange(&g_autoOpenPending, 0);
        }
#if SH_MENU_OVERLAY_ENABLED
        ShMenuSuppressKeys(g_open &&
                           InterlockedCompareExchange(&g_overlayReady, 0, 0),
                           g_key);
        if (!InterlockedCompareExchange(&g_overlayReady, 0, 0)) continue;
        TryAutoOpen();
        if (!g_open) continue;

        Lock();
        Navigate();
        Unlock();
#else
        ShMenuSuppressKeys(g_open, g_key);
        if (g_ui.built && g_ui.gen != ShUiGen()) DropWidgets();
        if (!g_ui.built && !BuildWidgets()) continue;
        TryAutoOpen();
        if (!g_open) {
            if (g_ui.built && g_ui.shown) {
                ShUiShow(g_ui.panel, 0);
                g_ui.shown = 0;
            }
            continue;
        }

        Lock();
        Navigate();
        Capture(&v);
        Unlock();

        if (g_ui.scaleChoice != g_scaleChoice) {
            float scale[3] = {g_scaleMultiplier[g_scaleChoice],
                              g_scaleMultiplier[g_scaleChoice], 1.0f};
            if (!ShUiSetV(g_ui.panel, SH_P_SCALE, scale, 3)) {
                DropWidgets();
                continue;
            }
            g_ui.scaleChoice = g_scaleChoice;
        }

        Sync(&v);
        if (!g_ui.shown) {
            ShUiShow(g_ui.panel, 1);
            g_ui.shown = 1;
        }
#endif
    }
    return 0;
}

void ShMenuOverlaySetReady(int ready) {
    InterlockedExchange(&g_overlayReady, ready ? 1 : 0);
    if (!ready) ShMenuSuppressKeys(0, g_key);
}

int ShMenuOverlayCapture(ShMenuOverlayView *view) {
    if (!view || !g_open ||
        !InterlockedCompareExchange(&g_overlayReady, 0, 0))
        return 0;
    Lock();
    Capture(view);
    Unlock();
    return view->title[0] != 0;
}

float ShMenuOverlayScale(void) {
    int choice = g_scaleChoice;
    if (choice < 0 || choice >= 4) choice = 1;
    return g_scaleMultiplier[choice];
}

int ShMenuOverlayFontChoice(void) {
    int choice = g_fontChoice;
    return (choice >= 0 && choice < 6) ? choice : 0;
}

int ShMenuOverlayFontSize(void) {
    int choice = g_fontSizeChoice;
    return (choice >= 0 && choice < 8) ? g_fontSizes[choice] : 26;
}

unsigned ShMenuOverlayTextColour(void) {
    int choice = g_textColourChoice;
    if (choice >= 0 && choice < 7) return g_textColours[choice];
    if (choice == 7)
        return ((unsigned)g_customTextColour[0] << 16) |
               ((unsigned)g_customTextColour[1] << 8) |
               (unsigned)g_customTextColour[2];
    return 0xFFFFFFu;
}

unsigned ShMenuOverlayBackgroundAlpha(void) {
    int choice = g_backgroundOpacityChoice;
    return (choice >= 0 && choice < 11) ? g_backgroundAlpha[choice] : 230;
}

unsigned ShMenuOverlayAccentColour(void) {
    int choice = g_accentColourChoice;
    if (choice >= 0 && choice < 7) return g_textColours[choice];
    if (choice == 7)
        return ((unsigned)g_customAccentColour[0] << 16) |
               ((unsigned)g_customAccentColour[1] << 8) |
               (unsigned)g_customAccentColour[2];
    return 0x3A5060u;
}

static int LoadFixedChoice(const char *key, const char **options,
                           int count, int fallback) {
    char value[32];
    int i;

    if (ShGhostSettingsGetStr(key, value, sizeof(value))) {
        for (i = 0; i < count; ++i)
            if (!_stricmp(value, options[i])) return i;
    }
    ShGhostSettingsSetStr(key, options[fallback]);
    return fallback;
}

static int LoadMenuScaling(void) {
    return LoadFixedChoice("MenuScaling", g_scaleOptions, 4, 1);
}

static int LoadColourChannel(const char *key, int fallback) {
    char value[32];
    char canonical[16];
    char *end;
    long parsed;
    int corrected = 0;

    if (!ShGhostSettingsGetStr(key, value, sizeof(value))) {
        snprintf(canonical, sizeof(canonical), "%d", fallback);
        ShGhostSettingsSetStr(key, canonical);
        return fallback;
    }
    end = NULL;
    parsed = strtol(value, &end, 10);
    if (end == value || !end || *end) {
        parsed = fallback;
        corrected = 1;
    } else if (parsed < 0) {
        parsed = 0;
        corrected = 1;
    } else if (parsed > 255) {
        parsed = 255;
        corrected = 1;
    }
    if (corrected) {
        snprintf(canonical, sizeof(canonical), "%ld", parsed);
        ShGhostSettingsSetStr(key, canonical);
    }
    return (int)parsed;
}

static void SetMenuScaling(uint32_t menu, uint32_t item, int value,
                           void *user) {
    (void)menu;
    (void)item;
    (void)user;

    if (value >= 0 && value < 4) {
        g_scaleChoice = value;
        ShGhostSettingsSetStr("MenuScaling", g_scaleOptions[value]);
    }
}

static void SetFont(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu;
    (void)item;
    (void)user;
    if (value >= 0 && value < 6) {
        g_fontChoice = value;
        ShGhostSettingsSetStr("Font", g_fontOptions[value]);
    }
}

static void SetFontSize(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu;
    (void)item;
    (void)user;
    if (value >= 0 && value < 8) {
        g_fontSizeChoice = value;
        ShGhostSettingsSetStr("FontSize", g_fontSizeOptions[value]);
    }
}

static void SetTextColour(uint32_t menu, uint32_t item, int value, void *user) {
    (void)menu;
    (void)item;
    (void)user;
    if (value >= 0 && value < 8) {
        g_textColourChoice = value;
        ShGhostSettingsSetStr("TextColor", g_textColourOptions[value]);
    }
}

static void SetCustomTextColour(uint32_t menu, uint32_t item, int value,
                                void *user) {
    static const char *keys[3] = {"TextColorR", "TextColorG", "TextColorB"};
    char text[16];
    int channel = (int)(uintptr_t)user;
    (void)menu;
    (void)item;
    if (channel < 0 || channel >= 3) return;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    g_customTextColour[channel] = value;
    snprintf(text, sizeof(text), "%d", value);
    ShGhostSettingsSetStr(keys[channel], text);
}

static void SetBackgroundOpacity(uint32_t menu, uint32_t item, int value,
                                 void *user) {
    (void)menu;
    (void)item;
    (void)user;
    if (value >= 0 && value < 11) {
        g_backgroundOpacityChoice = value;
        ShGhostSettingsSetStr("BackgroundOpacity",
                              g_backgroundOpacityOptions[value]);
    }
}

static void SetAccentColour(uint32_t menu, uint32_t item, int value,
                            void *user) {
    (void)menu;
    (void)item;
    (void)user;
    if (value >= 0 && value < 9) {
        g_accentColourChoice = value;
        ShGhostSettingsSetStr("AccentColor", g_accentColourOptions[value]);
    }
}

static void SetCustomAccentColour(uint32_t menu, uint32_t item, int value,
                                  void *user) {
    static const char *keys[3] = {
        "AccentColorR", "AccentColorG", "AccentColorB"
    };
    char text[16];
    int channel = (int)(uintptr_t)user;
    (void)menu;
    (void)item;
    if (channel < 0 || channel >= 3) return;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    g_customAccentColour[channel] = value;
    snprintf(text, sizeof(text), "%d", value);
    ShGhostSettingsSetStr(keys[channel], text);
}

static void SetOpenOnStartup(uint32_t menu, uint32_t item, int value,
                             void *user) {
    (void)menu;
    (void)item;
    (void)user;
    if (value >= 0 && value < 2) {
        g_openOnStartupChoice = value;
        ShGhostSettingsSetStr("OpenMenuOnStartup", g_yesNoOptions[value]);
    }
}

static void SetListOptions(Item *item, const char **options,
                           int count, int value) {
    int i;
    if (!item) return;
    for (i = 0; i < count; ++i) item->opts[i] = options[i];
    item->nopts = count;
    item->value = value;
}

static void SetNumberOptions(Item *item, int value, int channel) {
    if (!item) return;
    item->num = (float)value;
    item->lo = 0.0f;
    item->hi = 255.0f;
    item->step = 5.0f;
    item->user = (void *)(uintptr_t)channel;
}

static void EnsureMenu(void) {
    if (g_started) return;
    g_started = 1;
    InitializeCriticalSection(&g_lock);
    g_lockReady = 1;
    g_scaleChoice = LoadMenuScaling();
    g_fontChoice = LoadFixedChoice("Font", g_fontOptions, 6, 0);
    g_fontSizeChoice = LoadFixedChoice("FontSize", g_fontSizeOptions, 8, 5);
    g_textColourChoice = LoadFixedChoice("TextColor", g_textColourOptions, 8, 0);
    g_customTextColour[0] = LoadColourChannel("TextColorR", 255);
    g_customTextColour[1] = LoadColourChannel("TextColorG", 255);
    g_customTextColour[2] = LoadColourChannel("TextColorB", 255);
    g_backgroundOpacityChoice = LoadFixedChoice(
        "BackgroundOpacity", g_backgroundOpacityOptions, 11, 9);
    g_accentColourChoice = LoadFixedChoice(
        "AccentColor", g_accentColourOptions, 9, 8);
    g_customAccentColour[0] = LoadColourChannel("AccentColorR", 58);
    g_customAccentColour[1] = LoadColourChannel("AccentColorG", 80);
    g_customAccentColour[2] = LoadColourChannel("AccentColorB", 96);
    g_openOnStartupChoice = LoadFixedChoice(
        "OpenMenuOnStartup", g_yesNoOptions, 2, 0);
    g_root = NewMenu("GhostHook", 0, "");
    g_settings = NewMenu("GhostHook Settings", g_root, "");
    if (g_settings) {
        uint32_t custom = NewMenu("Custom Text Color", g_settings, "");
        uint32_t customAccent = NewMenu("Custom Accent Color", g_settings, "");
        Item *row = NewItem(MenuOf(g_root), IT_SUB,
                            "GhostHook Settings", NULL, NULL);
        Item *scale = NewItem(MenuOf(g_settings), IT_LIST,
                              "Menu Scaling", SetMenuScaling, NULL);
        Item *font = NewItem(MenuOf(g_settings), IT_LIST,
                             "Font", SetFont, NULL);
        Item *fontSize = NewItem(MenuOf(g_settings), IT_LIST,
                                 "Font Size", SetFontSize, NULL);
        Item *textColour = NewItem(MenuOf(g_settings), IT_LIST,
                                   "Text Color", SetTextColour, NULL);
        Item *customRow = NewItem(MenuOf(g_settings), IT_SUB,
                                  "Custom Text Color", NULL, NULL);
        Item *backgroundOpacity = NewItem(MenuOf(g_settings), IT_LIST,
                                          "Background Opacity",
                                          SetBackgroundOpacity, NULL);
        Item *accentColour = NewItem(MenuOf(g_settings), IT_LIST,
                                     "Accent Color", SetAccentColour, NULL);
        Item *customAccentRow = NewItem(MenuOf(g_settings), IT_SUB,
                                        "Custom Accent Color", NULL, NULL);
        Item *openOnStartup = NewItem(MenuOf(g_settings), IT_LIST,
                                      "Open Menu on Startup",
                                      SetOpenOnStartup, NULL);
        if (row) row->sub = g_settings;
        if (customRow) customRow->sub = custom;
        if (customAccentRow) customAccentRow->sub = customAccent;
        SetListOptions(scale, g_scaleOptions, 4, g_scaleChoice);
        SetListOptions(font, g_fontOptions, 6, g_fontChoice);
        SetListOptions(fontSize, g_fontSizeOptions, 8, g_fontSizeChoice);
        SetListOptions(textColour, g_textColourOptions, 8,
                       g_textColourChoice);
        SetListOptions(backgroundOpacity, g_backgroundOpacityOptions, 11,
                       g_backgroundOpacityChoice);
        SetListOptions(accentColour, g_accentColourOptions, 9,
                       g_accentColourChoice);
        SetListOptions(openOnStartup, g_yesNoOptions, 2,
                       g_openOnStartupChoice);
        if (custom) {
            Item *red = NewItem(MenuOf(custom), IT_NUMBER,
                                "Red", SetCustomTextColour, NULL);
            Item *green = NewItem(MenuOf(custom), IT_NUMBER,
                                  "Green", SetCustomTextColour, NULL);
            Item *blue = NewItem(MenuOf(custom), IT_NUMBER,
                                 "Blue", SetCustomTextColour, NULL);
            SetNumberOptions(red, g_customTextColour[0], 0);
            SetNumberOptions(green, g_customTextColour[1], 1);
            SetNumberOptions(blue, g_customTextColour[2], 2);
        }
        if (customAccent) {
            Item *red = NewItem(MenuOf(customAccent), IT_NUMBER,
                                "Red", SetCustomAccentColour, NULL);
            Item *green = NewItem(MenuOf(customAccent), IT_NUMBER,
                                  "Green", SetCustomAccentColour, NULL);
            Item *blue = NewItem(MenuOf(customAccent), IT_NUMBER,
                                 "Blue", SetCustomAccentColour, NULL);
            SetNumberOptions(red, g_customAccentColour[0], 0);
            SetNumberOptions(green, g_customAccentColour[1], 1);
            SetNumberOptions(blue, g_customAccentColour[2], 2);
        }
    }
    CreateThread(NULL, 0, MenuThread, NULL, 0, NULL);
}

SH_API uint32_t ShMenuCreate(const char *title) {
    void *caller = __builtin_return_address(0);
    char owner[48];
    Menu *root;
    uint32_t h;
    uint32_t selectedSub = 0;
    Item *it;
    int i;

    EnsureMenu();
    OwnerFromAddress(caller, owner, sizeof(owner));
    Lock();
    h = NewMenu(title, g_root, owner);
    root = MenuOf(g_root);
    if (h && root) {
        if (g_open && root->sel >= 0 && root->sel < root->count &&
            root->items[root->sel].kind == IT_SUB)
            selectedSub = root->items[root->sel].sub;
        it = NewItem(root, IT_SUB, title, NULL, NULL);
        if (it) {
            it->sub = h;
            it->rootPlugin = 1;
            InsertRootPlugin(root);
            if (selectedSub)
                for (i = 0; i < root->count; i++)
                    if (root->items[i].sub == selectedSub) {
                        root->sel = i;
                        break;
                    }
            ClampScroll(root);
        }
    }
    Unlock();
    ShSetError(h ? SH_OK : SH_ERR_NO_CANDIDATE);
    return h;
}

/* Drop the items but keep the row, so a plugin can rebuild
 * its own menu (a reload) without stacking duplicates. */
SH_API int ShMenuClear(uint32_t menu) {
    Menu *m;

    Lock();
    m = MenuOf(menu);
    if (m) {
        int i;
        for (i = 0; i < m->count; i++)
            if (m->items[i].kind == IT_SUB && m->items[i].sub)
                DropMenu(m->items[i].sub);
        m->count = 0;
        m->sel = 0;
    }
    Unlock();
    return m != NULL;
}

/* Remove the row itself, and its subtree with it. */
SH_API int ShMenuDestroy(uint32_t menu) {
    Menu *parent;
    int found = 0;

    if (menu == g_root || menu == g_settings) return 0;
    Lock();
    parent = MenuOf(MenuOf(menu) ? MenuOf(menu)->parent : 0);
    if (parent) {
        int i, w = 0;
        for (i = 0; i < parent->count; i++) {
            if (parent->items[i].kind == IT_SUB &&
                parent->items[i].sub == menu) {
                found = 1;
                continue;
            }
            if (w != i) parent->items[w] = parent->items[i];
            w++;
        }
        parent->count = w;
        ClampScroll(parent);
    }
    DropMenu(menu);
    Unlock();
    return found;
}

SH_API uint32_t ShMenuSub(uint32_t parent, const char *label) {
    uint32_t h;
    Item *it;
    Menu *m;

    EnsureMenu();
    Lock();
    m = MenuOf(parent);
    h = NewMenu(label, parent, m ? m->owner : "");
    if (h && m) {
        it = NewItem(m, IT_SUB, label, NULL, NULL);
        if (it) it->sub = h;
    }
    Unlock();
    return h;
}

SH_API int ShMenuAction(uint32_t menu, const char *label,
                        ShMenuFn fn, void *user) {
    Item *it;

    Lock();
    it = NewItem(MenuOf(menu), IT_ACTION, label, fn, user);
    Unlock();
    return it != NULL;
}

SH_API int ShMenuToggle(uint32_t menu, const char *label,
                        int initial, ShMenuFn fn, void *user) {
    Item *it;

    Lock();
    it = NewItem(MenuOf(menu), IT_TOGGLE, label, fn, user);
    if (it) it->value = initial ? 1 : 0;
    Unlock();
    return it != NULL;
}

SH_API int ShMenuNumber(uint32_t menu, const char *label,
                        float initial, float lo, float hi,
                        float step, ShMenuFn fn, void *user) {
    Item *it;

    Lock();
    it = NewItem(MenuOf(menu), IT_NUMBER, label, fn, user);
    if (it) {
        it->num = initial;
        it->lo = lo;
        it->hi = hi;
        it->step = step;
    }
    Unlock();
    return it != NULL;
}

/* The option strings are borrowed, so they must outlive the
 * menu. String literals are the intended case.
 */
SH_API int ShMenuList(uint32_t menu, const char *label,
                      const char **opts, int n, int initial,
                      ShMenuFn fn, void *user) {
    Item *it;
    int i;

    if (n > OPTS) n = OPTS;
    Lock();
    it = NewItem(MenuOf(menu), IT_LIST, label, fn, user);
    if (it) {
        for (i = 0; i < n; i++) it->opts[i] = opts[i];
        it->nopts = n;
        it->value = (n > 0) ? (initial % n) : 0;
    }
    Unlock();
    return it != NULL;
}

SH_API int ShMenuSetValue(uint32_t menu, const char *label, int value) {
    Menu *m;
    int i;
    Lock();
    m = MenuOf(menu);
    if (!m || !label) { Unlock(); ShSetError(SH_ERR_BAD_ARG); return 0; }
    for (i = 0; i < m->count; i++) {
        Item *it = &m->items[i];
        if (strcmp(it->label, label)) continue;
        if (it->kind == IT_TOGGLE) {
            it->value = value ? 1 : 0;
            Unlock(); ShSetError(SH_OK); return 1;
        }
        if (it->kind == IT_LIST && it->nopts > 0) {
            it->value = ((value % it->nopts) + it->nopts) % it->nopts;
            Unlock(); ShSetError(SH_OK); return 1;
        }
    }
    Unlock();
    ShSetError(SH_ERR_NO_CANDIDATE);
    return 0;
}

/* The line under the items, for whatever the last action
 * has to report. Empty text removes it.
 */
SH_API int ShMenuStatus(uint32_t menu, const char *text) {
    Menu *m;

    Lock();
    m = MenuOf(menu);
    if (!m) { Unlock(); ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (text) {
        strncpy(m->status, text, sizeof(m->status) - 1);
        m->status[sizeof(m->status) - 1] = 0;
    } else {
        m->status[0] = 0;
    }
    Unlock();
    return 1;
}

SH_API int ShMenuStatusF(uint32_t menu, const char *fmt, ...) {
    char english[512];
    char localized[512];
    char text[384];
    va_list ap;
    int written;
    Menu *m;

    if (!fmt) return 0;
    Lock();
    m = MenuOf(menu);
    if (!m) {
        Unlock();
        ShSetError(SH_ERR_BAD_ARG);
        return 0;
    }
    if (fmt[0] == '@') {
        if (!ShLangCopyEnglish(m->owner, fmt, english, sizeof(english))) {
            strncpy(english, fmt, sizeof(english) - 1);
            english[sizeof(english) - 1] = 0;
        }
        ShLangCopyMenu(m->owner, fmt, localized, sizeof(localized));
    }
    Unlock();

    va_start(ap, fmt);
    written = fmt[0] == '@'
        ? ShLangFormatMenuV(text, sizeof(text), english, localized, ap)
        : vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (written < 0) return 0;
    return ShMenuStatus(menu, text);
}

SH_API int ShMenuHint(uint32_t menu, const char *text) {
    Menu *m;

    Lock();
    m = MenuOf(menu);
    if (!m) { Unlock(); ShSetError(SH_ERR_BAD_ARG); return 0; }
    if (text) {
        strncpy(m->hint, text, sizeof(m->hint) - 1);
        m->hint[sizeof(m->hint) - 1] = 0;
    } else {
        m->hint[0] = 0;
    }
    Unlock();
    return 1;
}

SH_API void ShMenuSetKey(int vk) { g_key = vk; }
SH_API int  ShMenuIsOpen(void) { return g_open; }

SH_API int ShMenuIsShowing(uint32_t menu) {
    int hit;
    if (!menu) { ShSetError(SH_ERR_BAD_ARG); return 0; }
    Lock();
    if (!MenuOf(menu)) {
        Unlock(); ShSetError(SH_ERR_BAD_ARG); return 0;
    }
    hit = (g_open && g_current == menu) ? 1 : 0;
    Unlock();
    ShSetError(SH_OK);
    return hit;
}

SH_API void ShMenuOpen(int open) {
    EnsureMenu();
    Lock();
    g_open = open ? 1 : 0;
    if (g_open) g_current = g_root;
    if (g_open || InterlockedCompareExchange(&g_autoOpenPending, 0, 0))
        InterlockedExchange(&g_greeted, 1);
    InterlockedExchange(&g_autoOpenPending, 0);
    Unlock();
}

/* Latch Playing independently of plugin registration. The menu
 * thread consumes this only after the root and widgets are ready. */
void ShMenuOnStateChanged(int playing) {
    InterlockedExchange(&g_playing, playing ? 1 : 0);
    if (!playing) {
        InterlockedExchange(&g_autoOpenPending, 0);
        return;
    }
    if (!InterlockedCompareExchange(&g_greeted, 0, 0))
        InterlockedExchange(&g_autoOpenPending, 1);
}
