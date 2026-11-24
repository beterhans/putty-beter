/*
 * controls.c: routines to self-manage the controls in a dialog
 * box.
 */

/*
 * Possible TODO in new cross-platform config box stuff:
 *
 *  - When lining up two controls alongside each other, I wonder if
 *    we could conveniently arrange to centre them vertically?
 *    Particularly ugly in the current setup is the `Add new
 *    forwarded port:' static next to the rather taller `Remove'
 *    button.
 */

#include <assert.h>
#include <ctype.h>

#include "putty.h"
#include "misc.h"
#include "dialog.h"

#include <commctrl.h>

/*
 * PuTTY-beter: BCM_GETIDEALSIZE asks a button for the size it would like to
 * be, which is how button_force_left() finds a control's natural width. It
 * has been in commctrl.h for a long time, but define it if an old SDK is
 * missing it; the message is only honoured by comctl32 version 6, which the
 * manifest asks for.
 */
#ifndef BCM_GETIDEALSIZE
#define BCM_GETIDEALSIZE 0x1601
#endif

#define GAPBETWEEN 3
#define GAPWITHIN 1
#define GAPXBOX 7
#define GAPYBOX 4
#define DLGWIDTH 168
#define STATICHEIGHT 8
#define TITLEHEIGHT 12
#define CHECKBOXHEIGHT 8
#define RADIOHEIGHT 8
#define EDITHEIGHT 12
#define LISTHEIGHT 11
#define LISTINCREMENT 8
#define COMBOHEIGHT 12
#define PUSHBTNHEIGHT 14
#define PROGBARHEIGHT 14
/*
 * PuTTY-beter: how tall a multi-line edit box is, in text lines. The panel is
 * laid out to fit it, so this is what decides the comment box's size
 * (tech_detail.md section 7).
 */
#define MULTILINE_EDIT_LINES 6

DECL_WINDOWS_FUNCTION(static, void, InitCommonControls, (void));
DECL_WINDOWS_FUNCTION(static, BOOL, MakeDragList, (HWND));
DECL_WINDOWS_FUNCTION(static, int, LBItemFromPt, (HWND, POINT, BOOL));
DECL_WINDOWS_FUNCTION(static, void, DrawInsert, (HWND, HWND, int));
/*
 * PuTTY-beter: subclassing, which is how a list box gets drag-and-drop of
 * its own rows (see listbox_state_install() further down). comctl32 has
 * exported these since Windows XP, but this file loads every comctl32 entry
 * point dynamically, so do the same here rather than add an import that the
 * platform link line in cmake/platforms/windows.cmake does not carry.
 */
DECL_WINDOWS_FUNCTION(static, BOOL, SetWindowSubclass,
                      (HWND, SUBCLASSPROC, UINT_PTR, DWORD_PTR));
DECL_WINDOWS_FUNCTION(static, BOOL, RemoveWindowSubclass,
                      (HWND, SUBCLASSPROC, UINT_PTR));
DECL_WINDOWS_FUNCTION(static, LRESULT, DefSubclassProc,
                      (HWND, UINT, WPARAM, LPARAM));

void init_common_controls(void)
{
    HMODULE comctl32_module = load_system32_dll("comctl32.dll");
    GET_WINDOWS_FUNCTION(comctl32_module, InitCommonControls);
    GET_WINDOWS_FUNCTION(comctl32_module, MakeDragList);
    GET_WINDOWS_FUNCTION(comctl32_module, LBItemFromPt);
    GET_WINDOWS_FUNCTION(comctl32_module, DrawInsert);
    GET_WINDOWS_FUNCTION(comctl32_module, SetWindowSubclass);
    GET_WINDOWS_FUNCTION(comctl32_module, RemoveWindowSubclass);
    GET_WINDOWS_FUNCTION(comctl32_module, DefSubclassProc);
    p_InitCommonControls();
}

void ctlposinit(struct ctlpos *cp, HWND hwnd,
                int leftborder, int rightborder, int topborder)
{
    RECT r, r2;
    cp->hwnd = hwnd;
    cp->font = SendMessage(hwnd, WM_GETFONT, 0, 0);
    cp->ypos = topborder;
    GetClientRect(hwnd, &r);
    r2.left = r2.top = 0;
    r2.right = 4;
    r2.bottom = 8;
    MapDialogRect(hwnd, &r2);
    cp->dlu4inpix = r2.right;
    cp->width = (r.right * 4) / (r2.right) - 2 * GAPBETWEEN;
    cp->xoff = leftborder;
    cp->width -= leftborder + rightborder;
}

HWND doctl(struct ctlpos *cp, RECT r, const char *wclass, int wstyle,
           int exstyle, const char *wtext, int wid)
{
    HWND ctl;
    /*
     * Note nonstandard use of RECT. This is deliberate: by
     * transforming the width and height directly we arrange to
     * have all supposedly same-sized controls really same-sized.
     */

    r.left += cp->xoff;
    MapDialogRect(cp->hwnd, &r);

    /*
     * We can pass in cp->hwnd == NULL, to indicate a dry run
     * without creating any actual controls.
     */
    if (cp->hwnd) {
        ctl = CreateWindowEx(exstyle, wclass, wtext, wstyle,
                             r.left, r.top, r.right, r.bottom,
                             cp->hwnd, (HMENU)(ULONG_PTR)wid, hinst, NULL);
        SendMessage(ctl, WM_SETFONT, cp->font, MAKELPARAM(true, 0));

        if (!strcmp(wclass, "LISTBOX")) {
            /*
             * Bizarre Windows bug: the list box calculates its
             * number of lines based on the font it has at creation
             * time, but sending it WM_SETFONT doesn't cause it to
             * recalculate. So now, _after_ we've sent it
             * WM_SETFONT, we explicitly resize it (to the same
             * size it was already!) to force it to reconsider.
             */
            SetWindowPos(ctl, NULL, 0, 0, r.right, r.bottom,
                         SWP_NOACTIVATE | SWP_NOCOPYBITS |
                         SWP_NOMOVE | SWP_NOZORDER);
        }
    } else
        ctl = NULL;
    return ctl;
}

/*
 * A title bar across the top of a sub-dialog.
 */
void bartitle(struct ctlpos *cp, const char *name, int id)
{
    RECT r;

    r.left = GAPBETWEEN;
    r.right = cp->width;
    r.top = cp->ypos;
    r.bottom = STATICHEIGHT;
    cp->ypos += r.bottom + GAPBETWEEN;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, name, id);
}

/*
 * Begin a grouping box, with or without a group title.
 */
void beginbox(struct ctlpos *cp, const char *name, int idbox)
{
    cp->boxystart = cp->ypos;
    if (!name)
        cp->boxystart -= STATICHEIGHT / 2;
    if (name)
        cp->ypos += STATICHEIGHT;
    cp->ypos += GAPYBOX;
    cp->width -= 2 * GAPXBOX;
    cp->xoff += GAPXBOX;
    cp->boxid = idbox;
    cp->boxtext = name;
}

/*
 * End a grouping box.
 */
void endbox(struct ctlpos *cp)
{
    RECT r;
    cp->xoff -= GAPXBOX;
    cp->width += 2 * GAPXBOX;
    cp->ypos += GAPYBOX - GAPBETWEEN;
    r.left = GAPBETWEEN;
    r.right = cp->width;
    r.top = cp->boxystart;
    r.bottom = cp->ypos - cp->boxystart;
    doctl(cp, r, "BUTTON", BS_GROUPBOX | WS_CHILD | WS_VISIBLE, 0,
          cp->boxtext ? cp->boxtext : "", cp->boxid);
    cp->ypos += GAPYBOX;
}

/*
 * A static line, followed by a full-width edit box.
 */
void editboxfw(struct ctlpos *cp, bool password, bool readonly,
               const char *text, int staticid, int editid)
{
    RECT r;

    r.left = GAPBETWEEN;
    r.right = cp->width;

    if (text) {
        r.top = cp->ypos;
        r.bottom = STATICHEIGHT;
        doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, text, staticid);
        cp->ypos += STATICHEIGHT + GAPWITHIN;
    }
    r.top = cp->ypos;
    r.bottom = EDITHEIGHT;
    doctl(cp, r, "EDIT",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
          (password ? ES_PASSWORD : 0) |
          (readonly ? ES_READONLY : 0),
          WS_EX_CLIENTEDGE, "", editid);
    cp->ypos += EDITHEIGHT + GAPBETWEEN;
}

/*
 * A static line, followed by a full-width combo box.
 */
void combobox(struct ctlpos *cp, const char *text, int staticid, int listid)
{
    RECT r;

    r.left = GAPBETWEEN;
    r.right = cp->width;

    if (text) {
        r.top = cp->ypos;
        r.bottom = STATICHEIGHT;
        doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, text, staticid);
        cp->ypos += STATICHEIGHT + GAPWITHIN;
    }
    r.top = cp->ypos;
    r.bottom = COMBOHEIGHT * 10;
    doctl(cp, r, "COMBOBOX",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
          CBS_DROPDOWN | CBS_HASSTRINGS, WS_EX_CLIENTEDGE, "", listid);
    cp->ypos += COMBOHEIGHT + GAPBETWEEN;
}

struct radio { const char *text; int id; };

static void radioline_common(struct ctlpos *cp, const char *text, int id,
                             int nacross, struct radio *buttons, int nbuttons)
{
    RECT r;
    int group;
    int i;
    int j;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    if (text) {
        r.right = cp->width;
        r.bottom = STATICHEIGHT;
        cp->ypos += r.bottom + GAPWITHIN;
        doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, text, id);
    } else {
        r.right = r.bottom = 0;
    }

    group = WS_GROUP;
    i = 0;
    for (j = 0; j < nbuttons; j++) {
        const char *btext = buttons[j].text;
        int bid = buttons[j].id;

        if (i == nacross) {
            cp->ypos += r.bottom + (nacross > 1 ? GAPBETWEEN : GAPWITHIN);
            i = 0;
        }
        r.left = GAPBETWEEN + i * (cp->width + GAPBETWEEN) / nacross;
        if (j < nbuttons-1)
            r.right =
                (i + 1) * (cp->width + GAPBETWEEN) / nacross - r.left;
        else
            r.right = cp->width - r.left;
        r.top = cp->ypos;
        r.bottom = RADIOHEIGHT;
        doctl(cp, r, "BUTTON",
              BS_NOTIFY | BS_AUTORADIOBUTTON | WS_CHILD |
              WS_VISIBLE | WS_TABSTOP | group, 0, btext, bid);
        group = 0;
        i++;
    }
    cp->ypos += r.bottom + GAPBETWEEN;
}

/*
 * A set of radio buttons on the same line, with a static above
 * them. `nacross' dictates how many parts the line is divided into
 * (you might want this not to equal the number of buttons if you
 * needed to line up some 2s and some 3s to look good in the same
 * panel).
 *
 * There's a bit of a hack in here to ensure that if nacross
 * exceeds the actual number of buttons, the rightmost button
 * really does get all the space right to the edge of the line, so
 * you can do things like
 *
 * (*) Button1  (*) Button2  (*) ButtonWithReallyLongTitle
 */
void radioline(struct ctlpos *cp, const char *text, int id, int nacross, ...)
{
    va_list ap;
    struct radio *buttons;
    int i, nbuttons;

    va_start(ap, nacross);
    nbuttons = 0;
    while (1) {
        const char *btext = va_arg(ap, const char *);
        if (!btext)
            break;
        (void) va_arg(ap, int); /* id */
        nbuttons++;
    }
    va_end(ap);
    buttons = snewn(nbuttons, struct radio);
    va_start(ap, nacross);
    for (i = 0; i < nbuttons; i++) {
        buttons[i].text = va_arg(ap, const char *);
        buttons[i].id = va_arg(ap, int);
    }
    va_end(ap);
    radioline_common(cp, text, id, nacross, buttons, nbuttons);
    sfree(buttons);
}

/*
 * A set of radio buttons on the same line, without a static above
 * them. Otherwise just like radioline.
 */
void bareradioline(struct ctlpos *cp, int nacross, ...)
{
    va_list ap;
    struct radio *buttons;
    int i, nbuttons;

    va_start(ap, nacross);
    nbuttons = 0;
    while (1) {
        const char *btext = va_arg(ap, const char *);
        if (!btext)
            break;
        (void) va_arg(ap, int); /* id */
        nbuttons++;
    }
    va_end(ap);
    buttons = snewn(nbuttons, struct radio);
    va_start(ap, nacross);
    for (i = 0; i < nbuttons; i++) {
        buttons[i].text = va_arg(ap, const char *);
        buttons[i].id = va_arg(ap, int);
    }
    va_end(ap);
    radioline_common(cp, NULL, 0, nacross, buttons, nbuttons);
    sfree(buttons);
}

/*
 * A set of radio buttons on multiple lines, with a static above
 * them.
 */
void radiobig(struct ctlpos *cp, const char *text, int id, ...)
{
    va_list ap;
    struct radio *buttons;
    int i, nbuttons;

    va_start(ap, id);
    nbuttons = 0;
    while (1) {
        const char *btext = va_arg(ap, const char *);
        if (!btext)
            break;
        (void) va_arg(ap, int); /* id */
        nbuttons++;
    }
    va_end(ap);
    buttons = snewn(nbuttons, struct radio);
    va_start(ap, id);
    for (i = 0; i < nbuttons; i++) {
        buttons[i].text = va_arg(ap, const char *);
        buttons[i].id = va_arg(ap, int);
    }
    va_end(ap);
    radioline_common(cp, text, id, 1, buttons, nbuttons);
    sfree(buttons);
}

/*
 * A single standalone checkbox.
 */
void checkbox(struct ctlpos *cp, const char *text, int id)
{
    RECT r;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = CHECKBOXHEIGHT;
    cp->ypos += r.bottom + GAPBETWEEN;
    doctl(cp, r, "BUTTON",
          BS_NOTIFY | BS_AUTOCHECKBOX | WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0,
          text, id);
}

/*
 * Wrap a piece of text for a static text control. Returns the
 * wrapped text (a malloc'ed string containing \ns), and also
 * returns the number of lines required.
 */
char *staticwrap(struct ctlpos *cp, HWND hwnd, const char *text, int *lines)
{
    HDC hdc = GetDC(hwnd);
    int width, nlines, j;
    INT *pwidths, nfit;
    SIZE size;
    const char *p;
    RECT r;
    HFONT oldfont, newfont;

    strbuf *sb = strbuf_new();
    p = text;
    pwidths = snewn(1+strlen(text), INT);

    /*
     * Work out the width the text will need to fit in, by doing
     * the same adjustment that the `statictext' function itself
     * will perform.
     */
    SetMapMode(hdc, MM_TEXT);          /* ensure logical units == pixels */
    r.left = r.top = r.bottom = 0;
    r.right = cp->width;
    MapDialogRect(hwnd, &r);
    width = r.right;

    nlines = 1;

    /*
     * We must select the correct font into the HDC before calling
     * GetTextExtent*, or silly things will happen.
     */
    newfont = (HFONT)SendMessage(hwnd, WM_GETFONT, 0, 0);
    oldfont = SelectObject(hdc, newfont);

    while (*p) {
        if (!GetTextExtentExPoint(hdc, p, strlen(p), width,
                                  &nfit, pwidths, &size) ||
            (size_t)nfit >= strlen(p)) {
            /*
             * Either GetTextExtentExPoint returned failure, or the
             * whole of the rest of the text fits on this line.
             * Either way, we stop wrapping, copy the remainder of
             * the input string unchanged to the output, and leave.
             */
            put_datapl(sb, ptrlen_from_asciz(p));
            break;
        }

        /*
         * Now we search backwards along the string from `nfit',
         * looking for a space at which to break the line. If we
         * don't find one at all, that's fine - we'll just break
         * the line at `nfit'.
         */
        for (j = nfit; j > 0; j--) {
            if (isspace((unsigned char)p[j])) {
                nfit = j;
                break;
            }
        }

        put_data(sb, p, nfit);
        put_byte(sb, '\n');

        p += nfit;
        while (*p && isspace((unsigned char)*p))
            p++;

        nlines++;
    }

    SelectObject(hdc, oldfont);
    ReleaseDC(cp->hwnd, hdc);

    if (lines) *lines = nlines;

    sfree(pwidths);

    return strbuf_to_str(sb);
}

