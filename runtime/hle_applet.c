/* Library applets - the system's own full-screen programs a game can call up: web pages
 * (the electronic manual, news), the software keyboard, the shop. The keyboard is a Windows
 * text box here; the others don't exist, so each behaves as if the player opened it and
 * immediately backed out. */
#define _CRT_SECURE_NO_WARNINGS
#include "host.h"
#include "gpu_backend.h"

void watchdog_pause(int on);   /* debug_watchdog.c */

#define R_CANCELED RESULT(162, 1)         /* "the user closed the applet" */
#define R_SWKBD_CANCELED RESULT(159, 1)   /* nn::swkbd's own: what ShowKeyboard returns for "back" */

/* Web pages: return at once, as if closed with the B button. */
HLE(_ZN2nn3web19ShowOfflineHtmlPageEPNS0_26OfflineHtmlPageReturnValueERKNS0_22ShowOfflineHtmlPageArgE) { c->x[0] = 0; }
HLE(_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE) { c->x[0] = 0; }
HLE(_ZN2nn3web13ShowLobbyPageEPNS0_20LobbyPageReturnValueERKNS0_16ShowLobbyPageArgE) { c->x[0] = 0; }
HLE(_ZN2nn3web16ShowYouTubeVideoEPNS0_18WebPageReturnValueERKNS0_19ShowYouTubeVideoArgE) { c->x[0] = 0; }

/* ---- the software keyboard: a text box --------------------------------------------- */
/* ShowKeyboard(String *out, const ShowKeyboardArg *arg). The argument starts with the
 * KeyboardConfig: header text (u16[65]) at 0x24, sub text (u16[129]) at 0xA6, guide text
 * (u16[257]) at 0x1A8, maximum and minimum length at 0x3AC/0x3B0, "new lines allowed" and
 * "answer in UTF-8" at 0x3BC/0x3BD; the initial text lies in the work buffer (pointer at
 * 0x4D0) at the offset and length (in characters) at 0x3C0/0x3C4. The answer goes to
 * out = {pointer, size in bytes}, NUL-terminated; 0 means OK, R_SWKBD_CANCELED backed out. */
enum { ID_TEXT = 100, ID_EDIT = 101 };

typedef struct Keyboard {
    WCHAR header[66], sub[130], guide[258], text[1024];
    int max_len, min_len, multiline;
} Keyboard;

static void guest_text(WCHAR *dst, size_t cap, u64 src, size_t max) {
    size_t n = 0;
    while (n + 1 < cap && n < max && ld16(src + 2 * n)) { dst[n] = (WCHAR)ld16(src + 2 * n); n++; }
    dst[n] = 0;
}

static INT_PTR CALLBACK keyboard_proc(HWND dlg, UINT msg, WPARAM w, LPARAM l) {
    Keyboard *k = (Keyboard *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        k = (Keyboard *)l;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)k);
        WCHAR prompt[600] = L"";   /* header, sub text, then the guide (the box's placeholder on the console) */
        if (k->header[0]) wcscat(prompt, k->header);
        if (k->sub[0]) { if (prompt[0]) wcscat(prompt, L"\n"); wcscat(prompt, k->sub); }
        if (k->guide[0]) {
            int bracket = prompt[0] != 0;
            if (bracket) wcscat(prompt, L"\n(");
            wcscat(prompt, k->guide);
            if (bracket) wcscat(prompt, L")");
        }
        SetDlgItemTextW(dlg, ID_TEXT, prompt[0] ? prompt : L"Enter text");
        SendDlgItemMessageW(dlg, ID_EDIT, EM_LIMITTEXT, (WPARAM)k->max_len, 0);
        SetDlgItemTextW(dlg, ID_EDIT, k->text);
        SendDlgItemMessageW(dlg, ID_EDIT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, ID_EDIT));
        return FALSE;   /* (focus set) */
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            GetDlgItemTextW(dlg, ID_EDIT, k->text, sizeof k->text / sizeof *k->text);
            if ((int)wcslen(k->text) < k->min_len) { MessageBeep(MB_ICONWARNING); return TRUE; }
            EndDialog(dlg, 1);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(dlg, 0); return TRUE; }
        break;
    }
    (void)l;
    return FALSE;
}

/* An in-memory dialog template: a prompt, a text box, OK and Cancel. */
static WORD *put_words(WORD *p, const WCHAR *s) { do *p++ = *s; while (*s++); return p; }
static WORD *put_item(WORD *p, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const WCHAR *text) {
    p = (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);   /* items are DWORD-aligned */
    DLGITEMTEMPLATE *it = (DLGITEMTEMPLATE *)p;
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    p = (WORD *)(it + 1);
    *p++ = 0xFFFF; *p++ = cls;   /* a predefined class: 0x80 button, 0x81 edit, 0x82 static */
    p = put_words(p, text);
    *p++ = 0;                    /* no creation data */
    return p;
}

