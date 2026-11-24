/*
 * storage-win32-compat.h: let unix/storage.c build on Windows.
 *
 * PuTTY-beter keeps its configuration in a folder rather than the registry, on
 * every platform. unix/storage.c is the file-based implementation: one .conf
 * file per saved session, folders as real directories, a host-keys file, one
 * file per host CA, the random seed, the `config-location.conf' redirect, and
 * the small key=value state files. That layout is deliberately identical on
 * all platforms so a configuration folder can be copied between a Mac and a
 * PC unchanged.
 *
 * Rather than write a second, registry-backed implementation for Windows and
 * risk the two drifting apart, Windows compiles unix/storage.c too. This
 * header supplies the handful of POSIX things mingw does not have.
 *
 * Measured 2026-11-23 against mingw-w64 UCRT64 gcc 16.2.0, mingw already
 * provides open/read/write/close, stat, access, chmod, rename, unlink, rmdir,
 * struct stat, mode_t, ssize_t, PATH_MAX and even dirent.h. What is missing,
 * and what this file supplies, is:
 *
 *   mkdir(path, mode)   mingw's mkdir takes one argument
 *   lstat               no symlinks to worry about here, so it is stat
 *   symlink, readlink   only used by the relocation copy, which cannot see
 *                       symlinks on Windows; they fail cleanly
 *   getuid, getpwuid    %USERPROFILE% stands in for the home directory
 *   mkstemp             _mktemp_s plus CREATE_NEW
 *   make_dir_path       a unix-only utility (unix/utils/make_dir_path.c)
 *   dirent              opendir/readdir/closedir built on FindFirstFileW
 *
 * PATHS ARE UTF-8. mingw's open/stat/opendir are ANSI, which would mangle any
 * non-ASCII session name on a non-English Windows, so every path is converted
 * UTF-8 -> UTF-16 here and the wide entry points are called. (The alternative,
 * declaring the process's active code page to be UTF-8 in the manifest, is not
 * used: it would change GetACP() for the whole process, which a terminal
 * emulator must not have changed underneath it.)
 *
 * Windows accepts '/' as a path separator in these APIs, so unix/storage.c's
 * "%s/%s" path building needs no change.
 *
 * INCLUDE THIS AFTER "putty.h" - it uses smalloc/sfree.
 */

#ifndef PUTTY_STORAGE_WIN32_COMPAT_H
#define PUTTY_STORAGE_WIN32_COMPAT_H

#ifdef _WIN32

#include <windows.h>
#include <io.h>
#include <direct.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * mingw does not define S_ISLNK. Nothing here can produce one either: a
 * Windows configuration folder has no symlinks in it, so the branch in
 * config_copy_path() that tests for one is simply never taken.
 */
#ifndef S_ISLNK
#define S_ISLNK(mode) 0
#endif

/*
 * unix/storage.c normally falls back to the X resource database for a setting
 * that a session does not define (see provide_xrm_string). There is no X
 * resource database on Windows, so that fallback is compiled out there instead
 * of being stubbed here - see get_setting() in unix/storage.c.
 */

/*
 * setenv/unsetenv are used by test/test_sessions.c, which exercises this
 * storage layer directly, rather than by the storage code itself. (mkdtemp is
 * used by the same test but mingw already provides it.)
 */
static int setenv(const char *name, const char *value, int overwrite)
{
    if (!overwrite && getenv(name))
        return 0;
    return _putenv_s(name, value);     /* an empty value removes it */
}

static int unsetenv(const char *name)
{
    return _putenv_s(name, "");
}

/* ------------------------------------------------------------------ */
/* UTF-8 <-> UTF-16                                                     */
/* ------------------------------------------------------------------ */

/* A path as UTF-16, or NULL (with errno set) if it cannot be converted. */
static wchar_t *putty_win_wpath(const char *path)
{
    int need;
    wchar_t *w;

    if (!path) {
        errno = EINVAL;
        return NULL;
    }

    need = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (need <= 0) {
        errno = EINVAL;
        return NULL;
    }

    w = smalloc((size_t)need * sizeof(wchar_t));
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, need)) {
        sfree(w);
        errno = EINVAL;
        return NULL;
    }
    return w;
}

/* A filename as UTF-8, into a caller-supplied buffer. */
static void putty_win_u8path(const wchar_t *w, char *out, size_t outlen)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)outlen, NULL, NULL))
        out[0] = '\0';
}

/* Turn a Win32 error into something strerror() can report. */
static void putty_win_seterr(DWORD err)
{
    switch (err) {
      case ERROR_FILE_NOT_FOUND:
      case ERROR_PATH_NOT_FOUND:
        errno = ENOENT;
        break;
      case ERROR_ACCESS_DENIED:
        errno = EACCES;
        break;
      case ERROR_ALREADY_EXISTS:
      case ERROR_FILE_EXISTS:
        errno = EEXIST;
        break;
      case ERROR_DIR_NOT_EMPTY:
        errno = ENOTEMPTY;
        break;
      case ERROR_DISK_FULL:
        errno = ENOSPC;
        break;
      default:
        errno = EINVAL;
        break;
    }
}