/*
 * A single standalone static text control.
 */
void statictext(struct ctlpos *cp, const char *text, int lines, int id)
{
    RECT r;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = STATICHEIGHT * lines;
    cp->ypos += r.bottom + GAPBETWEEN;
    doctl(cp, r, "STATIC",
          WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
          0, text, id);
}

/*
 * An owner-drawn static text control for a panel title.
 */
void paneltitle(struct ctlpos *cp, int id)
{
    RECT r;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = TITLEHEIGHT;
    cp->ypos += r.bottom + GAPBETWEEN;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
          0, NULL, id);
}

/*
 * A button on the right hand side, with a static to its left.
 */
void staticbtn(struct ctlpos *cp, const char *stext, int sid,
               const char *btext, int bid)
{
    const int height = (PUSHBTNHEIGHT > STATICHEIGHT ?
                        PUSHBTNHEIGHT : STATICHEIGHT);
    RECT r;
    int lwid, rwid, rpos;

    rpos = GAPBETWEEN + 3 * (cp->width + GAPBETWEEN) / 4;
    lwid = rpos - 2 * GAPBETWEEN;
    rwid = cp->width + GAPBETWEEN - rpos;

    r.left = GAPBETWEEN;
    r.top = cp->ypos + (height - STATICHEIGHT) / 2;
    r.right = lwid;
    r.bottom = STATICHEIGHT;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);

    r.left = rpos;
    r.top = cp->ypos + (height - PUSHBTNHEIGHT) / 2;
    r.right = rwid;
    r.bottom = PUSHBTNHEIGHT;
    doctl(cp, r, "BUTTON",
          BS_NOTIFY | WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0, btext, bid);

    cp->ypos += height + GAPBETWEEN;
}

/*
 * A simple push button.
 */
HWND button(struct ctlpos *cp, const char *btext, int bid, bool defbtn)
{
    RECT r;
    HWND h;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = PUSHBTNHEIGHT;

    /* Q67655: the _dialog box_ must know which button is default
     * as well as the button itself knowing */
    if (defbtn && cp->hwnd)
        SendMessage(cp->hwnd, DM_SETDEFID, bid, 0);

    h = doctl(cp, r, "BUTTON",
              BS_NOTIFY | WS_CHILD | WS_VISIBLE | WS_TABSTOP |
              (defbtn ? BS_DEFPUSHBUTTON : 0) | BS_PUSHBUTTON,
              0, btext, bid);

    cp->ypos += PUSHBTNHEIGHT + GAPBETWEEN;
    return h;
}

/*
 * PuTTY-beter: like button(), but the control keeps its natural width and is
 * left-aligned in its column rather than stretched to fill it. This is what
 * the shared code's `force_left' flag asks for (dialog.h); GTK does the same
 * thing with columns_force_left_align(), and it stops a row of buttons from
 * growing into wide bars when the configuration box is resized.
 *
 * The natural width is whatever comctl32 reports for the label, via
 * BCM_GETIDEALSIZE. That needs the button to exist first, so the button is
 * created at the column width and then shrunk. The column width is the
 * ceiling, so a button can never overrun its column and cover the panel
 * border; if comctl32 does not answer (no version 6), the column width
 * stands, which is the old behaviour.
 */
HWND button_force_left(struct ctlpos *cp, const char *btext, int bid,
                       bool defbtn)
{
    HWND h = button(cp, btext, bid, defbtn);
    RECT r;
    SIZE sz;

    if (!h)
        return h;

    sz.cx = sz.cy = 0;
    if (!SendMessage(h, BCM_GETIDEALSIZE, 0, (LPARAM)&sz) || sz.cx <= 0)
        return h;

    GetWindowRect(h, &r);
    if (sz.cx < r.right - r.left)
        SetWindowPos(h, NULL, 0, 0, sz.cx, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    return h;
}

/*
 * Like staticbtn, but two buttons.
 */
void static2btn(struct ctlpos *cp, const char *stext, int sid,
                const char *btext1, int bid1, const char *btext2, int bid2)
{
    const int height = (PUSHBTNHEIGHT > STATICHEIGHT ?
                        PUSHBTNHEIGHT : STATICHEIGHT);
    RECT r;
    int lwid, rwid1, rwid2, rpos1, rpos2;

    rpos1 = GAPBETWEEN + (cp->width + GAPBETWEEN) / 2;
    rpos2 = GAPBETWEEN + 3 * (cp->width + GAPBETWEEN) / 4;
    lwid = rpos1 - 2 * GAPBETWEEN;
    rwid1 = rpos2 - rpos1 - GAPBETWEEN;
    rwid2 = cp->width + GAPBETWEEN - rpos2;

    r.left = GAPBETWEEN;
    r.top = cp->ypos + (height - STATICHEIGHT) / 2;
    r.right = lwid;
    r.bottom = STATICHEIGHT;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);

    r.left = rpos1;
    r.top = cp->ypos + (height - PUSHBTNHEIGHT) / 2;
    r.right = rwid1;
    r.bottom = PUSHBTNHEIGHT;
    doctl(cp, r, "BUTTON",
          BS_NOTIFY | WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0, btext1, bid1);

    r.left = rpos2;
    r.top = cp->ypos + (height - PUSHBTNHEIGHT) / 2;
    r.right = rwid2;
    r.bottom = PUSHBTNHEIGHT;
    doctl(cp, r, "BUTTON",
          BS_NOTIFY | WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0, btext2, bid2);

    cp->ypos += height + GAPBETWEEN;
}

/*
 * An edit control on the right hand side, with a static to its left.
 */
static void staticedit_internal(struct ctlpos *cp, const char *stext,
                                int sid, int eid, int percentedit,
                                int style)
{
    const int height = (EDITHEIGHT > STATICHEIGHT ?
                        EDITHEIGHT : STATICHEIGHT);
    RECT r;
    int lwid, rwid, rpos;

    rpos =
        GAPBETWEEN + (100 - percentedit) * (cp->width + GAPBETWEEN) / 100;
    lwid = rpos - 2 * GAPBETWEEN;
    rwid = cp->width + GAPBETWEEN - rpos;

    r.left = GAPBETWEEN;
    r.top = cp->ypos + (height - STATICHEIGHT) / 2;
    r.right = lwid;
    r.bottom = STATICHEIGHT;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);

    r.left = rpos;
    r.top = cp->ypos + (height - EDITHEIGHT) / 2;
    r.right = rwid;
    r.bottom = EDITHEIGHT;
    doctl(cp, r, "EDIT",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | style,
          WS_EX_CLIENTEDGE, "", eid);

    cp->ypos += height + GAPBETWEEN;
}

void staticedit(struct ctlpos *cp, const char *stext,
                int sid, int eid, int percentedit)
{
    staticedit_internal(cp, stext, sid, eid, percentedit, 0);
}

void staticpassedit(struct ctlpos *cp, const char *stext,
                    int sid, int eid, int percentedit)
{
    staticedit_internal(cp, stext, sid, eid, percentedit, ES_PASSWORD);
}

/*
 * A drop-down list box on the right hand side, with a static to
 * its left.
 */
void staticddl(struct ctlpos *cp, const char *stext,
               int sid, int lid, int percentlist)
{
    const int height = (COMBOHEIGHT > STATICHEIGHT ?
                        COMBOHEIGHT : STATICHEIGHT);
    RECT r;
    int lwid, rwid, rpos;

    rpos =
        GAPBETWEEN + (100 - percentlist) * (cp->width + GAPBETWEEN) / 100;
    lwid = rpos - 2 * GAPBETWEEN;
    rwid = cp->width + GAPBETWEEN - rpos;

    r.left = GAPBETWEEN;
    r.top = cp->ypos + (height - STATICHEIGHT) / 2;
    r.right = lwid;
    r.bottom = STATICHEIGHT;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);

    r.left = rpos;
    r.top = cp->ypos + (height - EDITHEIGHT) / 2;
    r.right = rwid;
    r.bottom = COMBOHEIGHT*4;
    doctl(cp, r, "COMBOBOX",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
          CBS_DROPDOWNLIST | CBS_HASSTRINGS, WS_EX_CLIENTEDGE, "", lid);

    cp->ypos += height + GAPBETWEEN;
}

/*
 * A combo box on the right hand side, with a static to its left.
 */
void staticcombo(struct ctlpos *cp, const char *stext,
                 int sid, int lid, int percentlist)
{
    const int height = (COMBOHEIGHT > STATICHEIGHT ?
                        COMBOHEIGHT : STATICHEIGHT);
    RECT r;
    int lwid, rwid, rpos;

    rpos =
        GAPBETWEEN + (100 - percentlist) * (cp->width + GAPBETWEEN) / 100;
    lwid = rpos - 2 * GAPBETWEEN;
    rwid = cp->width + GAPBETWEEN - rpos;

    r.left = GAPBETWEEN;
    r.top = cp->ypos + (height - STATICHEIGHT) / 2;
    r.right = lwid;
    r.bottom = STATICHEIGHT;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);

    r.left = rpos;
    r.top = cp->ypos + (height - EDITHEIGHT) / 2;
    r.right = rwid;
    r.bottom = COMBOHEIGHT*10;
    doctl(cp, r, "COMBOBOX",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
          CBS_DROPDOWN | CBS_HASSTRINGS, WS_EX_CLIENTEDGE, "", lid);

    cp->ypos += height + GAPBETWEEN;
}

/*
 * A static, with a full-width drop-down list box below it.
 */
void staticddlbig(struct ctlpos *cp, const char *stext,
                  int sid, int lid)
{
    RECT r;

    if (stext) {
        r.left = GAPBETWEEN;
        r.top = cp->ypos;
        r.right = cp->width;
        r.bottom = STATICHEIGHT;
        doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);
        cp->ypos += STATICHEIGHT;
    }

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = COMBOHEIGHT*4;
    doctl(cp, r, "COMBOBOX",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
          CBS_DROPDOWNLIST | CBS_HASSTRINGS, WS_EX_CLIENTEDGE, "", lid);
    cp->ypos += COMBOHEIGHT + GAPBETWEEN;
}

/*
 * A big multiline edit control with a static labelling it.
 */
void bigeditctrl(struct ctlpos *cp, const char *stext,
                 int sid, int eid, int lines)
{
    RECT r;

    if (stext) {
        r.left = GAPBETWEEN;
        r.top = cp->ypos;
        r.right = cp->width;
        r.bottom = STATICHEIGHT;
        cp->ypos += r.bottom + GAPWITHIN;
        doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);
    }

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = EDITHEIGHT + (lines - 1) * STATICHEIGHT;
    cp->ypos += r.bottom + GAPBETWEEN;
    /*
     * PuTTY-beter: ES_WANTRETURN so that Return puts a line break in the box
     * rather than reaching the dialog and firing its default button, and
     * ES_AUTOVSCROLL so a long block scrolls itself.
     */
    doctl(cp, r, "EDIT",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE |
          ES_WANTRETURN | ES_AUTOVSCROLL,
          WS_EX_CLIENTEDGE, "", eid);
}

/*
 * A list box with a static labelling it.
 */
void listbox(struct ctlpos *cp, const char *stext,
             int sid, int lid, int lines, bool multi)
{
    RECT r;

    if (stext != NULL) {
        r.left = GAPBETWEEN;
        r.top = cp->ypos;
        r.right = cp->width;
        r.bottom = STATICHEIGHT;
        cp->ypos += r.bottom + GAPWITHIN;
        doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);
    }

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = LISTHEIGHT + (lines - 1) * LISTINCREMENT;
    cp->ypos += r.bottom + GAPBETWEEN;
    /*
     * PuTTY-beter: a multi-selection list box uses LBS_EXTENDEDSEL, not
     * LBS_MULTIPLESEL. LBS_MULTIPLESEL means "every click toggles the row
     * under the mouse", so a plain click on the saved-session list selects
     * without clearing the previous row - which reads as if Ctrl or Shift
     * were being held down when they are not. LBS_EXTENDEDSEL is the style
     * that behaves like the GTK side's GTK_SELECTION_MULTIPLE, which is
     * what the shared configuration code was written against: a plain click
     * selects one row, Ctrl-click toggles a row, Shift-click extends.
     *
     * (Upstream used LBS_MULTIPLESEL here for the one multi-selection list
     * box it had, the character-class panel. That panel now needs Ctrl to
     * pick several codes, which is how it already works on macOS.)
     */
    /* (multi ? LBS_MULTIPLESEL : 0), */
    doctl(cp, r, "LISTBOX",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
          LBS_NOTIFY | LBS_HASSTRINGS | LBS_USETABSTOPS |
          (multi ? LBS_EXTENDEDSEL : 0),
          WS_EX_CLIENTEDGE, "", lid);
}

/*
 * A tab-control substitute when a real tab control is unavailable.
 */
void ersatztab(struct ctlpos *cp, const char *stext, int sid, int lid,
               int s2id)
{
    const int height = (COMBOHEIGHT > STATICHEIGHT ?
                        COMBOHEIGHT : STATICHEIGHT);
    RECT r;
    int bigwid, lwid, rwid, rpos;
    static const int BIGGAP = 15;
    static const int MEDGAP = 3;

    bigwid = cp->width + 2 * GAPBETWEEN - 2 * BIGGAP;
    cp->ypos += MEDGAP;
    rpos = BIGGAP + (bigwid + BIGGAP) / 2;
    lwid = rpos - 2 * BIGGAP;
    rwid = bigwid + BIGGAP - rpos;

    r.left = BIGGAP;
    r.top = cp->ypos + (height - STATICHEIGHT) / 2;
    r.right = lwid;
    r.bottom = STATICHEIGHT;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);

    r.left = rpos;
    r.top = cp->ypos + (height - COMBOHEIGHT) / 2;
    r.right = rwid;
    r.bottom = COMBOHEIGHT * 10;
    doctl(cp, r, "COMBOBOX",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP |
          CBS_DROPDOWNLIST | CBS_HASSTRINGS, WS_EX_CLIENTEDGE, "", lid);

    cp->ypos += height + MEDGAP + GAPBETWEEN;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = 2;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
          0, "", s2id);
}

/*
 * A static line, followed by an edit control on the left hand side
 * and a button on the right.
 */
void editbutton(struct ctlpos *cp, const char *stext, int sid,
                int eid, const char *btext, int bid)
{
    const int height = (EDITHEIGHT > PUSHBTNHEIGHT ?
                        EDITHEIGHT : PUSHBTNHEIGHT);
    RECT r;
    int lwid, rwid, rpos;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = STATICHEIGHT;
    cp->ypos += r.bottom + GAPWITHIN;
    doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);

    rpos = GAPBETWEEN + 3 * (cp->width + GAPBETWEEN) / 4;
    lwid = rpos - 2 * GAPBETWEEN;
    rwid = cp->width + GAPBETWEEN - rpos;

    r.left = GAPBETWEEN;
    r.top = cp->ypos + (height - EDITHEIGHT) / 2;
    r.right = lwid;
    r.bottom = EDITHEIGHT;
    doctl(cp, r, "EDIT",
          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
          WS_EX_CLIENTEDGE, "", eid);

    r.left = rpos;
    r.top = cp->ypos + (height - PUSHBTNHEIGHT) / 2;
    r.right = rwid;
    r.bottom = PUSHBTNHEIGHT;
    doctl(cp, r, "BUTTON",
          BS_NOTIFY | WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
          0, btext, bid);

    cp->ypos += height + GAPBETWEEN;
}

/*
 * A special control for manipulating an ordered preference list
 * (eg. for cipher selection).
 * XXX: this is a rough hack and could be improved.
 */