static int ask_text(Keyboard *k) {
    static WORD buf[1024];
    memset(buf, 0, sizeof buf);
    DLGTEMPLATE *t = (DLGTEMPLATE *)buf;
    t->style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    t->cdit = 4;
    t->cx = 300; t->cy = k->multiline ? 140 : 92;
    WORD *p = (WORD *)(t + 1);
    *p++ = 0; *p++ = 0;                           /* no menu, the default class */
    p = put_words(p, L"Super Smash Bros. Ultimate - keyboard");
    *p++ = 10;                                    /* font size */
    p = put_words(p, L"Segoe UI");
    short edit_h = k->multiline ? 60 : 14, y = 44;
    p = put_item(p, SS_LEFT, 8, 6, 284, 34, ID_TEXT, 0x82, L"");
    p = put_item(p, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | (k->multiline ? ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN : 0),
                 8, y, 284, edit_h, ID_EDIT, 0x81, L"");
    y = (short)(y + edit_h + 10);
    p = put_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 186, y, 50, 14, IDOK, 0x80, L"OK");
    put_item(p, BS_PUSHBUTTON | WS_TABSTOP, 242, y, 50, 14, IDCANCEL, 0x80, L"Cancel");
    watchdog_pause(1);   /* (the game waits for the dialog: no frames meanwhile) */
    int ok = DialogBoxIndirectParamW(GetModuleHandleW(NULL), t, (HWND)gfx_window_handle(), keyboard_proc, (LPARAM)k) == 1;
    watchdog_pause(0);
    return ok;
}

HLE(_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE) {
    u64 out = c->x[0], arg = c->x[1];
    static Keyboard k;
    memset(&k, 0, sizeof k);
    guest_text(k.header, 66, arg + 0x24, 64);
    guest_text(k.sub, 130, arg + 0xA6, 128);
    guest_text(k.guide, 258, arg + 0x1A8, 256);
    k.max_len = (int)ld32(arg + 0x3AC);
    k.min_len = (int)ld32(arg + 0x3B0);
    if (k.max_len <= 0 || k.max_len > 1000) k.max_len = 1000;
    if (k.min_len < 0 || k.min_len > k.max_len) k.min_len = 0;
    k.multiline = ld8(arg + 0x3BC) != 0;
    int utf8 = ld8(arg + 0x3BD) != 0;
    u64 work = ld64(arg + 0x4D0);
    u32 offset = ld32(arg + 0x3C0), length = ld32(arg + 0x3C4);
    if (work && length) guest_text(k.text, 1024, work + offset, length < 1000 ? length : 1000);
    int ok = ask_text(&k);
    /* the text (also on Cancel: what the box held, as the console's keyboard reports it) */
    u64 ptr = out ? ld64(out) : 0, size = out ? ld64(out + 8) : 0;
    if (ptr && size >= (utf8 ? 1u : 2u)) {
        if (utf8) {
            char buf[4096];
            int n = WideCharToMultiByte(CP_UTF8, 0, k.text, -1, buf, sizeof buf, NULL, NULL);
            if (n <= 0) { buf[0] = 0; n = 1; }
            if ((u64)n > size) { n = (int)size; buf[n - 1] = 0; }
            memcpy(P(void, ptr), buf, (size_t)n);
        } else {
            size_t n = wcslen(k.text), room = (size_t)(size / 2) - 1;
            if (n > room) n = room;
            for (size_t i = 0; i < n; i++) st16(ptr + 2 * i, k.text[i]);
            st16(ptr + 2 * n, 0);
        }
    }
    c->x[0] = ok ? 0 : R_SWKBD_CANCELED;
}

/* The shop: unavailable offline. */
HLE(_ZN2nn2ec19ShowShopProductListEPKNS0_5NsUidEiPKcRKNS_7account10UserHandleE) { c->x[0] = R_CANCELED; }
HLE(_ZN2nn2ec22ShowShopProductDetailsENS0_5NsUidERKNS_7account10UserHandleE) { c->x[0] = R_CANCELED; }
HLE(_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdE) { c->x[0] = R_CANCELED; }
HLE(_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE) { c->x[0] = R_CANCELED; }

/* A friend's profile page. */
HLE(_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_) { c->x[0] = 0; }
