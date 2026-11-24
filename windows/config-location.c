/*
 * config-location.c: choose where PuTTY-beter keeps its configuration.
 *
 * The Windows twin of unix/config-location.c. PuTTY-beter keeps saved
 * sessions, host keys, CA records, the random seed and its settings in one
 * folder - %USERPROFILE%\.putty by default, or whatever a config-location.conf
 * redirect file in that folder names. This offers to move that folder somewhere
 * more convenient.
 *
 * The dialog appears under exactly the same three conditions as on macOS:
 *
 *   - the configuration is missing (no sessions subdirectory yet), or
 *   - the user held Alt while launching PuTTY, or
 *   - PUTTY_CONFIG_RELOCATE=1 is set in the environment.
 *
 * Alt is the same physical key as the Mac's Option key, which is what
 * unix/config-location.c tests there, so the two front ends behave alike.
 *
 * Set PUTTY_CONFIG_DEBUG=1 to trace what happens to stderr.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <shlobj.h>

#include "putty.h"
#include "putty-rc.h"

static bool cfg_debug(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = (getenv("PUTTY_CONFIG_DEBUG") != NULL);
    return cached != 0;
}

/*
 * Was Alt held when this process started? Alt is the PC's Option key, which is
 * what the macOS front end looks for. GetAsyncKeyState is used rather than the
 * window message state because no key message will have been delivered yet
 * this early in startup.
 */
static bool alt_key_held(void)
{
    return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

/* UTF-8 <-> UTF-16, so a non-ASCII folder name survives. */
static wchar_t *cl_wpath(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w;

    if (n <= 0)
        return NULL;
    w = smalloc((size_t)n * sizeof(wchar_t));
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) {
        sfree(w);
        return NULL;
    }
    return w;
}

static char *cl_utf8(const wchar_t *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s;

    if (n <= 0)
        return NULL;
    s = smalloc((size_t)n);
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) {
        sfree(s);
        return NULL;
    }
    return s;
}

/*
 * PuTTY-beter: write a path the way a Windows user writes one.
 *
 * The shared storage layer builds paths POSIX-style, with '/' separators, so
 * that a configuration folder looks the same on a Mac and a PC, and Windows
 * accepts either separator. But this dialog and the configuration box *show*
 * the path to the user, and "C:\Users\me/Documents/..." with both kinds of
 * slash in it is just untidy. Rewriting in place is safe: the string is one we
 * own, and any backslash already in it (a drive letter, a UNC prefix) is left
 * alone.
 */
static void cl_native_path(char *path)
{
    for (; *path; path++)
        if (*path == '/')
            *path = '\\';
}

static void cl_set_text(HWND hwnd, int id, const char *utf8)
{
    wchar_t *w = cl_wpath(utf8 ? utf8 : "");

    if (w) {
        SetDlgItemTextW(hwnd, id, w);
        sfree(w);
    }
}

static char *cl_get_text(HWND hwnd, int id)
{
    int len = GetWindowTextLengthW(GetDlgItem(hwnd, id));
    wchar_t *w = smalloc((len + 1) * sizeof(wchar_t));
    char *ret;

    GetDlgItemTextW(hwnd, id, w, len + 1);
    ret = cl_utf8(w);
    sfree(w);
    return ret ? ret : dupstr("");
}

static void cl_error(HWND hwnd, const char *msg)
{
    wchar_t *w = cl_wpath(msg);

    MessageBoxW(hwnd, w ? w : L"Unable to change the configuration folder",
                L"PuTTY configuration folder", MB_OK | MB_ICONERROR);
    sfree(w);
}

/*
 * The folder chooser. SHBrowseForFolder is used rather than the newer
 * IFileDialog because it needs no COM set-up beyond what the shell already
 * does, and this dialog is shown before the rest of the application starts.
 */
static char *browse_for_folder(HWND owner)
{
    BROWSEINFOW bi;
    wchar_t display[MAX_PATH];
    LPITEMIDLIST pidl;
    char *ret = NULL;

    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = owner;
    bi.pszDisplayName = display;
    bi.lpszTitle = L"Choose the folder to keep the configuration in";
    bi.ulFlags = BIF_RETURNONLYFSDIRS;
#ifdef BIF_NEWDIALOGSTYLE
    bi.ulFlags |= BIF_NEWDIALOGSTYLE;   /* resizable, with a new-folder button */
#endif

    pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];

        if (SHGetPathFromIDListW(pidl, path))
            ret = cl_utf8(path);
        CoTaskMemFree(pidl);
    }
    return ret;
}

struct cl_state {
    char *msg;
    char *hint;
    char *initial;             /* what the path box starts with */
    char *typed;               /* what the user left in it, if they accepted */
};

static INT_PTR CALLBACK cl_dlgproc(HWND hwnd, UINT msg,
                                   WPARAM wParam, LPARAM lParam)
{
    struct cl_state *st =
        (struct cl_state *)GetWindowLongPtr(hwnd, GWLP_USERDATA);

    switch (msg) {
      case WM_INITDIALOG:
        st = (struct cl_state *)lParam;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)st);
        cl_set_text(hwnd, IDC_CL_MSG, st->msg);
        cl_set_text(hwnd, IDC_CL_HINT, st->hint);
        cl_set_text(hwnd, IDC_CL_PATH, st->initial);
        centre_window(hwnd);
        SetForegroundWindow(hwnd);
        SetFocus(GetDlgItem(hwnd, IDC_CL_PATH));
        return true;           /* we set the focus ourselves */

      case WM_COMMAND:
        if (!st)
            return false;
        switch (LOWORD(wParam)) {
          case IDC_CL_BROWSE: {
            char *chosen = browse_for_folder(hwnd);
            if (chosen) {
                cl_set_text(hwnd, IDC_CL_PATH, chosen);
                sfree(chosen);
            }
            return true;
          }
          case IDOK:
            /* Keep the text: the window is gone once EndDialog returns. */
            sfree(st->typed);
            st->typed = cl_get_text(hwnd, IDC_CL_PATH);
            EndDialog(hwnd, IDOK);
            return true;
          case IDCANCEL:
            EndDialog(hwnd, IDCANCEL);
            return true;
        }
        return false;

      case WM_CLOSE:
        EndDialog(hwnd, IDCANCEL);
        return true;
    }
    return false;
}