void prefslist(struct prefslist *hdl, struct ctlpos *cp, int lines,
               const char *stext, int sid, int listid, int upbid, int dnbid)
{
    const static int percents[] = { 5, 75, 20 };
    RECT r;
    int xpos, percent = 0, i;
    int listheight = LISTHEIGHT + (lines - 1) * LISTINCREMENT;
    const int BTNSHEIGHT = 2*PUSHBTNHEIGHT + GAPBETWEEN;
    int totalheight, buttonpos;

    /* Squirrel away IDs. */
    hdl->listid = listid;
    hdl->upbid  = upbid;
    hdl->dnbid  = dnbid;

    /* The static label. */
    if (stext != NULL) {
        r.left = GAPBETWEEN;
        r.top = cp->ypos;
        r.right = cp->width;
        r.bottom = STATICHEIGHT;
        cp->ypos += r.bottom + GAPWITHIN;
        doctl(cp, r, "STATIC", WS_CHILD | WS_VISIBLE, 0, stext, sid);
    }

    if (listheight > BTNSHEIGHT) {
        totalheight = listheight;
        buttonpos = (listheight - BTNSHEIGHT) / 2;
    } else {
        totalheight = BTNSHEIGHT;
        buttonpos = 0;
    }

    for (i=0; i<3; i++) {
        int left, wid;
        xpos = (cp->width + GAPBETWEEN) * percent / 100;
        left = xpos + GAPBETWEEN;
        percent += percents[i];
        xpos = (cp->width + GAPBETWEEN) * percent / 100;
        wid = xpos - left;

        switch (i) {
          case 1: {
            /* The drag list box. */
            r.left = left; r.right = wid;
            r.top = cp->ypos; r.bottom = listheight;
            HWND ctl = doctl(cp, r, "LISTBOX",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                             WS_VSCROLL | LBS_HASSTRINGS | LBS_USETABSTOPS,
                             WS_EX_CLIENTEDGE,
                             "", listid);
            p_MakeDragList(ctl);
            break;
          }

          case 2:
            /* The "Up" and "Down" buttons. */
            /* XXX worry about accelerators if we have more than one
             * prefslist on a panel */
            r.left = left; r.right = wid;
            r.top = cp->ypos + buttonpos; r.bottom = PUSHBTNHEIGHT;
            doctl(cp, r, "BUTTON",
                  BS_NOTIFY | WS_CHILD | WS_VISIBLE |
                  WS_TABSTOP | BS_PUSHBUTTON,
                  0, "&Up", upbid);

            r.left = left; r.right = wid;
            r.top = cp->ypos + buttonpos + PUSHBTNHEIGHT + GAPBETWEEN;
            r.bottom = PUSHBTNHEIGHT;
            doctl(cp, r, "BUTTON",
                  BS_NOTIFY | WS_CHILD | WS_VISIBLE |
                  WS_TABSTOP | BS_PUSHBUTTON,
                  0, "&Down", dnbid);

            break;

        }
    }

    cp->ypos += totalheight + GAPBETWEEN;

}

/*
 * Helper function for prefslist: move item in list box.
 */
static void pl_moveitem(HWND hwnd, int listid, int src, int dst)
{
    int tlen, val;
    char *txt;
    /* Get the item's data. */
    tlen = SendDlgItemMessage (hwnd, listid, LB_GETTEXTLEN, src, 0);
    txt = snewn(tlen+1, char);
    SendDlgItemMessage (hwnd, listid, LB_GETTEXT, src, (LPARAM) txt);
    val = SendDlgItemMessage (hwnd, listid, LB_GETITEMDATA, src, 0);
    /* Deselect old location. */
    SendDlgItemMessage (hwnd, listid, LB_SETSEL, false, src);
    /* Delete it at the old location. */
    SendDlgItemMessage (hwnd, listid, LB_DELETESTRING, src, 0);
    /* Insert it at new location. */
    SendDlgItemMessage (hwnd, listid, LB_INSERTSTRING, dst,
                        (LPARAM) txt);
    SendDlgItemMessage (hwnd, listid, LB_SETITEMDATA, dst,
                        (LPARAM) val);
    /* Set selection. */
    SendDlgItemMessage (hwnd, listid, LB_SETCURSEL, dst, 0);
    sfree (txt);
}

int pl_itemfrompt(HWND hwnd, POINT cursor, bool scroll)
{
    int ret;
    POINT uppoint, downpoint;
    int updist, downdist, upitem, downitem, i;

    /*
     * Ghastly hackery to try to figure out not which
     * _item_, but which _gap between items_, the user
     * is pointing at. We do this by first working out
     * which list item is under the cursor, and then
     * working out how far the cursor would have to
     * move up or down before the answer was different.
     * Then we put the insertion point _above_ the
     * current item if the upper edge is closer than
     * the lower edge, or _below_ it if vice versa.
     */
    ret = p_LBItemFromPt(hwnd, cursor, scroll);
    if (ret == -1)
        return ret;
    ret = p_LBItemFromPt(hwnd, cursor, false);
    updist = downdist = 0;
    for (i = 1; i < 4096 && (!updist || !downdist); i++) {
        uppoint = downpoint = cursor;
        uppoint.y -= i;
        downpoint.y += i;
        upitem = p_LBItemFromPt(hwnd, uppoint, false);
        downitem = p_LBItemFromPt(hwnd, downpoint, false);
        if (!updist && upitem != ret)
            updist = i;
        if (!downdist && downitem != ret)
            downdist = i;
    }
    if (downdist < updist)
        ret++;
    return ret;
}

/*
 * Handler for prefslist above.
 *
 * Return value has bit 0 set if the dialog box procedure needs to
 * return true from handling this message; it has bit 1 set if a
 * change may have been made in the contents of the list.
 */
int handle_prefslist(struct prefslist *hdl,
                     int *array, int maxmemb,
                     bool is_dlmsg, HWND hwnd,
                     WPARAM wParam, LPARAM lParam)
{
    int i;
    int ret = 0;

    if (is_dlmsg) {

        if ((int)wParam == hdl->listid) {
            DRAGLISTINFO *dlm = (DRAGLISTINFO *)lParam;
            int dest = 0;              /* initialise to placate gcc */
            switch (dlm->uNotification) {
              case DL_BEGINDRAG:
                /* Add a dummy item to make pl_itemfrompt() work
                 * better.
                 * FIXME: this causes scrollbar glitches if the count of
                 *        listbox contains >= its height. */
                hdl->dummyitem =
                    SendDlgItemMessage(hwnd, hdl->listid,
                                       LB_ADDSTRING, 0, (LPARAM) "");

                hdl->srcitem = p_LBItemFromPt(dlm->hWnd, dlm->ptCursor, true);
                hdl->dragging = false;
                /* XXX hack Q183115 */
                SetWindowLongPtr(hwnd, DWLP_MSGRESULT, true);
                ret |= 1; break;
              case DL_CANCELDRAG:
                p_DrawInsert(hwnd, dlm->hWnd, -1);     /* Clear arrow */
                SendDlgItemMessage(hwnd, hdl->listid,
                                   LB_DELETESTRING, hdl->dummyitem, 0);
                hdl->dragging = false;
                ret |= 1; break;
              case DL_DRAGGING:
                hdl->dragging = true;
                dest = pl_itemfrompt(dlm->hWnd, dlm->ptCursor, true);
                if (dest > hdl->dummyitem) dest = hdl->dummyitem;
                p_DrawInsert (hwnd, dlm->hWnd, dest);
                if (dest >= 0)
                    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, DL_MOVECURSOR);
                else
                    SetWindowLongPtr(hwnd, DWLP_MSGRESULT, DL_STOPCURSOR);
                ret |= 1; break;
              case DL_DROPPED:
                if (hdl->dragging) {
                    dest = pl_itemfrompt(dlm->hWnd, dlm->ptCursor, true);
                    if (dest > hdl->dummyitem) dest = hdl->dummyitem;
                    p_DrawInsert (hwnd, dlm->hWnd, -1);
                }
                SendDlgItemMessage(hwnd, hdl->listid,
                                   LB_DELETESTRING, hdl->dummyitem, 0);
                if (hdl->dragging) {
                    hdl->dragging = false;
                    if (dest >= 0) {
                        /* Correct for "missing" item. */
                        if (dest > hdl->srcitem) dest--;
                        pl_moveitem(hwnd, hdl->listid, hdl->srcitem, dest);
                    }
                    ret |= 2;
                }
                ret |= 1; break;
            }
        }

    } else {

        if (((LOWORD(wParam) == hdl->upbid) ||
             (LOWORD(wParam) == hdl->dnbid)) &&
            ((HIWORD(wParam) == BN_CLICKED) ||
             (HIWORD(wParam) == BN_DOUBLECLICKED))) {
            /* Move an item up or down the list. */
            /* Get the current selection, if any. */
            int selection = SendDlgItemMessage (hwnd, hdl->listid, LB_GETCURSEL, 0, 0);
            if (selection == LB_ERR) {
                MessageBeep(0);
            } else {
                int nitems;
                /* Get the total number of items. */
                nitems = SendDlgItemMessage (hwnd, hdl->listid, LB_GETCOUNT, 0, 0);
                /* Should we do anything? */
                if (LOWORD(wParam) == hdl->upbid && (selection > 0))
                    pl_moveitem(hwnd, hdl->listid, selection, selection - 1);
                else if (LOWORD(wParam) == hdl->dnbid && (selection < nitems - 1))
                    pl_moveitem(hwnd, hdl->listid, selection, selection + 1);
                ret |= 2;
            }

        }

    }

    if (array) {
        /* Update array to match the list box. */
        for (i=0; i < maxmemb; i++)
            array[i] = SendDlgItemMessage (hwnd, hdl->listid, LB_GETITEMDATA,
                                           i, 0);
    }

    return ret;
}

/*
 * A progress bar (from Common Controls). We like our progress bars
 * to be smooth and unbroken, without those ugly divisions; some
 * older compilers may not support that, but that's life.
 */
void progressbar(struct ctlpos *cp, int id)
{
    RECT r;

    r.left = GAPBETWEEN;
    r.top = cp->ypos;
    r.right = cp->width;
    r.bottom = PROGBARHEIGHT;
    cp->ypos += r.bottom + GAPBETWEEN;

    doctl(cp, r, PROGRESS_CLASS, WS_CHILD | WS_VISIBLE
#ifdef PBS_SMOOTH
          | PBS_SMOOTH
#endif
          , WS_EX_CLIENTEDGE, "", id);
}

/* ----------------------------------------------------------------------
 * Platform-specific side of portable dialog-box mechanism.
 */

/*
 * This function takes a string, escapes all the ampersands, and
 * places a single (unescaped) ampersand in front of the first
 * occurrence of the given shortcut character (which may be
 * NO_SHORTCUT).
 *
 * Return value is a malloc'ed copy of the processed version of the
 * string.
 */
static char *shortcut_escape(const char *text, char shortcut)
{
    if (!text)
        return NULL;                   /* sfree won't choke on this */

    strbuf *sb = strbuf_new();
    shortcut = tolower((unsigned char)shortcut);

    const char *p = text;
    while (*p) {
        if (shortcut != NO_SHORTCUT &&
            tolower((unsigned char)*p) == shortcut) {
            put_byte(sb, '&');
            shortcut = NO_SHORTCUT;    /* stop it happening twice */
        } else if (*p == '&') {
            put_byte(sb, '&');
        }
        put_byte(sb, *p++);
    }
    return strbuf_to_str(sb);
}

void winctrl_add_shortcuts(struct dlgparam *dp, struct winctrl *c)
{
    int i;
    for (i = 0; i < lenof(c->shortcuts); i++)
        if (c->shortcuts[i] != NO_SHORTCUT) {
            unsigned char s = tolower((unsigned char)c->shortcuts[i]);
            assert(!dp->shortcuts[s]);
            dp->shortcuts[s] = true;
        }
}

void winctrl_rem_shortcuts(struct dlgparam *dp, struct winctrl *c)
{
    int i;
    for (i = 0; i < lenof(c->shortcuts); i++)
        if (c->shortcuts[i] != NO_SHORTCUT) {
            unsigned char s = tolower((unsigned char)c->shortcuts[i]);
            assert(dp->shortcuts[s]);
            dp->shortcuts[s] = false;
        }
}

static int winctrl_cmp_byctrl(void *av, void *bv)
{
    struct winctrl *a = (struct winctrl *)av;
    struct winctrl *b = (struct winctrl *)bv;
    if (a->ctrl < b->ctrl)
        return -1;
    else if (a->ctrl > b->ctrl)
        return +1;
    else
        return 0;
}
static int winctrl_cmp_byid(void *av, void *bv)
{
    struct winctrl *a = (struct winctrl *)av;
    struct winctrl *b = (struct winctrl *)bv;
    if (a->base_id < b->base_id)
        return -1;
    else if (a->base_id > b->base_id)
        return +1;
    else
        return 0;
}
static int winctrl_cmp_byctrl_find(void *av, void *bv)
{
    dlgcontrol *a = (dlgcontrol *)av;
    struct winctrl *b = (struct winctrl *)bv;
    if (a < b->ctrl)
        return -1;
    else if (a > b->ctrl)
        return +1;
    else
        return 0;
}
static int winctrl_cmp_byid_find(void *av, void *bv)
{
    int *a = (int *)av;
    struct winctrl *b = (struct winctrl *)bv;
    if (*a < b->base_id)
        return -1;
    else if (*a >= b->base_id + b->num_ids)
        return +1;
    else
        return 0;
}

void winctrl_init(struct winctrls *wc)
{
    wc->byctrl = newtree234(winctrl_cmp_byctrl);
    wc->byid = newtree234(winctrl_cmp_byid);
}
void winctrl_cleanup(struct winctrls *wc)
{
    struct winctrl *c;

    while ((c = index234(wc->byid, 0)) != NULL) {
        winctrl_remove(wc, c);
        sfree(c->data);
        sfree(c);
    }

    freetree234(wc->byctrl);
    freetree234(wc->byid);
    wc->byctrl = wc->byid = NULL;
}

void winctrl_add(struct winctrls *wc, struct winctrl *c)
{
    struct winctrl *ret;
    if (c->ctrl) {
        ret = add234(wc->byctrl, c);
        assert(ret == c);
    }
    ret = add234(wc->byid, c);
    assert(ret == c);
}

void winctrl_remove(struct winctrls *wc, struct winctrl *c)
{
    struct winctrl *ret;
    ret = del234(wc->byctrl, c);
    ret = del234(wc->byid, c);
    assert(ret == c);
}

struct winctrl *winctrl_findbyctrl(struct winctrls *wc, dlgcontrol *ctrl)
{
    return find234(wc->byctrl, ctrl, winctrl_cmp_byctrl_find);
}

struct winctrl *winctrl_findbyid(struct winctrls *wc, int id)
{
    return find234(wc->byid, &id, winctrl_cmp_byid_find);
}

struct winctrl *winctrl_findbyindex(struct winctrls *wc, int index)
{
    return index234(wc->byid, index);
}

