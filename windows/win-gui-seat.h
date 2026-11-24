/*
 * Main state structure for an instance of the Windows PuTTY front
 * end, containing all the PuTTY objects and all the Windows API
 * resources like the window handle.
 */

typedef struct WinGuiSeat WinGuiSeat;

/*
 * PuTTY-beter: the MCP session, defined in mcp.h and driven from window.c.
 * Declared as a tag rather than a typedef so this header needs no include.
 */
struct McpSession;

/*
 * PuTTY-beter: the ZMODEM transfer engine, defined in zmodem.h and driven from
 * window.c. Declared as a tag for the same reason as McpSession.
 */
struct Zmodem;

struct PopupMenu {
    HMENU menu;
};
enum { SYSMENU, CTXMENU };             /* indices into popup_menus field */

#define FONT_NORMAL 0
#define FONT_BOLD 1
#define FONT_UNDERLINE 2
#define FONT_BOLDUND 3
#define FONT_WIDE       0x04
#define FONT_HIGH       0x08
#define FONT_NARROW     0x10

#define FONT_OEM        0x20
#define FONT_OEMBOLD    0x21
#define FONT_OEMUND     0x22
#define FONT_OEMBOLDUND 0x23

#define FONT_MAXNO      0x40
#define FONT_SHIFT      5

enum BoldMode {
    BOLD_NONE, BOLD_SHADOW, BOLD_FONT
};
enum UnderlineMode {
    UND_LINE, UND_FONT
};

struct _dpi_info {
    POINT cur_dpi;
    RECT new_wnd_rect;
};

/*
 * Against the future possibility of having more than one of these
 * in a process (and the current possibility of having zero), we
 * keep a linked list of all live WinGuiSeats, so that cleanups
 * can be done to any that exist.
 */
struct WinGuiSeatListNode {
    struct WinGuiSeatListNode *next, *prev;
};
extern struct WinGuiSeatListNode wgslisthead; /* static end pointer */

struct WinGuiSeat {
    struct WinGuiSeatListNode wgslistnode;

    Seat seat;
    TermWin termwin;
    LogPolicy logpolicy;

    HWND term_hwnd;

    int extra_width, extra_height;
    int font_width, font_height;
    bool font_dualwidth, font_varpitch;
    int offset_width, offset_height;
    bool was_zoomed;
    int prev_rows, prev_cols; // FIXME I don't think these are even used

    HBITMAP caretbm;
    int caret_x, caret_y;

    int kbd_codepage;

    Ldisc *ldisc;
    Backend *backend;

    cmdline_get_passwd_input_state cmdline_get_passwd_state;

    /*
     * PuTTY-beter: saved-session login details (tech_detail.md section 9).
     * sesspass_tried is the "one attempt per
     * connection" flag; the other three are what a successful login has to
     * offer to write into the session's file. See windows/window.c.
     */
    bool sesspass_tried;
    char *sesspass_used;            /* the password this login used */
    char *ssh_username_used;        /* the login name it used */
    bool sesspass_offered;          /* the "remember it?" offer has run */

    struct unicode_data ucsdata;
    bool session_closed;
    bool reconfiguring;

    /*
     * PuTTY-beter: the readable exit (tech_detail.md section 11). When the
     * session ends and CloseOnExit says the window goes, it is held for
     * CloseOnExitDelay seconds so that the last thing the session printed can
     * be read. `exit_countdown_left' is what is left of that hold, and 0 means
     * no countdown is running; `exit_status' is the line below the terminal
     * that says so. See windows/window.c.
     */
    int exit_countdown_left;
    HWND exit_status;

    const SessionSpecial *specials;
    HMENU specials_menu;
    int n_specials;

    struct PopupMenu popup_menus[2];
    HMENU savedsess_menu;

    Conf *conf;
    LogContext *logctx;
    Terminal *term;

    /*
     * PuTTY-beter: this window's MCP session, if it is offered over MCP at
     * all. NULL unless McpEnabled was set for the session, and NULL again once
     * the session has closed. See windows/mcp.c.
     */
    struct McpSession *mcp;

    /*
     * PuTTY-beter: ZMODEM file transfer (tech_detail.md section 12). The
     * engine exists for the whole life of the session, because it has to sit in
     * the incoming stream from the start - that is where a transfer is
     * detected. Until one begins it does nothing but pass bytes through, so an
     * idle engine is byte-identical to upstream. The rest is its front-end
     * state. See windows/window.c.
     */
    struct Zmodem *zmodem;
    bool zm_in_engine;              /* zmodem_feed_in() is on the stack */
    unsigned char *zm_pending;      /* bytes that arrived while it was */
    size_t zm_pendinglen, zm_pendingalloc;
    FILE *zm_file;                  /* the local file in use, if any */
    Filename *zm_path;              /* its path, for deleting a partial one */
    HWND zm_dialog;                 /* the progress window, NULL if none */
    bool zm_receive;                /* direction, for the dialog's wording */
    bool zm_sent_one;               /* a file has been sent in this transfer */
    uint64_t zm_total;

    int cursor_type;
    int vtmode;

    HFONT fonts[FONT_MAXNO];
    LOGFONT lfont;
    bool fontflag[FONT_MAXNO];
    enum BoldMode bold_font_mode;

    bool bold_colours;
    enum UnderlineMode und_mode;
    int descent, font_strikethrough_y;

    COLORREF colours[OSC4_NCOLOURS];
    HPALETTE pal;
    LPLOGPALETTE logpal;
    bool tried_pal;
    COLORREF colorref_modifier;

    struct _dpi_info dpi_info;

    int dbltime, lasttime, lastact;
    Mouse_Button lastbtn;

    bool send_raw_mouse;
    int wheel_accumulator;

    bool pointer_indicates_raw_mouse;

    BusyStatus busy_status;

    wchar_t *window_name, *icon_name;

    int alt_numberpad_accumulator;
    int compose_state;
    int compose_char;
    WPARAM compose_keycode;

    HDC wintw_hdc;

    bool resizing;
    bool need_backend_resize;

    long next_flash;
    bool flashing;

    long last_beep_time;

    bool ignore_clip;
    bool fullscr_on_max;
    bool processed_resize;
    bool in_scrollbar_loop;
    UINT last_mousemove;
    WPARAM last_wm_mousemove_wParam, last_wm_ncmousemove_wParam;
    LPARAM last_wm_mousemove_lParam, last_wm_ncmousemove_lParam;
    wchar_t pending_surrogate;
};

extern const LogPolicyVtable win_gui_logpolicy_vt; /* in dialog.c */

static inline void wgs_link(WinGuiSeat *wgs)
{
    wgs->wgslistnode.prev = wgslisthead.prev;
    wgs->wgslistnode.next = &wgslisthead;
    wgs->wgslistnode.prev->next = &wgs->wgslistnode;
    wgs->wgslistnode.next->prev = &wgs->wgslistnode;
}

static inline void wgs_unlink(WinGuiSeat *wgs)
{
    wgs->wgslistnode.prev->next = wgs->wgslistnode.next;
    wgs->wgslistnode.next->prev = wgs->wgslistnode.prev;
}