/*
 * The relocation dialog. Returns true if the folder was changed.
 *
 * If the user cancels, the current folder is recorded as configured anyway, so
 * that "first run" really is a one-off rather than something that reappears on
 * every launch.
 */
static bool ask_dialog(bool forced)
{
    char *current = config_location_current();
    char *base = config_location_base();
    char *initial = config_location_is_redirected()
        ? dupstr(current) : config_location_default_dir();
    struct cl_state st;
    bool changed = false;

    if (!initial)
        initial = dupstr(current);

    /*
     * Show the paths the way Windows writes them - the storage layer builds
     * them with '/', which would otherwise appear alongside the '\' of the
     * drive letter.
     */
    cl_native_path(current);
    cl_native_path(base);
    cl_native_path(initial);

    memset(&st, 0, sizeof(st));
    st.msg = dupprintf(
        "PuTTY-beter keeps saved sessions, host keys and settings in one "
        "folder.\r\n\r\n"
        "Folder in use now:  %s\r\n\r\n"
        "Choose the folder to use from now on. Anything already saved will be "
        "moved there. Nothing is deleted, and nothing is overwritten: if the "
        "new folder already contains one of these files, the old copy is left "
        "where it is.", current);
    st.hint = dupstr(
        forced
        ? "Opened because Alt was held at launch, or because "
          "PUTTY_CONFIG_RELOCATE is set."
        : "To change this later, hold Alt while launching PuTTY, or set "
          "PUTTY_CONFIG_RELOCATE=1.");
    st.initial = initial;

    for (;;) {
        INT_PTR r = DialogBoxParam(GetModuleHandle(NULL),
                                   MAKEINTRESOURCE(IDD_CONFIG_LOCATION),
                                   NULL, cl_dlgproc, (LPARAM)&st);
        char *err;

        if (r != IDOK)
            break;             /* "Keep Current", Esc, or the close box */

        err = config_location_set(st.typed ? st.typed : st.initial);
        if (!err) {
            changed = true;
            break;
        }

        if (cfg_debug())
            fprintf(stderr, "[config-location] %s\n", err);

        cl_error(NULL, err);
        sfree(err);

        /* Try again, starting from what they typed. */
        sfree(st.initial);
        st.initial = st.typed ? st.typed : dupstr(current);
        st.typed = NULL;
    }

    if (!changed) {
        /*
         * Remember the current folder so we do not ask again. This also
         * creates the sessions subdirectory that marks the configuration as
         * present.
         */
        char *err = config_location_set(current);
        if (err) {
            if (cfg_debug())
                fprintf(stderr, "[config-location] keeping %s failed: %s\n",
                        current, err);
            sfree(err);
        }
    }

    if (cfg_debug())
        fprintf(stderr, "[config-location] dialog done: base=%s current=%s "
                "changed=%d\n", base, config_location_current(), changed);

    sfree(st.typed);
    sfree(st.initial);
    sfree(st.hint);
    sfree(st.msg);
    sfree(base);
    sfree(current);

    return changed;
}

/*
 * Make sure <configuration folder>/logs exists, so that the default log file
 * name (see platform_default_filename in windows/utils/defaults.c) can be
 * opened. Failure is not fatal: logging just reports the error when it is
 * used.
 */
static void ensure_logs_dir(void)
{
    char *dir = config_location_current();
    char *logs = dupprintf("%s/logs", dir);
    wchar_t *w;

    cl_native_path(logs);
    w = cl_wpath(logs);

    if (w) {
        if (!CreateDirectoryW(w, NULL) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            if (cfg_debug())
                fprintf(stderr, "[config-location] cannot create %s (%lu)\n",
                        logs, (unsigned long)GetLastError());
        }
        sfree(w);
    }

    sfree(logs);
    sfree(dir);
}

/*
 * Handed to windows/utils/defaults.c so that default file names can be put in
 * the configuration folder - which only the front end knows about, and which
 * may have been redirected.
 */
static char *default_filename_dir(void)
{
    return config_location_current();
}

void config_location_init(void)
{
    static bool already_done = false;
    const char *env;
    bool forced, configured;

    if (already_done)
        return;
    already_done = true;

    env = getenv("PUTTY_CONFIG_RELOCATE");
    forced = (env && *env && strcmp(env, "0") != 0);
    if (!forced)
        forced = alt_key_held();

    configured = config_location_is_configured();

    if (cfg_debug()) {
        char *cur = config_location_current();
        fprintf(stderr, "[config-location] init: current=%s configured=%d "
                "forced=%d\n", cur, configured, forced);
        sfree(cur);
    }

    /*
     * From here on, default file names belong in the configuration folder.
     * This is done before the caller reads a Conf, because the log file name
     * is a Conf default.
     */
    platform_default_filename_dir = default_filename_dir;

    if (!forced && configured) {
        ensure_logs_dir();
        return;
    }

    ask_dialog(forced);
    ensure_logs_dir();
}