static void move_windows(HWND hwnd, int base_id, int num_ids, LONG dy)
{
    if (!dy)
        return;
    for (int i = 0; i < num_ids; i++) {
        HWND win = GetDlgItem(hwnd, base_id + i);

        RECT rect;
        if (!GetWindowRect(win, &rect))
            continue;

        POINT p;
        p.x = rect.left;
        p.y = rect.top + dy;
        if (!ScreenToClient(hwnd, &p))
            continue;

        SetWindowPos(win, NULL, p.x, p.y, 0, 0,
                     SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
    }
}

/*
 * PuTTY-beter: the state a saved-session list box carries - drag-and-drop of
 * its rows, and the folder tree. It is attached to the list box with
 * SetWindowSubclass(); see the definitions with the other list box code
 * further down. The struct itself is defined there, so these are forward
 * declarations only.
 */
struct listbox_state;

static void listbox_state_install(struct dlgparam *dp, dlgcontrol *ctrl,
                                  HWND hwnd, int id);
static struct listbox_state *listbox_state_of(struct dlgparam *dp,
                                              struct winctrl *c);
static void listbox_tree_forget_paths(struct listbox_state *ls);
static void listbox_tree_add_path(struct listbox_state *ls, const char *path,
                                  int at);
static int listbox_tree_insert_at(struct listbox_state *ls, const char *path);
static bool listbox_tree_hidden(struct listbox_state *ls, const char *path);
static bool listbox_tree_is_folder(const char *path);
static char *listbox_tree_row_text(struct listbox_state *ls, const char *path);
static bool listbox_tree_toggle(struct dlgcontrol *ctrl, struct dlgparam *dp,
                                struct winctrl *c);

void winctrl_layout(struct dlgparam *dp, struct winctrls *wc,
                    struct ctlpos *cp, struct controlset *s, int *id)
{
    struct ctlpos columns[16];
    int ncols, colstart, colspan;

    struct ctlpos tabdelays[16];
    dlgcontrol *tabdelayed[16];
    int ntabdelays;

    struct ctlpos pos;

    char shortcuts[MAX_SHORTCUTS_PER_CTRL];
    int nshortcuts;
    char *escaped;
    int i, actual_base_id, base_id, num_ids, align_id_relative;
    void *data;

    base_id = *id;

    ctrlset_normalise_aligns(s);

    /* Start a containing box, if we have a boxname. */
    if (s->boxname && *s->boxname) {
        struct winctrl *c = snew(struct winctrl);
        c->ctrl = NULL;
        c->base_id = c->align_id = base_id;
        c->num_ids = 1;
        c->data = NULL;
        memset(c->shortcuts, NO_SHORTCUT, lenof(c->shortcuts));
        winctrl_add(wc, c);
        beginbox(cp, s->boxtitle, base_id);
        base_id++;
    }

    /* Draw a title, if we have one. */
    if (!s->boxname && s->boxtitle) {
        struct winctrl *c = snew(struct winctrl);
        c->ctrl = NULL;
        c->base_id = c->align_id = base_id;
        c->num_ids = 1;
        c->data = dupstr(s->boxtitle);
        memset(c->shortcuts, NO_SHORTCUT, lenof(c->shortcuts));
        winctrl_add(wc, c);
        paneltitle(cp, base_id);
        base_id++;
    }

    /* Initially we have just one column. */
    ncols = 1;
    columns[0] = *cp;                  /* structure copy */

    /* And initially, there are no pending tab-delayed controls. */
    ntabdelays = 0;

    /* Loop over each control in the controlset. */
    for (i = 0; i < s->ncontrols; i++) {
        dlgcontrol *ctrl = s->ctrls[i];

        /*
         * Generic processing that pertains to all control types.
         * At the end of this if statement, we'll have produced
         * `ctrl' (a pointer to the control we have to create, or
         * think about creating, in this iteration of the loop),
         * `pos' (a suitable ctlpos with which to position it), and
         * `c' (a winctrl structure to receive details of the
         * dialog IDs). Or we'll have done a `continue', if it was
         * CTRL_COLUMNS and doesn't require any control creation at
         * all.
         */
        if (ctrl->type == CTRL_COLUMNS) {
            assert((ctrl->columns.ncols == 1) ^ (ncols == 1));

            if (ncols == 1) {
                /*
                 * We're splitting into multiple columns.
                 */
                int lpercent, rpercent, lx, rx, i;

                ncols = ctrl->columns.ncols;
                assert(ncols <= lenof(columns));
                for (i = 1; i < ncols; i++)
                    columns[i] = columns[0];   /* structure copy */

                lpercent = 0;
                for (i = 0; i < ncols; i++) {
                    rpercent = lpercent + ctrl->columns.percentages[i];
                    lx = columns[i].xoff + lpercent *
                        (columns[i].width + GAPBETWEEN) / 100;
                    rx = columns[i].xoff + rpercent *
                        (columns[i].width + GAPBETWEEN) / 100;
                    columns[i].xoff = lx;
                    columns[i].width = rx - lx - GAPBETWEEN;
                    lpercent = rpercent;
                }
            } else {
                /*
                 * We're recombining the various columns into one.
                 */
                int maxy = columns[0].ypos;
                int i;
                for (i = 1; i < ncols; i++)
                    if (maxy < columns[i].ypos)
                        maxy = columns[i].ypos;
                ncols = 1;
                columns[0] = *cp;      /* structure copy */
                columns[0].ypos = maxy;
            }

            continue;
        } else if (ctrl->type == CTRL_TABDELAY) {
            int i;

            assert(!ctrl->delay_taborder);
            ctrl = ctrl->tabdelay.ctrl;

            for (i = 0; i < ntabdelays; i++)
                if (tabdelayed[i] == ctrl)
                    break;
            assert(i < ntabdelays);    /* we have to have found it */

            pos = tabdelays[i];        /* structure copy */

            colstart = colspan = -1;   /* indicate this was tab-delayed */

        } else {
            /*
             * If it wasn't one of those, it's a genuine control;
             * so we'll have to compute a position for it now, by
             * checking its column span.
             */
            int col;

            colstart = COLUMN_START(ctrl->column);
            colspan = COLUMN_SPAN(ctrl->column);

            pos = columns[colstart];   /* structure copy */
            pos.width = columns[colstart+colspan-1].width +
                (columns[colstart+colspan-1].xoff - columns[colstart].xoff);

            for (col = colstart; col < colstart+colspan; col++)
                if (pos.ypos < columns[col].ypos)
                    pos.ypos = columns[col].ypos;

            /*
             * If this control is to be tabdelayed, add it to the
             * tabdelay list, and unset pos.hwnd to inhibit actual
             * control creation.
             */
            if (ctrl->delay_taborder) {
                assert(ntabdelays < lenof(tabdelays));
                tabdelays[ntabdelays] = pos;   /* structure copy */
                tabdelayed[ntabdelays] = ctrl;
                ntabdelays++;
                pos.hwnd = NULL;
            }
        }

        /* Most controls don't need anything in c->data. */
        data = NULL;

        /* And they all start off with no shortcuts registered. */
        memset(shortcuts, NO_SHORTCUT, lenof(shortcuts));
        nshortcuts = 0;

        /* Almost all controls start at base_id. */
        actual_base_id = base_id;

        /* For vertical alignment purposes, the most relevant control
         * in a group is usually the last one. But that can be
         * overridden occasionally. */
        align_id_relative = -1;

        /*
         * Now we're ready to actually create the control, by
         * switching on its type.
         */
        switch (ctrl->type) {
          case CTRL_TEXT:
            if (ctrl->text.wrap) {
                char *wrapped, *escaped;
                int lines;
                num_ids = 1;
                wrapped = staticwrap(&pos, cp->hwnd,
                                     ctrl->label, &lines);
                escaped = shortcut_escape(wrapped, NO_SHORTCUT);
                statictext(&pos, escaped, lines, base_id);
                sfree(escaped);
                sfree(wrapped);
            } else {
                num_ids = 1;
                editboxfw(&pos, false, true, NULL, 0, base_id);
                SetDlgItemText(pos.hwnd, base_id, ctrl->label);
                MakeDlgItemBorderless(pos.hwnd, base_id);
            }
            break;
          case CTRL_EDITBOX:
            num_ids = 2;               /* static, edit */
            escaped = shortcut_escape(ctrl->label,
                                      ctrl->editbox.shortcut);
            shortcuts[nshortcuts++] = ctrl->editbox.shortcut;
            if (ctrl->editbox.multiline) {
                /*
                 * PuTTY-beter: a multi-line box for a block of free text, which
                 * here means the session comment (tech_detail.md section 7).
                 * The GTK side uses a GtkTextView; this is the same ES_MULTILINE
                 * edit that windows/dialog.c builds for its "more information"
                 * box, and the panel is grown to fit it because the layout
                 * below it moves down with it.
                 */
                bigeditctrl(&pos, escaped, base_id, base_id+1,
                            MULTILINE_EDIT_LINES);
            } else if (ctrl->editbox.percentwidth == 100) {
                if (ctrl->editbox.has_list)
                    combobox(&pos, escaped,
                             base_id, base_id+1);
                else
                    editboxfw(&pos, ctrl->editbox.password, false, escaped,
                              base_id, base_id+1);
            } else {
                if (ctrl->editbox.has_list) {
                    staticcombo(&pos, escaped, base_id, base_id+1,
                                ctrl->editbox.percentwidth);
                } else {
                    (ctrl->editbox.password ? staticpassedit : staticedit)
                        (&pos, escaped, base_id, base_id+1,
                         ctrl->editbox.percentwidth);
                }
            }
            /*
             * PuTTY-beter: an edit box that has been given a placeholder
             * shows it while it is empty, which is how the "Saved Sessions"
             * box advertises that a leading '/' searches the list rather
             * than naming a session. GTK has gtk_entry_set_placeholder_text();
             * the Windows equivalent is EM_SETCUEBANNER, which needs
             * comctl32 6 - the manifest in putty.mft asks for it - and a
             * wide string, because this build is ANSI. wParam false means
             * the hint goes away as soon as there is text to see.
             */
            if (pos.hwnd && ctrl->editbox.placeholder) {
                HWND edit = GetDlgItem(pos.hwnd, base_id+1);
                if (edit) {
                    wchar_t *w = dup_mb_to_wc(DEFAULT_CODEPAGE,
                                              ctrl->editbox.placeholder);
                    SendMessageW(edit, EM_SETCUEBANNER, false, (LPARAM)w);
                    sfree(w);
                }
            }
            sfree(escaped);
            break;
          case CTRL_RADIO: {
            num_ids = ctrl->radio.nbuttons + 1;   /* label as well */
            struct radio *buttons;
            int i;

            escaped = shortcut_escape(ctrl->label,
                                      ctrl->radio.shortcut);
            shortcuts[nshortcuts++] = ctrl->radio.shortcut;

            buttons = snewn(ctrl->radio.nbuttons, struct radio);

            for (i = 0; i < ctrl->radio.nbuttons; i++) {
                buttons[i].text =
                    shortcut_escape(ctrl->radio.buttons[i],
                                    (char)(ctrl->radio.shortcuts ?
                                           ctrl->radio.shortcuts[i] :
                                           NO_SHORTCUT));
                buttons[i].id = base_id + 1 + i;
                if (ctrl->radio.shortcuts) {
                    assert(nshortcuts < MAX_SHORTCUTS_PER_CTRL);
                    shortcuts[nshortcuts++] = ctrl->radio.shortcuts[i];
                }
            }

            radioline_common(&pos, escaped, base_id,
                             ctrl->radio.ncolumns,
                             buttons, ctrl->radio.nbuttons);

            for (i = 0; i < ctrl->radio.nbuttons; i++) {
                sfree((char *)buttons[i].text);
            }
            sfree(buttons);
            sfree(escaped);
            break;
          }
          case CTRL_CHECKBOX:
            num_ids = 1;
            escaped = shortcut_escape(ctrl->label,
                                      ctrl->checkbox.shortcut);
            shortcuts[nshortcuts++] = ctrl->checkbox.shortcut;
            checkbox(&pos, escaped, base_id);
            sfree(escaped);
            break;
          case CTRL_BUTTON:
            escaped = shortcut_escape(ctrl->label,
                                      ctrl->button.shortcut);
            shortcuts[nshortcuts++] = ctrl->button.shortcut;
            if (ctrl->button.iscancel)
                actual_base_id = IDCANCEL;
            num_ids = 1;
            /*
             * PuTTY-beter: the shared code marks the saved-session buttons
             * `force_left' so that they keep their natural width instead of
             * stretching across their column. Windows has to be told to
             * honour that.
             */
            if (ctrl->force_left)
                button_force_left(&pos, escaped, actual_base_id,
                                  ctrl->button.isdefault);
            else
                button(&pos, escaped, actual_base_id, ctrl->button.isdefault);
            sfree(escaped);
            break;
          case CTRL_LISTBOX:
            num_ids = 2;
            escaped = shortcut_escape(ctrl->label,
                                      ctrl->listbox.shortcut);
            shortcuts[nshortcuts++] = ctrl->listbox.shortcut;
            if (ctrl->listbox.draglist) {
                data = snew(struct prefslist);
                num_ids = 4;
                prefslist(data, &pos, ctrl->listbox.height, escaped,
                          base_id, base_id+1, base_id+2, base_id+3);
                shortcuts[nshortcuts++] = 'u';   /* Up */
                shortcuts[nshortcuts++] = 'd';   /* Down */
            } else if (ctrl->listbox.height == 0) {
                /* Drop-down list. */
                if (ctrl->listbox.percentwidth == 100) {
                    staticddlbig(&pos, escaped,
                                 base_id, base_id+1);
                } else {
                    staticddl(&pos, escaped, base_id,
                              base_id+1, ctrl->listbox.percentwidth);
                }
            } else {
                /* Ordinary list. */
                listbox(&pos, escaped, base_id, base_id+1,
                        ctrl->listbox.height, ctrl->listbox.multisel);
            }
            if (ctrl->listbox.ncols) {
                /*
                 * This method of getting the box width is a bit of
                 * a hack; we'd do better to try to retrieve the
                 * actual width in dialog units from doctl() just
                 * before MapDialogRect. But that's going to be no
                 * fun, and this should be good enough accuracy.
                 */
                int width = cp->width * ctrl->listbox.percentwidth;
                int *tabarray;
                int i, percent;

                tabarray = snewn(ctrl->listbox.ncols-1, int);
                percent = 0;
                for (i = 0; i < ctrl->listbox.ncols-1; i++) {
                    percent += ctrl->listbox.percentages[i];
                    tabarray[i] = width * percent / 10000;
                }
                SendDlgItemMessage(cp->hwnd, base_id+1, LB_SETTABSTOPS,
                                   ctrl->listbox.ncols-1, (LPARAM)tabarray);
                sfree(tabarray);
            }
            sfree(escaped);
            break;
          case CTRL_FILESELECT:
            escaped = shortcut_escape(ctrl->label, ctrl->fileselect.shortcut);
            shortcuts[nshortcuts++] = ctrl->fileselect.shortcut;
            num_ids = 3;
            if (!ctrl->fileselect.just_button) {
                editbutton(&pos, escaped, base_id, base_id+1,
                           "Browse...", base_id+2);
            } else {
                button(&pos, escaped, base_id+2, false);
            }
            sfree(escaped);
            break;
          case CTRL_FONTSELECT:
            num_ids = 3;
            escaped = shortcut_escape(ctrl->label,
                                      ctrl->fontselect.shortcut);
            shortcuts[nshortcuts++] = ctrl->fontselect.shortcut;
            statictext(&pos, escaped, 1, base_id);
            staticbtn(&pos, "", base_id+1, "Change...", base_id+2);
            data = fontspec_new_default();
            sfree(escaped);
            break;
          default:
            unreachable("bad control type in winctrl_layout");
        }

        /* Translate the original align_id_relative of -1 into n-1 */
        if (align_id_relative < 0)
            align_id_relative += num_ids;

        /*
         * Create a `struct winctrl' for this control, and advance
         * the dialog ID counter, if it's actually been created
         * (and isn't tabdelayed).
         */
        if (pos.hwnd) {
            struct winctrl *c = snew(struct winctrl);

            c->ctrl = ctrl;
            c->base_id = actual_base_id;
            c->align_id = c->base_id + align_id_relative;
            c->num_ids = num_ids;
            c->data = data;
            memcpy(c->shortcuts, shortcuts, sizeof(shortcuts));
            winctrl_add(wc, c);
            winctrl_add_shortcuts(dp, c);
            if (actual_base_id == base_id)
                base_id += num_ids;

            /*
             * PuTTY-beter: a list box that has a drop handler accepts a row
             * dragged onto another row, and a list box that is a tree needs
             * somewhere to keep which folders are closed. Both are handled by
             * the box itself, so it gets a subclassed window procedure rather
             * than the dialog engine having to track the mouse.
             */
            if (ctrl->type == CTRL_LISTBOX && ctrl->listbox.height != 0 &&
                (ctrl->listbox.tree || ctrl->listbox.drag_handler))
                listbox_state_install(dp, ctrl, pos.hwnd, actual_base_id + 1);

            if (ctrl->align_next_to) {
                /*
                 * Implement align_next_to by looking at the y extents
                 * of the two controls now that both are created, and
                 * moving one or the other downwards so that they're
                 * centred on a common horizontal line.
                 */
                LONG mid2 = 0;
                for (dlgcontrol *thisctrl = ctrl; thisctrl;
                     thisctrl = thisctrl->align_next_to) {
                    struct winctrl *thisc = winctrl_findbyctrl(wc, thisctrl);
                    assert(thisc);

                    HWND win = GetDlgItem(pos.hwnd, thisc->align_id);
                    assert(win);

                    RECT rect;
                    if (!GetWindowRect(win, &rect))
                        continue;
                    if (mid2 < rect.top + rect.bottom)
                        mid2 = rect.top + rect.bottom;
                }

                for (dlgcontrol *thisctrl = ctrl; thisctrl;
                     thisctrl = thisctrl->align_next_to) {
                    struct winctrl *thisc = winctrl_findbyctrl(wc, thisctrl);
                    assert(thisc);

                    HWND win = GetDlgItem(pos.hwnd, thisc->align_id);
                    assert(win);

                    RECT rect;
                    if (!GetWindowRect(win, &rect))
                        continue;

                    LONG dy = (mid2 - (rect.top + rect.bottom)) / 2;
                    move_windows(pos.hwnd, thisc->base_id, thisc->num_ids, dy);
                }
            }
        } else {
            sfree(data);
        }

        if (colstart >= 0) {
            /*
             * Update the ypos in all columns crossed by this
             * control.
             */
            int i;
            for (i = colstart; i < colstart+colspan; i++)
                columns[i].ypos = pos.ypos;
        }
    }

    /*
     * We've now finished laying out the controls; so now update
     * the ctlpos and control ID that were passed in, terminate
     * any containing box, and return.
     */
    for (i = 0; i < ncols; i++)
        if (cp->ypos < columns[i].ypos)
            cp->ypos = columns[i].ypos;
    *id = base_id;

    if (s->boxname && *s->boxname)
        endbox(cp);
}

static void winctrl_set_focus(dlgcontrol *ctrl, struct dlgparam *dp,
                              bool has_focus)
{
    if (has_focus) {
        if (dp->focused)
            dp->lastfocused = dp->focused;
        dp->focused = ctrl;
    } else if (!has_focus && dp->focused == ctrl) {
        dp->lastfocused = dp->focused;
        dp->focused = NULL;
    }
}

dlgcontrol *dlg_last_focused(dlgcontrol *ctrl, dlgparam *dp)
{
    return dp->focused == ctrl ? dp->lastfocused : dp->focused;
}

/*
 * The dialog-box procedure calls this function to handle Windows
 * messages on a control we manage.
 */
bool winctrl_handle_command(struct dlgparam *dp, UINT msg,
                            WPARAM wParam, LPARAM lParam)
{
    struct winctrl *c;
    dlgcontrol *ctrl;
    int i, id;
    bool ret;
    static UINT draglistmsg = WM_NULL;

    /*
     * Filter out pointless window messages. Our interest is in
     * WM_COMMAND and the drag list message, and nothing else.
     */
    if (draglistmsg == WM_NULL)
        draglistmsg = RegisterWindowMessage (DRAGLISTMSGSTRING);

    if (msg != draglistmsg && msg != WM_COMMAND && msg != WM_DRAWITEM)
        return false;

    /*
     * Look up the control ID in our data.
     */
    c = NULL;
    for (i = 0; i < dp->nctrltrees; i++) {
        c = winctrl_findbyid(dp->controltrees[i], LOWORD(wParam));
        if (c)
            break;
    }
    if (!c)
        return false;                  /* we have nothing to do */

    if (msg == WM_DRAWITEM) {
        /*
         * Owner-draw request for a panel title.
         */
        LPDRAWITEMSTRUCT di = (LPDRAWITEMSTRUCT) lParam;
        HDC hdc = di->hDC;
        RECT r = di->rcItem;
        SIZE s;

        SetMapMode(hdc, MM_TEXT);      /* ensure logical units == pixels */

        GetTextExtentPoint32(hdc, (char *)c->data,
                             strlen((char *)c->data), &s);
        DrawEdge(hdc, &r, EDGE_ETCHED, BF_ADJUST | BF_RECT);
        TextOut(hdc,
                r.left + (r.right-r.left-s.cx)/2,
                r.top + (r.bottom-r.top-s.cy)/2,
                (char *)c->data, strlen((char *)c->data));

        return true;
    }

    ctrl = c->ctrl;
    id = LOWORD(wParam) - c->base_id;

    if (!ctrl || !ctrl->handler)
        return false;                  /* nothing we can do here */

    /*
     * From here on we do not issue `return' statements until the
     * very end of the dialog box: any event handler is entitled to
     * ask for a colour selector, so we _must_ always allow control
     * to reach the end of this switch statement so that the
     * subsequent code can test dp->coloursel_wanted().
     */
    ret = false;
    dp->coloursel_wanted = false;

    /*
     * Now switch on the control type and the message.
     */
    switch (ctrl->type) {
      case CTRL_EDITBOX:
        if (msg == WM_COMMAND && !ctrl->editbox.has_list &&
            (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == EN_SETFOCUS);
        if (msg == WM_COMMAND && ctrl->editbox.has_list &&
            (HIWORD(wParam)==CBN_SETFOCUS || HIWORD(wParam)==CBN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == CBN_SETFOCUS);

        if (msg == WM_COMMAND && !ctrl->editbox.has_list &&
            HIWORD(wParam) == EN_CHANGE)
            ctrl->handler(ctrl, dp, dp->data, EVENT_VALCHANGE);
        if (msg == WM_COMMAND &&
            ctrl->editbox.has_list) {
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int index, len;
                char *text;

                index = SendDlgItemMessage(dp->hwnd, c->base_id+1,
                                           CB_GETCURSEL, 0, 0);
                len = SendDlgItemMessage(dp->hwnd, c->base_id+1,
                                         CB_GETLBTEXTLEN, index, 0);
                text = snewn(len+1, char);
                SendDlgItemMessage(dp->hwnd, c->base_id+1, CB_GETLBTEXT,
                                   index, (LPARAM)text);
                SetDlgItemText(dp->hwnd, c->base_id+1, text);
                sfree(text);
                ctrl->handler(ctrl, dp, dp->data, EVENT_VALCHANGE);
            } else if (HIWORD(wParam) == CBN_EDITCHANGE) {
                ctrl->handler(ctrl, dp, dp->data, EVENT_VALCHANGE);
            } else if (HIWORD(wParam) == CBN_KILLFOCUS) {
                ctrl->handler(ctrl, dp, dp->data, EVENT_REFRESH);
            }

        }
        break;
      case CTRL_RADIO:
        if (msg == WM_COMMAND &&
            (HIWORD(wParam) == BN_SETFOCUS || HIWORD(wParam) == BN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == BN_SETFOCUS);
        /*
         * We sometimes get spurious BN_CLICKED messages for the
         * radio button that is just about to _lose_ selection, if
         * we're switching using the arrow keys. Therefore we
         * double-check that the button in wParam is actually
         * checked before generating an event.
         */
        if (msg == WM_COMMAND &&
            (HIWORD(wParam) == BN_CLICKED ||
             HIWORD(wParam) == BN_DOUBLECLICKED) &&
            IsDlgButtonChecked(dp->hwnd, LOWORD(wParam))) {
            ctrl->handler(ctrl, dp, dp->data, EVENT_VALCHANGE);
        }
        break;
      case CTRL_CHECKBOX:
        if (msg == WM_COMMAND &&
            (HIWORD(wParam) == BN_SETFOCUS || HIWORD(wParam) == BN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == BN_SETFOCUS);
        if (msg == WM_COMMAND &&
            (HIWORD(wParam) == BN_CLICKED ||
             HIWORD(wParam) == BN_DOUBLECLICKED)) {
            ctrl->handler(ctrl, dp, dp->data, EVENT_VALCHANGE);
        }
        break;
      case CTRL_BUTTON:
        if (msg == WM_COMMAND &&
            (HIWORD(wParam) == BN_SETFOCUS || HIWORD(wParam) == BN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == BN_SETFOCUS);
        if (msg == WM_COMMAND &&
            (HIWORD(wParam) == BN_CLICKED ||
             HIWORD(wParam) == BN_DOUBLECLICKED)) {
            ctrl->handler(ctrl, dp, dp->data, EVENT_ACTION);
        }
        break;
      case CTRL_LISTBOX:
        if (msg == WM_COMMAND && ctrl->listbox.height != 0 &&
            (HIWORD(wParam)==LBN_SETFOCUS || HIWORD(wParam)==LBN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == LBN_SETFOCUS);
        if (msg == WM_COMMAND && ctrl->listbox.height == 0 &&
            (HIWORD(wParam)==CBN_SETFOCUS || HIWORD(wParam)==CBN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == CBN_SETFOCUS);
        if (msg == WM_COMMAND && id >= 2 &&
            (HIWORD(wParam) == BN_SETFOCUS || HIWORD(wParam) == BN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == BN_SETFOCUS);
        if (ctrl->listbox.draglist) {
            int pret;
            pret = handle_prefslist(c->data, NULL, 0, (msg != WM_COMMAND),
                                    dp->hwnd, wParam, lParam);
            if (pret & 2)
                ctrl->handler(ctrl, dp, dp->data, EVENT_VALCHANGE);
            ret = pret & 1;
        } else {
            if (msg == WM_COMMAND && HIWORD(wParam) == LBN_DBLCLK) {
                /*
                 * PuTTY-beter: in the saved-session list a double click on a
                 * folder row opens or closes it, and the list is rebuilt. A
                 * folder cannot be loaded, so nothing is lost by taking the
                 * gesture over from the shared code, which would only beep.
                 */
                bool toggled = false;

                if (ctrl->listbox.tree)
                    toggled = listbox_tree_toggle(ctrl, dp, c);

                if (!toggled) {
                    SetCapture(dp->hwnd);
                    ctrl->handler(ctrl, dp, dp->data, EVENT_ACTION);
                }
            } else if (msg == WM_COMMAND && HIWORD(wParam) == LBN_SELCHANGE) {
                ctrl->handler(ctrl, dp, dp->data, EVENT_SELCHANGE);
            }
        }
        break;
      case CTRL_FILESELECT:
        if (msg == WM_COMMAND && id == 1 &&
            (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == EN_SETFOCUS);
        if (msg == WM_COMMAND && id == 2 &&
            (HIWORD(wParam) == BN_SETFOCUS || HIWORD(wParam) == BN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == BN_SETFOCUS);
        if (msg == WM_COMMAND && id == 1 && HIWORD(wParam) == EN_CHANGE)
            ctrl->handler(ctrl, dp, dp->data, EVENT_VALCHANGE);
        if (id == 2 &&
            (msg == WM_COMMAND &&
             (HIWORD(wParam) == BN_CLICKED ||
              HIWORD(wParam) == BN_DOUBLECLICKED))) {
            Filename *fn_prev = NULL;
            if (!ctrl->fileselect.just_button) {
                wchar_t *text = GetDlgItemTextW_alloc(dp->hwnd, c->base_id+1);
                if (*text)
                    fn_prev = filename_from_wstr(text);
                sfree(text);
            }

            /*
             * PuTTY-beter: a setting that names a folder gets the folder
             * picker. The file dialog can only return a file name, which is
             * why the transfer folder's Browse button used to insist on a
             * file and a directory could not be selected at all.
             */
            Filename *fn = ctrl->fileselect.for_directory ?
                request_directory(dp->hwnd, ctrl->fileselect.title, fn_prev) :
                request_file(
                    dp->hwnd, ctrl->fileselect.title, fn_prev,
                    ctrl->fileselect.for_writing, NULL, false,
                    ctrl->fileselect.filter);
            if (fn_prev)
                filename_free(fn_prev);

            if (fn) {
                if (!ctrl->fileselect.just_button) {
                    SetDlgItemTextW(dp->hwnd, c->base_id + 1,
                                    filename_to_wstr(fn));
                    ctrl->handler(ctrl, dp, dp->data, EVENT_VALCHANGE);
                } else {
                    assert(!c->data);
                    c->data = fn;
                    ctrl->handler(ctrl, dp, dp->data, EVENT_ACTION);
                    c->data = NULL;
                }
                filename_free(fn);
            }
        }
        break;
      case CTRL_FONTSELECT:
        if (msg == WM_COMMAND && id == 2 &&
            (HIWORD(wParam) == BN_SETFOCUS || HIWORD(wParam) == BN_KILLFOCUS))
            winctrl_set_focus(ctrl, dp, HIWORD(wParam) == BN_SETFOCUS);
        if (id == 2 &&
            (msg == WM_COMMAND &&
             (HIWORD(wParam) == BN_CLICKED ||
              HIWORD(wParam) == BN_DOUBLECLICKED))) {
            CHOOSEFONT cf;
            LOGFONT lf;
            HDC hdc;
            FontSpec *fs = (FontSpec *)c->data;

            hdc = GetDC(0);
            lf.lfHeight = -MulDiv(fs->height,
                                  GetDeviceCaps(hdc, LOGPIXELSY), 72);
            ReleaseDC(0, hdc);
            lf.lfWidth = lf.lfEscapement = lf.lfOrientation = 0;
            lf.lfItalic = lf.lfUnderline = lf.lfStrikeOut = 0;
            lf.lfWeight = (fs->isbold ? FW_BOLD : 0);
            lf.lfCharSet = fs->charset;
            lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
            lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
            lf.lfQuality = DEFAULT_QUALITY;
            lf.lfPitchAndFamily = FIXED_PITCH | FF_DONTCARE;
            strncpy(lf.lfFaceName, fs->name,
                    sizeof(lf.lfFaceName) - 1);
            lf.lfFaceName[sizeof(lf.lfFaceName) - 1] = '\0';

            cf.lStructSize = sizeof(cf);
            cf.hwndOwner = dp->hwnd;
            cf.lpLogFont = &lf;
            cf.Flags = (dp->fixed_pitch_fonts ? CF_FIXEDPITCHONLY : 0) |
                CF_FORCEFONTEXIST | CF_INITTOLOGFONTSTRUCT | CF_SCREENFONTS;

            if (ChooseFont(&cf)) {
                fs = fontspec_new(lf.lfFaceName, (lf.lfWeight == FW_BOLD),
                                  cf.iPointSize / 10, lf.lfCharSet);
                dlg_fontsel_set(ctrl, dp, fs);
                fontspec_free(fs);

                ctrl->handler(ctrl, dp, dp->data, EVENT_VALCHANGE);
            }
        }
        break;
    }

    /*
     * If the above event handler has asked for a colour selector,
     * now is the time to generate one.
     */
    if (dp->coloursel_wanted) {
        static CHOOSECOLOR cc;
        static DWORD custom[16] = { 0 };    /* zero initialisers */
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = dp->hwnd;
        cc.hInstance = (HWND) hinst;
        cc.lpCustColors = custom;
        cc.rgbResult = RGB(dp->coloursel_result.r,
                           dp->coloursel_result.g,
                           dp->coloursel_result.b);
        cc.Flags = CC_FULLOPEN | CC_RGBINIT;
        if (ChooseColor(&cc)) {
            dp->coloursel_result.r =
                (unsigned char) (cc.rgbResult & 0xFF);
            dp->coloursel_result.g =
                (unsigned char) (cc.rgbResult >> 8) & 0xFF;
            dp->coloursel_result.b =
                (unsigned char) (cc.rgbResult >> 16) & 0xFF;
            dp->coloursel_result.ok = true;
        } else
            dp->coloursel_result.ok = false;
        ctrl->handler(ctrl, dp, dp->data, EVENT_CALLBACK);
    }

    return ret;
}

/*
 * This function can be called to produce context help on a
 * control. Returns true if it has actually launched some help.
 */
bool winctrl_context_help(struct dlgparam *dp, HWND hwnd, int id)
{
    int i;
    struct winctrl *c;

    /*
     * Look up the control ID in our data.
     */
    c = NULL;
    for (i = 0; i < dp->nctrltrees; i++) {
        c = winctrl_findbyid(dp->controltrees[i], id);
        if (c)
            break;
    }
    if (!c)
        return false;                  /* we have nothing to do */

    /*
     * This is the Windows front end, so we're allowed to assume
     * `helpctx' is a context string.
     */
    if (!c->ctrl || !c->ctrl->helpctx)
        return false;            /* no help available for this ctrl */

    launch_help(hwnd, c->ctrl->helpctx);
    return true;
}

/*
 * Now the various functions that the platform-independent
 * mechanism can call to access the dialog box entries.
 */

static struct winctrl *dlg_findbyctrl(struct dlgparam *dp, dlgcontrol *ctrl)
{
    int i;

    for (i = 0; i < dp->nctrltrees; i++) {
        struct winctrl *c = winctrl_findbyctrl(dp->controltrees[i], ctrl);
        if (c)
            return c;
    }
    return NULL;
}

bool dlg_is_visible(dlgcontrol *ctrl, dlgparam *dp)
{
    /*
     * In this implementation of the dialog box, we physically
     * uncreate controls that aren't in a visible panel of the config
     * box. So we can tell if a control is visible just by checking if
     * it _exists_.
     */
    return dlg_findbyctrl(dp, ctrl) != NULL;
}

void dlg_radiobutton_set(dlgcontrol *ctrl, dlgparam *dp, int whichbutton)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_RADIO);
    CheckRadioButton(dp->hwnd,
                     c->base_id + 1,
                     c->base_id + c->ctrl->radio.nbuttons,
                     c->base_id + 1 + whichbutton);
}

int dlg_radiobutton_get(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int i;
    assert(c && c->ctrl->type == CTRL_RADIO);
    for (i = 0; i < c->ctrl->radio.nbuttons; i++)
        if (IsDlgButtonChecked(dp->hwnd, c->base_id + 1 + i))
            return i;
    unreachable("no radio button was checked");
}

void dlg_checkbox_set(dlgcontrol *ctrl, dlgparam *dp, bool checked)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_CHECKBOX);
    CheckDlgButton(dp->hwnd, c->base_id, checked);
}

bool dlg_checkbox_get(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_CHECKBOX);
    return 0 != IsDlgButtonChecked(dp->hwnd, c->base_id);
}

/*
 * PuTTY-beter: the two line-ending conversions a multi-line edit box needs.
 *
 * `dup_crlf' turns the configuration's LF-only text into the CRLF an edit
 * control wants (a CR that is already there is dropped first, so text that
 * somehow arrived with CRLF does not end up with CRCRLF); `dup_lf_only' does
 * the reverse, dropping a CR that precedes an LF. A lone CR - which a file
 * pasted into the box might contain - is left alone either way.
 */
static char *dup_crlf(const char *text)
{
    size_t len = strlen(text);
    char *ret = snewn(2 * len + 1, char);
    size_t at = 0;

    for (const char *p = text; *p; p++) {
        if (*p == '\r' && p[1] == '\n')
            continue;                  /* already CRLF; the LF adds the CR */
        if (*p == '\n')
            ret[at++] = '\r';
        ret[at++] = *p;
    }
    ret[at] = '\0';

    return ret;
}

static char *dup_lf_only(const char *text)
{
    size_t len = strlen(text);
    char *ret = snewn(len + 1, char);
    size_t at = 0;

    for (const char *p = text; *p; p++) {
        if (*p == '\r' && p[1] == '\n')
            continue;                  /* the edit's CRLF: the LF is enough */
        ret[at++] = *p;
    }
    ret[at] = '\0';

    return ret;
}

void dlg_editbox_set(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_EDITBOX);

    /*
     * PuTTY-beter: a multi-line edit wants CRLF between its lines, but the
     * value being set is the configuration's, and that is LF-only - the GTK
     * front end's GtkTextView produces LF, and settings.c escapes the two
     * separately so that a comment written on one platform reads correctly on
     * the other (tech_detail.md section 7). Convert on the way in and on the
     * way out, and the stored text stays the same on both.
     */
    if (c->ctrl->editbox.multiline) {
        char *crlf = dup_crlf(text);
        SetDlgItemText(dp->hwnd, c->base_id+1, crlf);
        sfree(crlf);
        return;
    }

    SetDlgItemText(dp->hwnd, c->base_id+1, text);
}

void dlg_editbox_set_utf8(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_EDITBOX);
    wchar_t *wtext = dup_mb_to_wc(CP_UTF8, text);
    SetDlgItemTextW(dp->hwnd, c->base_id+1, wtext);
    sfree(wtext);
}

char *dlg_editbox_get(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    char *text;

    assert(c && c->ctrl->type == CTRL_EDITBOX);

    text = GetDlgItemText_alloc(dp->hwnd, c->base_id+1);

    /* A multi-line box hands back CRLF; the configuration keeps LF. */
    if (c->ctrl->editbox.multiline) {
        char *lf = dup_lf_only(text);
        sfree(text);
        text = lf;
    }

    return text;
}

char *dlg_editbox_get_utf8(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_EDITBOX);
    wchar_t *wtext = GetDlgItemTextW_alloc(dp->hwnd, c->base_id+1);
    char *text = dup_wc_to_mb(CP_UTF8, wtext, "");
    sfree(wtext);
    return text;
}

void dlg_editbox_select_range(dlgcontrol *ctrl, dlgparam *dp,
                              size_t start, size_t len)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_EDITBOX);
    SendDlgItemMessage(dp->hwnd, c->base_id+1, EM_SETSEL, start, start+len);
}