/* ------------------------------------------------------------------ */
/* The home directory, getuid and getpwuid                              */
/* ------------------------------------------------------------------ */

struct passwd {
    char *pw_name;
    char *pw_dir;
    unsigned int pw_uid;
    unsigned int pw_gid;
};

/*
 * %USERPROFILE% is what $HOME is on Windows. getenv("HOME") is honoured first
 * so that an explicit HOME in the environment still works (some shells set
 * one), and so that the behaviour matches unix/storage.c exactly when it is.
 */
static const char *putty_win_home(void)
{
    const char *home = getenv("HOME");
    const char *prof;

    if (home && *home)
        return home;

    prof = getenv("USERPROFILE");
    if (prof && *prof)
        return prof;

    return NULL;
}

static unsigned int getuid(void)
{
    return 0;                          /* one user per session on Windows */
}

static struct passwd *getpwuid(unsigned int uid)
{
    static struct passwd pw;
    static char *cached;

    if (!cached) {
        const char *home = putty_win_home();
        cached = dupstr(home ? home : ".");
    }

    pw.pw_name = (char *)"user";
    pw.pw_dir = cached;
    pw.pw_uid = pw.pw_gid = uid;
    return &pw;
}

/* ------------------------------------------------------------------ */
/* The filesystem calls                                                 */
/* ------------------------------------------------------------------ */

static int putty_win_mkdir(const char *path, int mode)
{
    wchar_t *w = putty_win_wpath(path);
    int ret;
    (void)mode;                        /* Windows has no directory mode bits */
    if (!w)
        return -1;
    ret = _wmkdir(w);
    sfree(w);
    return ret;
}

static int putty_win_stat(const char *path, struct stat *st)
{
    wchar_t *w = putty_win_wpath(path);
    struct _stat64 w64;
    int ret;

    if (!w)
        return -1;
    ret = _wstat64(w, &w64);
    sfree(w);
    if (ret != 0)
        return -1;

    memset(st, 0, sizeof(*st));
    st->st_dev = (dev_t)w64.st_dev;
    st->st_ino = (ino_t)w64.st_ino;
    st->st_mode = (mode_t)w64.st_mode;
    st->st_nlink = (short)w64.st_nlink;
    st->st_uid = (short)w64.st_uid;
    st->st_gid = (short)w64.st_gid;
    st->st_rdev = (dev_t)w64.st_rdev;
    st->st_size = (off_t)w64.st_size;
    st->st_atime = (time_t)w64.st_atime;
    st->st_mtime = (time_t)w64.st_mtime;
    st->st_ctime = (time_t)w64.st_ctime;
    return 0;
}

static int putty_win_unlink(const char *path)
{
    wchar_t *w = putty_win_wpath(path);
    int ret;
    if (!w)
        return -1;
    ret = _wunlink(w);
    sfree(w);
    return ret;
}

static int putty_win_rmdir(const char *path)
{
    wchar_t *w = putty_win_wpath(path);
    int ret;
    if (!w)
        return -1;
    ret = _wrmdir(w);
    sfree(w);
    return ret;
}

static int putty_win_rename(const char *oldpath, const char *newpath)
{
    wchar_t *a = putty_win_wpath(oldpath), *b = putty_win_wpath(newpath);
    int ret;

    if (!a || !b) {
        sfree(a); sfree(b);
        return -1;
    }
    /* MoveFileEx with REPLACE_EXISTING is what rename(2) means. */
    if (MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING)) {
        ret = 0;
    } else {
        putty_win_seterr(GetLastError());
        ret = -1;
    }
    sfree(a); sfree(b);
    return ret;
}

static int putty_win_access(const char *path, int mode)
{
    wchar_t *w = putty_win_wpath(path);
    int ret;
    if (!w)
        return -1;
    (void)mode;                        /* only F_OK is used by storage.c */
    ret = _waccess(w, 0);
    sfree(w);
    return ret;
}

static int putty_win_chmod(const char *path, int mode)
{
    wchar_t *w = putty_win_wpath(path);
    int ret;
    if (!w)
        return -1;
    ret = _wchmod(w, mode);
    sfree(w);
    return ret;
}