/* The `listbox' functions can also apply to combo boxes. */
void dlg_listbox_clear(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int msg;
    assert(c &&
           (c->ctrl->type == CTRL_LISTBOX ||
            (c->ctrl->type == CTRL_EDITBOX &&
             c->ctrl->editbox.has_list)));
    msg = (c->ctrl->type==CTRL_LISTBOX && c->ctrl->listbox.height!=0 ?
           LB_RESETCONTENT : CB_RESETCONTENT);
    SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, 0, 0);

    /*
     * PuTTY-beter: the rows are about to be made again, so the record of what
     * each row is a name for goes with them. Which folders are closed stays.
     */
    if (c->ctrl->type == CTRL_LISTBOX && c->ctrl->listbox.tree) {
        struct listbox_state *ls = listbox_state_of(dp, c);

        if (ls)
            listbox_tree_forget_paths(ls);
    }
}

void dlg_listbox_del(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int msg;
    assert(c &&
           (c->ctrl->type == CTRL_LISTBOX ||
            (c->ctrl->type == CTRL_EDITBOX &&
             c->ctrl->editbox.has_list)));
    msg = (c->ctrl->type==CTRL_LISTBOX && c->ctrl->listbox.height!=0 ?
           LB_DELETESTRING : CB_DELETESTRING);
    SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, index, 0);
}

void dlg_listbox_add(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int msg;
    assert(c &&
           (c->ctrl->type == CTRL_LISTBOX ||
            (c->ctrl->type == CTRL_EDITBOX &&
             c->ctrl->editbox.has_list)));
    msg = (c->ctrl->type==CTRL_LISTBOX && c->ctrl->listbox.height!=0 ?
           LB_ADDSTRING : CB_ADDSTRING);
    SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, 0, (LPARAM)text);
}

/*
 * Each listbox entry may have a numeric id associated with it.
 * Note that some front ends only permit a string to be stored at
 * each position, which means that _if_ you put two identical
 * strings in any listbox then you MUST not assign them different
 * IDs and expect to get meaningful results back.
 */
void dlg_listbox_addwithid(dlgcontrol *ctrl, dlgparam *dp,
                           char const *text, int id)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int msg, msg2, index;
    struct listbox_state *ls = NULL;
    char *display = NULL;
    const char *row = text;

    assert(c &&
           (c->ctrl->type == CTRL_LISTBOX ||
            (c->ctrl->type == CTRL_EDITBOX &&
             c->ctrl->editbox.has_list)));
    msg = (c->ctrl->type==CTRL_LISTBOX && c->ctrl->listbox.height!=0 ?
           LB_ADDSTRING : CB_ADDSTRING);
    msg2 = (c->ctrl->type==CTRL_LISTBOX && c->ctrl->listbox.height!=0 ?
            LB_SETITEMDATA : CB_SETITEMDATA);

    /*
     * PuTTY-beter: in a tree list box a row is indented and marked with
     * whether its folder is open, a row inside a closed folder is not added
     * at all, and the row goes where the tree puts it rather than at the end.
     * The id still goes with the row, so the shared code's idea of which row
     * is which is untouched; only the text and the order differ.
     */
    if (c->ctrl->type == CTRL_LISTBOX && c->ctrl->listbox.height != 0 &&
        c->ctrl->listbox.tree) {
        ls = listbox_state_of(dp, c);

        if (ls && listbox_tree_hidden(ls, text))
            return;

        display = listbox_tree_row_text(ls, text);
        row = display;
        msg = LB_INSERTSTRING;
    }

    if (msg == LB_INSERTSTRING) {
        int at = ls ? listbox_tree_insert_at(ls, text) : 0;

        index = SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, at,
                                   (LPARAM)row);
        if (ls)
            listbox_tree_add_path(ls, text, at);
    } else {
        index = SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, 0,
                                   (LPARAM)row);
    }

    SendDlgItemMessage(dp->hwnd, c->base_id+1, msg2, index, (LPARAM)id);

    sfree(display);
}

int dlg_listbox_getid(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int msg;
    assert(c && c->ctrl->type == CTRL_LISTBOX);
    msg = (c->ctrl->listbox.height != 0 ? LB_GETITEMDATA : CB_GETITEMDATA);
    return
        SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, index, 0);
}

/* dlg_listbox_index returns <0 if no single element is selected. */
int dlg_listbox_index(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int msg, ret;
    assert(c && c->ctrl->type == CTRL_LISTBOX);
    if (c->ctrl->listbox.multisel) {
        assert(c->ctrl->listbox.height != 0); /* not combo box */
        ret = SendDlgItemMessage(dp->hwnd, c->base_id+1, LB_GETSELCOUNT, 0, 0);
        if (ret == LB_ERR || ret > 1)
            return -1;
    }
    msg = (c->ctrl->listbox.height != 0 ? LB_GETCURSEL : CB_GETCURSEL);
    ret = SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, 0, 0);
    if (ret == LB_ERR)
        return -1;
    else
        return ret;
}

bool dlg_listbox_issel(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_LISTBOX &&
           c->ctrl->listbox.multisel &&
           c->ctrl->listbox.height != 0);
    return
        SendDlgItemMessage(dp->hwnd, c->base_id+1, LB_GETSEL, index, 0);
}

void dlg_listbox_select(dlgcontrol *ctrl, dlgparam *dp, int index)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int msg;
    assert(c && c->ctrl->type == CTRL_LISTBOX);

    /*
     * PuTTY-beter: a multi-selection list box needs different handling.
     * The saved-session list is multi-selection (so that several sessions
     * and folders can be dragged at once), and the shared configuration
     * code selects a row in it whenever a session name is typed or a
     * search is run. This function used to assert that the control was
     * *not* multi-selection, which upstream Windows could get away with
     * because nothing was - so on this fork it aborted as soon as a
     * session name was entered.
     *
     * LB_SETCURSEL alone would not do here: on a multi-selection box it does
     * not set the caret (and on an LBS_EXTENDEDSEL box it does nothing at
     * all). So clear every selection first, then select the one wanted row,
     * then move the caret - with LB_SETCARETINDEX, which is the message for
     * the caret in a multi-selection list box and also scrolls the row into
     * view, which is what a search hit needs. That keeps this function's
     * meaning the same as everywhere else - "the selection is now exactly
     * this row".
     */
    if (c->ctrl->listbox.multisel) {
        int n;

        assert(c->ctrl->listbox.height != 0);   /* not a combo box */

        n = SendDlgItemMessage(dp->hwnd, c->base_id+1, LB_GETCOUNT, 0, 0);
        for (int i = 0; i < n; i++)
            SendDlgItemMessage(dp->hwnd, c->base_id+1, LB_SETSEL, false, i);
        if (index >= 0 && index < n)
            SendDlgItemMessage(dp->hwnd, c->base_id+1, LB_SETSEL, true, index);
        /* SendDlgItemMessage(dp->hwnd, c->base_id+1, LB_SETCURSEL, index, 0); */
        SendDlgItemMessage(dp->hwnd, c->base_id+1, LB_SETCARETINDEX, index,
                           TRUE);
        return;
    }

    msg = (c->ctrl->listbox.height != 0 ? LB_SETCURSEL : CB_SETCURSEL);
    SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, index, 0);
}

/*
 * PuTTY-beter: every selected row's id, in row order. The saved-session list
 * is multi-selection, so several sessions and folders can be dragged into a
 * folder - or deleted - in one go, and the shared configuration code asks for
 * all of them. This used to return at most one, because this front end had no
 * multi-selection list boxes; it has one now.
 */
int dlg_listbox_selected_ids(dlgcontrol *ctrl, dlgparam *dp, int **ids)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int n, *rows;

    assert(c && c->ctrl->type == CTRL_LISTBOX);

    *ids = NULL;

    if (c->ctrl->listbox.height == 0) {
        /* A drop-down list can hold only one selection. */
        int id = dlg_listbox_selected_id(ctrl, dp);

        if (id < 0)
            return 0;

        *ids = snew(int);
        (*ids)[0] = id;
        return 1;
    }

    n = SendDlgItemMessage(dp->hwnd, c->base_id+1, LB_GETSELCOUNT, 0, 0);
    if (n == LB_ERR || n <= 0)
        return 0;

    rows = snewn(n, int);
    n = SendDlgItemMessage(dp->hwnd, c->base_id+1, LB_GETSELITEMS, n,
                           (LPARAM)rows);
    if (n == LB_ERR || n <= 0) {
        sfree(rows);
        return 0;
    }

    *ids = snewn(n, int);
    for (int i = 0; i < n; i++)
        (*ids)[i] = dlg_listbox_getid(ctrl, dp, rows[i]);

    sfree(rows);
    return n;
}

/*
 * PuTTY-beter: drag and drop between the rows of a list box.
 *
 * The saved-session list is a flat list in which a folder is an entry whose
 * name ends in '/', and the shared configuration code wants a row dragged on
 * to a folder row to mean "move it into that folder". It is told about the
 * drop through ctrl->listbox.drag_handler(), which is passed the id of the
 * row that was dropped on, or -1 for the top level. The GTK front end does
 * the same thing in unix/dialog.c, against a real tree.
 *
 * The box is subclassed rather than the dialog engine being taught about the
 * mouse: the drag is then invisible to everything else, and the list box
 * keeps its ordinary behaviour, because DefSubclassProc() still gets every
 * message that is not part of a drag. Windows gives the window that received
 * WM_LBUTTONDOWN the mouse until the button comes up, so no SetCapture() is
 * needed either.
 *
 * The one thing that needs care is the selection. Pressing a row in a
 * multi-selection list box does not necessarily select it - Windows applies
 * the click when the button comes *up* - and a drag never lets the button come
 * up over the box. So the selection is copied before the press is passed on,
 * and set again before the handler is called: either the whole of what was
 * selected, if the press was on one of those rows, or just the pressed row, if
 * it was not. That is the rule the GTK front end uses in
 * listbox_restore_selection(), and it is what makes "drag one of several" move
 * several while "drag a row on its own" moves one. The mouse moves are also
 * withheld from the list box while a drag is in progress, because on its own
 * they would extend the selection as the cursor travelled.
 */