/* open() is variadic: two or three arguments. */
static int putty_win_open(const char *path, int flags, ...)
{
    wchar_t *w = putty_win_wpath(path);
    int mode = 0, ret;

    if (!w)
        return -1;
    if (flags & (_O_CREAT | _O_TEMPORARY)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    ret = _wopen(w, flags, mode);
    sfree(w);
    return ret;
}

static int putty_win_symlink(const char *target, const char *linkpath)
{
    (void)target; (void)linkpath;
    errno = ENOSYS;                    /* not reachable from a Windows config */
    return -1;
}

static ssize_t putty_win_readlink(const char *path, char *buf, size_t bufsiz)
{
    (void)path; (void)buf; (void)bufsiz;
    errno = EINVAL;
    return -1;
}

static int putty_win_mkstemp(char *tmpl)
{
    wchar_t *w;
    HANDLE h;
    int fd;

    if (_mktemp_s(tmpl, strlen(tmpl) + 1) != 0) {
        errno = EEXIST;
        return -1;
    }

    w = putty_win_wpath(tmpl);
    if (!w)
        return -1;

    h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    sfree(w);
    if (h == INVALID_HANDLE_VALUE) {
        putty_win_seterr(GetLastError());
        return -1;
    }

    fd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
    if (fd < 0) {
        CloseHandle(h);
        errno = EMFILE;
        return -1;
    }
    return fd;
}

/*
 * unix/utils/make_dir_path.c, reimplemented here: create a directory and any
 * missing parents, returning NULL on success or a message for the user.
 */
static char *make_dir_path(const char *path, int mode)
{
    char *copy = dupstr(path);
    size_t len = strlen(copy);
    char *ret = NULL;

    /* Walk the path creating each component in turn. */
    for (size_t i = 1; i <= len; i++) {
        if (copy[i] != '/' && copy[i] != '\\' && copy[i] != '\0')
            continue;

        char saved = copy[i];
        copy[i] = '\0';

        /* Skip a drive letter ("C:") and a UNC prefix. */
        if (!(i == 2 && copy[1] == ':') && copy[0] != '\0') {
            if (putty_win_mkdir(copy, mode) != 0) {
                DWORD err = GetLastError();
                struct stat st;
                /* Already there is fine; anything else is not. */
                if (!(err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) ||
                    putty_win_stat(copy, &st) != 0 || !S_ISDIR(st.st_mode)) {
                    putty_win_seterr(err);
                    ret = dupprintf("Unable to create directory %s: %s",
                                    copy, strerror(errno));
                    copy[i] = saved;
                    break;
                }
            }
        }

        copy[i] = saved;
    }

    sfree(copy);
    return ret;
}

/* ------------------------------------------------------------------ */
/* dirent                                                               */
/* ------------------------------------------------------------------ */

struct dirent {
    char d_name[260];
};

typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAW fd;
    bool pending;                      /* fd holds a valid entry to return */
    bool finished;
    struct dirent de;
} DIR;

static DIR *opendir(const char *path)
{
    wchar_t *w, *pattern;
    size_t n;
    DIR *d;

    w = putty_win_wpath(path);
    if (!w) {
        errno = ENOENT;
        return NULL;
    }

    n = wcslen(w);
    pattern = smalloc((n + 3) * sizeof(wchar_t));
    memcpy(pattern, w, n * sizeof(wchar_t));
    if (n && (w[n-1] == L'/' || w[n-1] == L'\\')) {
        pattern[n] = L'*';
        pattern[n+1] = L'\0';
    } else {
        pattern[n] = L'\\';
        pattern[n+1] = L'*';
        pattern[n+2] = L'\0';
    }
    sfree(w);

    d = smalloc(sizeof(DIR));
    memset(d, 0, sizeof(*d));
    d->handle = FindFirstFileW(pattern, &d->fd);
    sfree(pattern);

    if (d->handle == INVALID_HANDLE_VALUE) {
        putty_win_seterr(GetLastError());
        sfree(d);
        return NULL;
    }

    d->pending = true;
    return d;
}

static struct dirent *readdir(DIR *d)
{
    if (!d || d->finished)
        return NULL;

    if (!d->pending) {
        if (!FindNextFileW(d->handle, &d->fd)) {
            d->finished = true;
            return NULL;
        }
    }
    d->pending = false;

    putty_win_u8path(d->fd.cFileName, d->de.d_name, sizeof(d->de.d_name));
    return &d->de;
}

static int closedir(DIR *d)
{
    if (!d)
        return -1;
    if (d->handle != INVALID_HANDLE_VALUE)
        FindClose(d->handle);
    sfree(d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Map the POSIX names onto the shims                                   */
/* ------------------------------------------------------------------ */

/*
 * These are function-like macros, so they only expand where a call is
 * intended. In particular `struct stat st;' is untouched, because `stat' there
 * is not followed by '('.
 */
#define mkdir(path, mode)      putty_win_mkdir(path, mode)
#define stat(path, st)         putty_win_stat(path, st)
#define lstat(path, st)        putty_win_stat(path, st)
#define unlink(path)           putty_win_unlink(path)
#define rmdir(path)            putty_win_rmdir(path)
#define rename(oldp, newp)     putty_win_rename(oldp, newp)
#define access(path, mode)     putty_win_access(path, mode)
#define chmod(path, mode)      putty_win_chmod(path, mode)
#define open(...)              putty_win_open(__VA_ARGS__)
#define symlink(t, l)          putty_win_symlink(t, l)
#define readlink(p, b, n)      putty_win_readlink(p, b, n)
#define mkstemp(tmpl)          putty_win_mkstemp(tmpl)

#endif /* _WIN32 */

#endif /* PUTTY_STORAGE_WIN32_COMPAT_H */