struct listbox_state {
    struct dlgparam *dp;
    dlgcontrol *ctrl;
    HWND hwnd;
    bool pressed;                      /* left button went down on a row */
    bool dragging;                     /* and has moved far enough since */
    POINT press;                       /* where it went down, client coords */
    int press_row;                     /* the row it went down on */
    bool press_was_selected;           /* and was that row already selected? */
    int *press_rows;                   /* the selection as it was, by row */
    int npress_rows;
    int target;                        /* where a drop would go, -2 for unset */
    /*
     * The tree half: the full name of each row currently in the box, and the
     * folders the user has closed. `paths' is rebuilt by every refill;
     * `closed' deliberately survives one, because that is the whole point of
     * closing a folder.
     */
    char **paths;
    int npaths;
    char **closed;
    int nclosed;
};

static void listbox_drag_forget_rows(struct listbox_state *ld)
{
    sfree(ld->press_rows);
    ld->press_rows = NULL;
    ld->npress_rows = 0;
}

/* The row at a point in the box, or -1 if there is not one. */
static int listbox_row_at(struct listbox_state *ld, POINT pt)
{
    LRESULT hit;
    int row, outside, count;

    hit = SendMessage(ld->hwnd, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
    row = (int)LOWORD(hit);
    outside = (int)HIWORD(hit);
    count = (int)SendMessage(ld->hwnd, LB_GETCOUNT, 0, 0);

    if (row < 0 || outside || row >= count)
        return -1;

    return row;
}

/* Copy the selected rows, before the press is allowed to change them. */
static void listbox_drag_snapshot(struct listbox_state *ld)
{
    int n;

    listbox_drag_forget_rows(ld);

    n = (int)SendMessage(ld->hwnd, LB_GETSELCOUNT, 0, 0);
    if (n == LB_ERR || n <= 0)
        return;

    ld->press_rows = snewn(n, int);
    ld->npress_rows = (int)SendMessage(ld->hwnd, LB_GETSELITEMS, n,
                                       (LPARAM)ld->press_rows);
    if (ld->npress_rows == LB_ERR || ld->npress_rows <= 0)
        listbox_drag_forget_rows(ld);
}

/* Leave exactly one row selected, the way a plain click would. */
static void listbox_drag_select_one(struct listbox_state *ld, int row)
{
    int count = (int)SendMessage(ld->hwnd, LB_GETCOUNT, 0, 0);

    for (int i = 0; i < count; i++)
        SendMessage(ld->hwnd, LB_SETSEL, false, i);

    if (row >= 0 && row < count) {
        SendMessage(ld->hwnd, LB_SETSEL, true, row);
        SendMessage(ld->hwnd, LB_SETCARETINDEX, row, TRUE);
    }
}

/*
 * The selection the drop handler should act on: everything that was selected
 * when the drag began, if the drag began on one of those rows, or just the row
 * that was dragged if it began anywhere else.
 */
static void listbox_drag_restore(struct listbox_state *ld)
{
    if (ld->npress_rows > 1 && ld->press_was_selected)
        for (int i = 0; i < ld->npress_rows; i++)
            SendMessage(ld->hwnd, LB_SETSEL, true, ld->press_rows[i]);
    else
        listbox_drag_select_one(ld, ld->press_row);
}

/* The row holding a given id, or -1. */
static int listbox_row_for_id(struct listbox_state *ld, int id)
{
    int count = (int)SendMessage(ld->hwnd, LB_GETCOUNT, 0, 0);

    for (int i = 0; i < count; i++)
        if (dlg_listbox_getid(ld->ctrl, ld->dp, i) == id)
            return i;

    return -1;
}

/*
 * The full name a row stands for.
 *
 * A tree list box shows only the indented leaf of each name - "- Folder", not
 * "Folder/" - so the text in the control is not the name. The record kept
 * alongside the rows is. Without one (a list box that is not a tree) the text
 * *is* the name.
 */
static char *listbox_row_path(struct listbox_state *ld, int row)
{
    int len;
    char *text;

    if (row >= 0 && row < ld->npaths)
        return dupstr(ld->paths[row]);

    len = (int)SendMessage(ld->hwnd, LB_GETTEXTLEN, row, 0);
    if (len == LB_ERR || len < 0)
        return NULL;

    text = snewn(len + 1, char);
    SendMessage(ld->hwnd, LB_GETTEXT, row, (LPARAM)text);
    text[len] = '\0';

    return text;
}

/* The row whose name is exactly `name`, or -1. */
static int listbox_row_for_path(struct listbox_state *ld, const char *name)
{
    int count = (int)SendMessage(ld->hwnd, LB_GETCOUNT, 0, 0);

    for (int i = 0; i < count; i++) {
        char *text = listbox_row_path(ld, i);
        bool same = (text != NULL && !strcmp(text, name));

        sfree(text);
        if (same)
            return i;
    }

    return -1;
}

/*
 * What a drop at `pt` would mean: a folder row is the destination, a session
 * row means the folder that session lives in, and anywhere that is not a row
 * means the top level (-1).
 */
static int listbox_drop_target(struct listbox_state *ld, POINT pt)
{
    int row, count, id;
    char *path, *slash;

    row = listbox_row_at(ld, pt);
    if (row < 0)
        return -1;

    count = (int)SendMessage(ld->hwnd, LB_GETCOUNT, 0, 0);

    /*
     * A point in the empty space below the last row answers with that row,
     * so ask the last row where it really ends.
     */
    if (row == count - 1) {
        RECT r;

        if (SendMessage(ld->hwnd, LB_GETITEMRECT, row, (LPARAM)&r) == TRUE &&
            pt.y > r.bottom)
            return -1;
    }

    path = listbox_row_path(ld, row);
    if (!path)
        return -1;

    if (listbox_tree_is_folder(path)) {
        /* A folder: that is the destination itself. */
        id = dlg_listbox_getid(ld->ctrl, ld->dp, row);
        sfree(path);
        return id;
    }

    /*
     * A session: the destination is the folder it is in, which is its name up
     * to and including the last '/'. That folder may have no row of its own -
     * a search can filter it out, or it may be closed - in which case there is
     * nothing to name, and the drop goes to the top level.
     */
    slash = strrchr(path, '/');
    if (!slash) {
        sfree(path);
        return -1;
    }
    slash[1] = '\0';

    id = -1;
    row = listbox_row_for_path(ld, path);
    if (row >= 0)
        id = dlg_listbox_getid(ld->ctrl, ld->dp, row);

    sfree(path);
    return id;
}

/* Show where the drop would go, by moving the focus rectangle there. */
static void listbox_drag_show_target(struct listbox_state *ld, int target)
{
    int row;

    if (target == ld->target)
        return;

    if (target >= 0) {
        row = listbox_row_for_id(ld, target);
        if (row >= 0)
            SendMessage(ld->hwnd, LB_SETCARETINDEX, row, TRUE);
    }

    ld->target = target;
}

static LRESULT CALLBACK listbox_drag_proc(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam, UINT_PTR subclass,
                                          DWORD_PTR refdata)
{
    struct listbox_state *ld = (struct listbox_state *)refdata;

    switch (msg) {
      case WM_LBUTTONDOWN:
        /*
         * The selection is read before the box sees the press, because the
         * press is what changes it.
         */
        listbox_drag_snapshot(ld);
        ld->press.x = (short)LOWORD(lParam);
        ld->press.y = (short)HIWORD(lParam);
        ld->press_row = listbox_row_at(ld, ld->press);
        ld->press_was_selected = false;
        for (int i = 0; i < ld->npress_rows; i++)
            if (ld->press_rows[i] == ld->press_row)
                ld->press_was_selected = true;
        /* A press that is not on a row has nothing to drag. */
        ld->pressed = (ld->press_row >= 0);
        ld->dragging = false;
        ld->target = -2;
        break;

      case WM_MOUSEMOVE: {
        POINT pt;

        pt.x = (short)LOWORD(lParam);
        pt.y = (short)HIWORD(lParam);

        if (ld->pressed && !ld->dragging) {
            int dx = pt.x - ld->press.x;
            int dy = pt.y - ld->press.y;

            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;

            if (dx > GetSystemMetrics(SM_CXDRAG) ||
                dy > GetSystemMetrics(SM_CYDRAG))
                ld->dragging = true;
        }

        if (ld->dragging) {
            listbox_drag_show_target(ld, listbox_drop_target(ld, pt));
            /* Withheld from the box: see the comment on the state struct. */
            return 0;
        }
        break;
      }

      case WM_LBUTTONUP: {
        POINT pt;
        int target;

        if (!ld->dragging) {
            ld->pressed = false;
            listbox_drag_forget_rows(ld);
            break;
        }

        pt.x = (short)LOWORD(lParam);
        pt.y = (short)HIWORD(lParam);
        target = listbox_drop_target(ld, pt);

        ld->pressed = false;
        ld->dragging = false;
        listbox_drag_restore(ld);
        listbox_drag_forget_rows(ld);

        if (ld->ctrl->listbox.drag_handler)
            ld->ctrl->listbox.drag_handler(ld->ctrl, ld->dp, target);

        return 0;                  /* the drop was the point of the click */
      }

      case WM_SETCURSOR:
        if (ld->dragging) {
            SetCursor(LoadCursor(NULL, IDC_SIZEALL));
            return TRUE;
        }
        break;

      case WM_CAPTURECHANGED:
        /* The mouse went elsewhere, so whatever drag was running is over. */
        ld->dragging = false;
        break;

      case WM_NCDESTROY:
        listbox_drag_forget_rows(ld);
        listbox_tree_forget_paths(ld);
        for (int i = 0; i < ld->nclosed; i++)
            sfree(ld->closed[i]);
        sfree(ld->closed);
        if (p_RemoveWindowSubclass)
            p_RemoveWindowSubclass(hwnd, listbox_drag_proc, subclass);
        sfree(ld);
        break;
    }

    return p_DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void listbox_state_install(struct dlgparam *dp, dlgcontrol *ctrl,
                                 HWND hwnd, int id)
{
    struct listbox_state *ld;
    HWND list;

    if (!p_SetWindowSubclass || !p_RemoveWindowSubclass || !p_DefSubclassProc)
        return;                    /* this comctl32 cannot subclass */

    list = GetDlgItem(hwnd, id);
    if (!list)
        return;

    ld = snew(struct listbox_state);
    memset(ld, 0, sizeof(*ld));
    ld->dp = dp;
    ld->ctrl = ctrl;
    ld->hwnd = list;
    ld->target = -2;

    if (!p_SetWindowSubclass(list, listbox_drag_proc, 1, (DWORD_PTR)ld)) {
        sfree(ld);
        return;
    }

    /*
     * The dlg_listbox_* functions and the double-click handler need this
     * state too, and they only have the control, so leave a pointer to it on
     * the list box itself. Nothing else uses a list box's GWLP_USERDATA.
     */
    SetWindowLongPtr(list, GWLP_USERDATA, (LONG_PTR)ld);
}

/* The state installed above, or NULL if this control has none. */
static struct listbox_state *listbox_state_of(struct dlgparam *dp,
                                              struct winctrl *c)
{
    HWND list;

    if (!c || c->ctrl->type != CTRL_LISTBOX)
        return NULL;

    list = GetDlgItem(dp->hwnd, c->base_id+1);
    if (!list)
        return NULL;

    return (struct listbox_state *)GetWindowLongPtr(list, GWLP_USERDATA);
}

/*
 * PuTTY-beter: the saved-session list as a tree.
 *
 * A Windows list box is the only control here that can have several rows
 * selected at once, and several-at-once is what dragging a group into a folder
 * and deleting a group both need - so the tree is *flattened into rows* rather
 * than being a WC_TREEVIEW, which cannot multi-select at all. The shared
 * configuration code hands over one row per session, in the order
 * get_sesslist() produced (depth first, folders before sessions), and this is
 * where that becomes a tree: a row shows the last component of its name,
 * indented by its depth, with "- " for an open folder, "+ " for a closed one
 * and two spaces for a session. A row whose folder is closed is left out
 * altogether.
 *
 * Closing a folder is a front-end notion - the shared code knows nothing about
 * it - so the set of closed folders is kept here and the list is rebuilt with
 * dlg_refresh() when it changes. Folders start open, which is what the GTK side
 * does too (tree_expand() in unix/dialog.c).
 *
 * A double click on a folder row opens or closes it; see
 * listbox_tree_toggle(). A double click on a session row still loads it.
 */

#define LISTBOX_TREE_INDENT 3          /* spaces per level of nesting */

/* How deep a name sits: "A/" is 0, "A/B/" is 1, "A/B/c" is 2. */
static int listbox_tree_depth(const char *path)
{
    size_t len = strlen(path);
    int depth = 0;

    for (size_t i = 0; i < len; i++)
        if (path[i] == '/')
            depth++;

    if (len > 0 && path[len-1] == '/')
        depth--;                   /* a folder's own '/' is a marker, not a level */

    return depth < 0 ? 0 : depth;
}

/* The last component of a name: "A/B/" -> "B", "A/B/c" -> "c". */
static size_t listbox_tree_leaf(const char *path, const char **leafp)
{
    size_t len = strlen(path), start = 0;

    while (len > 0 && path[len-1] == '/')
        len--;

    for (size_t i = 0; i < len; i++)
        if (path[i] == '/')
            start = i + 1;

    *leafp = path + start;
    return len - start;
}

/* A name ending in '/' is a folder. */
static bool listbox_tree_is_folder(const char *path)
{
    size_t len = strlen(path);

    return len > 0 && path[len-1] == '/';
}

/*
 * Compare two names as entries of a tree: folders before sessions at every
 * level, then by name, with a folder's own entries directly after it.
 *
 * This is deliberately not sessioncmp(). That one only applies the
 * folders-before-sessions rule when both names end at the component being
 * compared, so "192.168.1.81" sorts after "zzone/" but *before* "zzone/zzmid"
 * - the entries of a folder do not stay together after all. Harmless for the
 * GTK front end, whose tree store groups rows by parent whatever order they
 * arrive in; wrong for this one, which shows rows in the order it is given
 * them, and would put a top-level session in the middle of a folder's
 * contents. So the rows are ordered here instead.
 */
static int listbox_tree_cmp(const char *a, const char *b)
{
    const char *pa = a, *pb = b;

    /* "Default Settings" is a special case and always comes first. */
    if (!strcmp(a, "Default Settings"))
        return -1;
    if (!strcmp(b, "Default Settings"))
        return +1;

    while (1) {
        const char *sa, *sb;
        size_t la, lb, l;
        int c;
        bool fa, fb;

        /*
         * A name that has run out is a folder the other one is inside, and a
         * folder comes before its own contents.
         */
        if (*pa == '\0' || *pb == '\0')
            return (*pa == '\0') ? ((*pb == '\0') ? 0 : -1) : +1;

        sa = strchr(pa, '/');
        sb = strchr(pb, '/');
        la = sa ? (size_t)(sa - pa) : strlen(pa);
        lb = sb ? (size_t)(sb - pb) : strlen(pb);
        l = la < lb ? la : lb;
        c = memcmp(pa, pb, l);

        /*
         * A component with a '/' after it is a folder: either the trailing
         * one that marks a folder, or one with more levels below it.
         */
        fa = (sa != NULL);
        fb = (sb != NULL);

        if (fa != fb)
            return fa ? -1 : +1;      /* the folder first */
        if (c)
            return c;
        if (la != lb)
            return la < lb ? -1 : +1;
        if (!sa || !sb)
            return 0;                 /* the same name */

        pa = sa + 1;
        pb = sb + 1;
    }
}

/* Where a name belongs among the rows already in the box. */
static int listbox_tree_insert_at(struct listbox_state *ls, const char *path)
{
    for (int i = 0; i < ls->npaths; i++)
        if (listbox_tree_cmp(path, ls->paths[i]) < 0)
            return i;

    return ls->npaths;
}

/* Is this path one of the folders the user has closed? */
static bool listbox_tree_closed(struct listbox_state *ls, const char *path)
{
    for (int i = 0; i < ls->nclosed; i++)
        if (!strcmp(ls->closed[i], path))
            return true;

    return false;
}

static void listbox_tree_set_closed(struct listbox_state *ls,
                                    const char *path, bool closed)
{
    for (int i = 0; i < ls->nclosed; i++) {
        if (strcmp(ls->closed[i], path))
            continue;

        if (closed)
            return;                /* already there */

        sfree(ls->closed[i]);
        ls->closed[i] = ls->closed[--ls->nclosed];
        return;
    }

    if (!closed)
        return;

    ls->closed = sresize(ls->closed, ls->nclosed + 1, char *);
    ls->closed[ls->nclosed++] = dupstr(path);
}

/* Was this row put in the box by the fill that is running now? */
static bool listbox_tree_shown(struct listbox_state *ls, const char *path)
{
    for (int i = 0; i < ls->npaths; i++)
        if (!strcmp(ls->paths[i], path))
            return true;

    return false;
}

/*
 * Is this row inside a folder that is closed? A folder that is not itself in
 * the list does not count: a search filters rows out, and a match inside a
 * closed folder must still be shown, or a search would come up empty.
 */
static bool listbox_tree_hidden(struct listbox_state *ls, const char *path)
{
    size_t len = strlen(path);

    for (size_t i = 0; i + 1 < len; i++) {
        char *anc;
        bool hidden;

        if (path[i] != '/')
            continue;

        /* path[0..i] is an ancestor folder's name, with its '/'. */
        anc = snewn(i + 2, char);
        memcpy(anc, path, i + 1);
        anc[i+1] = '\0';

        hidden = listbox_tree_shown(ls, anc) && listbox_tree_closed(ls, anc);
        sfree(anc);

        if (hidden)
            return true;
    }

    return false;
}

/* The text a row is displayed with: indent, marker, last component. */
static char *listbox_tree_row_text(struct listbox_state *ls, const char *path)
{
    const char *leaf;
    size_t leaflen = listbox_tree_leaf(path, &leaf);
    int depth = listbox_tree_depth(path);
    int at = depth * LISTBOX_TREE_INDENT;
    bool folder = listbox_tree_is_folder(path);
    char *out = snewn(at + 3 + leaflen, char);

    memset(out, ' ', at);
    out[at] = folder ? (listbox_tree_closed(ls, path) ? '+' : '-') : ' ';
    out[at + 1] = ' ';
    memcpy(out + at + 2, leaf, leaflen);
    out[at + 2 + leaflen] = '\0';

    return out;
}

/* Forget the rows, keeping the closed folders. */
static void listbox_tree_forget_paths(struct listbox_state *ls)
{
    for (int i = 0; i < ls->npaths; i++)
        sfree(ls->paths[i]);

    sfree(ls->paths);
    ls->paths = NULL;
    ls->npaths = 0;
}

/* Remember the full name of a row, at the row it was put in. */
static void listbox_tree_add_path(struct listbox_state *ls, const char *path,
                                  int at)
{
    if (at < 0 || at > ls->npaths)
        at = ls->npaths;

    ls->paths = sresize(ls->paths, ls->npaths + 1, char *);
    memmove(ls->paths + at + 1, ls->paths + at,
            (ls->npaths - at) * sizeof(char *));
    ls->paths[at] = dupstr(path);
    ls->npaths++;
}

/*
 * A double click on a folder row opens or closes it, and the list is rebuilt
 * from storage. Returns false if the double click was not on a folder, which
 * leaves the caller to do what it always did with it - load the session.
 */
static bool listbox_tree_toggle(struct dlgcontrol *ctrl, struct dlgparam *dp,
                               struct winctrl *c)
{
    struct listbox_state *ls = listbox_state_of(dp, c);
    HWND list = GetDlgItem(dp->hwnd, c->base_id+1);
    char *target;
    int row, n;

    if (!ls || !list)
        return false;

    row = (int)SendMessage(list, LB_GETCURSEL, 0, 0);
    if (row < 0 || row >= ls->npaths)
        return false;

    if (!listbox_tree_is_folder(ls->paths[row]))
        return false;              /* a session: not ours to handle */

    /* The refill below frees this, so keep a copy of it to look for. */
    target = dupstr(ls->paths[row]);

    listbox_tree_set_closed(ls, target, !listbox_tree_closed(ls, target));
    dlg_refresh(ctrl, dp);

    /* Leave the folder that was opened or closed selected, as before. */
    n = (int)SendMessage(list, LB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        if (i >= ls->npaths || strcmp(ls->paths[i], target))
            continue;

        SendMessage(list, LB_SETSEL, false, -1);   /* lParam -1: all rows */
        SendMessage(list, LB_SETSEL, true, i);
        SendMessage(list, LB_SETCARETINDEX, i, TRUE);
        break;
    }

    sfree(target);
    return true;
}

/*
 * PuTTY-beter: ask a question with a few buttons. This front end has no
 * equivalent of the GTK message box's button list, so the first button is
 * the one that is offered.
 */
void dlg_confirm(dlgcontrol *ctrl, dlgparam *dp,
                 const char *title, const char *msg,
                 const char *const *buttons, int nbuttons,
                 dlg_confirm_fn cb, void *ctx)
{
    if (cb)
        cb(ctx, 0);
}

/*
 * PuTTY-beter: selecting by id rather than row index, for tree list boxes.
 * This front end does not implement tree list boxes (it shows the entries as
 * a flat list), so an id is simply a row index here.
 */
int dlg_listbox_selected_id(dlgcontrol *ctrl, dlgparam *dp)
{
    int index = dlg_listbox_index(ctrl, dp);

    if (index < 0)
        return -1;
    return dlg_listbox_getid(ctrl, dp, index);
}

void dlg_listbox_select_id(dlgcontrol *ctrl, dlgparam *dp, int id)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int n, msg;
    assert(c && c->ctrl->type == CTRL_LISTBOX);

    msg = (c->ctrl->listbox.height != 0 ? LB_GETCOUNT : CB_GETCOUNT);
    n = SendDlgItemMessage(dp->hwnd, c->base_id+1, msg, 0, 0);

    for (int i = 0; i < n; i++) {
        if (dlg_listbox_getid(ctrl, dp, i) == id) {
            dlg_listbox_select(ctrl, dp, i);
            return;
        }
    }
}

void dlg_text_set(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_TEXT);
    SetDlgItemText(dp->hwnd, c->base_id, text);
}

void dlg_label_change(dlgcontrol *ctrl, dlgparam *dp, char const *text)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    char *escaped = NULL;
    int id = -1;

    assert(c);
    switch (c->ctrl->type) {
      case CTRL_EDITBOX:
        escaped = shortcut_escape(text, c->ctrl->editbox.shortcut);
        id = c->base_id;
        break;
      case CTRL_RADIO:
        escaped = shortcut_escape(text, c->ctrl->radio.shortcut);
        id = c->base_id;
        break;
      case CTRL_CHECKBOX:
        escaped = shortcut_escape(text, ctrl->checkbox.shortcut);
        id = c->base_id;
        break;
      case CTRL_BUTTON:
        escaped = shortcut_escape(text, ctrl->button.shortcut);
        id = c->base_id;
        break;
      case CTRL_LISTBOX:
        escaped = shortcut_escape(text, ctrl->listbox.shortcut);
        id = c->base_id;
        break;
      case CTRL_FILESELECT:
        escaped = shortcut_escape(text, ctrl->fileselect.shortcut);
        if (ctrl->fileselect.just_button)
            id = c->base_id + 2;       /* the button */
        else
            id = c->base_id;           /* the label */
        break;
      case CTRL_FONTSELECT:
        escaped = shortcut_escape(text, ctrl->fontselect.shortcut);
        id = c->base_id;
        break;
      default:
        unreachable("bad control type in label_change");
    }
    if (escaped) {
        SetDlgItemText(dp->hwnd, id, escaped);
        sfree(escaped);
    }
}

void dlg_filesel_set(dlgcontrol *ctrl, dlgparam *dp, Filename *fn)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c);
    assert(c->ctrl->type == CTRL_FILESELECT);
    assert(!c->ctrl->fileselect.just_button);
    SetDlgItemTextW(dp->hwnd, c->base_id+1, fn->wpath);
}

Filename *dlg_filesel_get(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c);
    assert(c->ctrl->type == CTRL_FILESELECT);
    if (!c->ctrl->fileselect.just_button) {
        wchar_t *tmp = GetDlgItemTextW_alloc(dp->hwnd, c->base_id+1);
        Filename *ret = filename_from_wstr(tmp);
        sfree(tmp);
        return ret;
    } else {
        return filename_copy(c->data);
    }
}

void dlg_fontsel_set(dlgcontrol *ctrl, dlgparam *dp, FontSpec *fs)
{
    char *buf, *boldstr;
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_FONTSELECT);

    fontspec_free((FontSpec *)c->data);
    c->data = fontspec_copy(fs);

    boldstr = (fs->isbold ? "bold, " : "");
    if (fs->height == 0)
        buf = dupprintf("Font: %s, %sdefault height", fs->name, boldstr);
    else
        buf = dupprintf("Font: %s, %s%d-%s", fs->name, boldstr,
                        (fs->height < 0 ? -fs->height : fs->height),
                        (fs->height < 0 ? "pixel" : "point"));
    SetDlgItemText(dp->hwnd, c->base_id+1, buf);
    sfree(buf);

    dlg_auto_set_fixed_pitch_flag(dp);
}

FontSpec *dlg_fontsel_get(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    assert(c && c->ctrl->type == CTRL_FONTSELECT);
    return fontspec_copy((FontSpec *)c->data);
}

/*
 * Bracketing a large set of updates in these two functions will
 * cause the front end (if possible) to delay updating the screen
 * until it's all complete, thus avoiding flicker.
 */
void dlg_update_start(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    if (c && c->ctrl->type == CTRL_LISTBOX) {
        SendDlgItemMessage(dp->hwnd, c->base_id+1, WM_SETREDRAW, false, 0);
    }
}

void dlg_update_done(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    if (c && c->ctrl->type == CTRL_LISTBOX) {
        HWND hw = GetDlgItem(dp->hwnd, c->base_id+1);
        SendMessage(hw, WM_SETREDRAW, true, 0);
        InvalidateRect(hw, NULL, true);
    }
}

void dlg_set_focus(dlgcontrol *ctrl, dlgparam *dp)
{
    struct winctrl *c = dlg_findbyctrl(dp, ctrl);
    int id;
    HWND ctl;
    if (!c)
        return;
    switch (ctrl->type) {
      case CTRL_EDITBOX: id = c->base_id + 1; break;
      case CTRL_RADIO:
        for (id = c->base_id + ctrl->radio.nbuttons; id > 1; id--)
            if (IsDlgButtonChecked(dp->hwnd, id))
                break;
        /*
         * In the theoretically-unlikely case that no button was
         * selected, id should come out of this as 1, which is a
         * reasonable enough choice.
         */
        break;
      case CTRL_CHECKBOX: id = c->base_id; break;
      case CTRL_BUTTON: id = c->base_id; break;
      case CTRL_LISTBOX: id = c->base_id + 1; break;
      case CTRL_FILESELECT: id = c->base_id + 1; break;
      case CTRL_FONTSELECT: id = c->base_id + 2; break;
      default: id = c->base_id; break;
    }
    ctl = GetDlgItem(dp->hwnd, id);
    SetFocus(ctl);
}

/*
 * During event processing, you might well want to give an error
 * indication to the user. dlg_beep() is a quick and easy generic
 * error; dlg_error() puts up a message-box or equivalent.
 */
void dlg_beep(dlgparam *dp)
{
    MessageBeep(0);
}

void dlg_error_msg(dlgparam *dp, const char *msg)
{
    MessageBox(dp->hwnd, msg,
               dp->errtitle ? dp->errtitle : NULL,
               MB_OK | MB_ICONERROR);
}

/*
 * This function signals to the front end that the dialog's
 * processing is completed, and passes an integer value (typically
 * a success status).
 */
void dlg_end(dlgparam *dp, int value)
{
    dp->ended = true;
    dp->endresult = value;
}

void dlg_refresh(dlgcontrol *ctrl, dlgparam *dp)
{
    int i, j;
    struct winctrl *c;

    if (!ctrl) {
        /*
         * Send EVENT_REFRESH to absolutely everything.
         */
        for (j = 0; j < dp->nctrltrees; j++) {
            for (i = 0;
                 (c = winctrl_findbyindex(dp->controltrees[j], i)) != NULL;
                 i++) {
                if (c->ctrl && c->ctrl->handler != NULL)
                    c->ctrl->handler(c->ctrl, dp,
                                     dp->data, EVENT_REFRESH);
            }
        }
    } else {
        /*
         * Send EVENT_REFRESH to a specific control.
         */
        if (ctrl->handler != NULL)
            ctrl->handler(ctrl, dp, dp->data, EVENT_REFRESH);
    }
}

void dlg_coloursel_start(dlgcontrol *ctrl, dlgparam *dp, int r, int g, int b)
{
    dp->coloursel_wanted = true;
    dp->coloursel_result.r = r;
    dp->coloursel_result.g = g;
    dp->coloursel_result.b = b;
}

bool dlg_coloursel_results(dlgcontrol *ctrl, dlgparam *dp,
                           int *r, int *g, int *b)
{
    if (dp->coloursel_result.ok) {
        *r = dp->coloursel_result.r;
        *g = dp->coloursel_result.g;
        *b = dp->coloursel_result.b;
        return true;
    } else
        return false;
}

void dlg_auto_set_fixed_pitch_flag(dlgparam *dp)
{
    Conf *conf = (Conf *)dp->data;
    FontSpec *fs;
    int quality;
    HFONT hfont;
    HDC hdc;
    TEXTMETRIC tm;
    bool is_var;

    /*
     * Attempt to load the current font, and see if it's
     * variable-pitch. If so, start off the fixed-pitch flag for the
     * dialog box as false.
     *
     * We assume here that any client of the dlg_* mechanism which is
     * using font selectors at all is also using a normal 'Conf *'
     * as dp->data.
     */

    quality = conf_get_int(conf, CONF_font_quality);
    fs = conf_get_fontspec(conf, CONF_font);

    hfont = CreateFont(0, 0, 0, 0, FW_DONTCARE, false, false, false,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, FONT_QUALITY(quality),
                       FIXED_PITCH | FF_DONTCARE, fs->name);
    hdc = GetDC(NULL);
    if (hdc && SelectObject(hdc, hfont) && GetTextMetrics(hdc, &tm)) {
        /* Note that the TMPF_FIXED_PITCH bit is defined upside down :-( */
        is_var = (tm.tmPitchAndFamily & TMPF_FIXED_PITCH);
    } else {
        is_var = false;                /* assume it's basically normal */
    }
    if (hdc)
        ReleaseDC(NULL, hdc);
    if (hfont)
        DeleteObject(hfont);

    if (is_var)
        dp->fixed_pitch_fonts = false;
}

bool dlg_get_fixed_pitch_flag(dlgparam *dp)
{
    return dp->fixed_pitch_fonts;
}

void dlg_set_fixed_pitch_flag(dlgparam *dp, bool flag)
{
    dp->fixed_pitch_fonts = flag;
}

void dp_init(struct dlgparam *dp)
{
    dp->nctrltrees = 0;
    dp->data = NULL;
    dp->ended = false;
    dp->focused = dp->lastfocused = NULL;
    memset(dp->shortcuts, 0, sizeof(dp->shortcuts));
    dp->hwnd = NULL;
    dp->wintitle = dp->errtitle = NULL;
    dp->fixed_pitch_fonts = true;
}

void dp_add_tree(struct dlgparam *dp, struct winctrls *wc)
{
    assert(dp->nctrltrees < lenof(dp->controltrees));
    dp->controltrees[dp->nctrltrees++] = wc;
}

void dp_cleanup(struct dlgparam *dp)
{
    sfree(dp->wintitle);
    sfree(dp->errtitle);
}
