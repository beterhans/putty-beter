/*
 * storage.c: Unix-specific implementation of the interface defined
 * in storage.h.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include "putty.h"
#include "storage.h"
#include "tree234.h"

#ifdef PATH_MAX
#define FNLEN PATH_MAX
#else
#define FNLEN 1024 /* XXX */
#endif

enum {
    INDEX_DIR, INDEX_HOSTKEYS, INDEX_HOSTKEYS_TMP, INDEX_RANDSEED,
    INDEX_SESSIONDIR, INDEX_SESSION, INDEX_HOSTCADIR, INDEX_HOSTCA
};

static const char hex[16] = "0123456789ABCDEF";

static void make_session_filename(const char *in, strbuf *out)
{
    if (!in || !*in)
        in = "Default Settings";

    while (*in) {
        /*
         * There are remarkably few punctuation characters that
         * aren't shell-special in some way or likely to be used as
         * separators in some file format or another! Hence we use
         * opt-in for safe characters rather than opt-out for
         * specific unsafe ones...
         */
        if (*in!='+' && *in!='-' && *in!='.' && *in!='@' && *in!='_' &&
            !(*in >= '0' && *in <= '9') &&
            !(*in >= 'A' && *in <= 'Z') &&
            !(*in >= 'a' && *in <= 'z')) {
            put_byte(out, '%');
            put_byte(out, hex[((unsigned char) *in) >> 4]);
            put_byte(out, hex[((unsigned char) *in) & 15]);
        } else
            put_byte(out, *in);
        in++;
    }
}

static void decode_session_filename(const char *in, strbuf *out)
{
    while (*in) {
        if (*in == '%' && in[1] && in[2]) {
            int i, j;

            i = in[1] - '0';
            i -= (i > 9 ? 7 : 0);
            j = in[2] - '0';
            j -= (j > 9 ? 7 : 0);

            put_byte(out, (i << 4) + j);
            in += 3;
        } else {
            put_byte(out, *in++);
        }
    }
}

/*
 * PuTTY-beter: apply any config-location redirect to a base directory.
 * Defined below, next to the rest of the relocatable-config code.
 */
static char *config_effective_dir_for(char *base);

static char *make_filename(int index, const char *subname)
{
    char *env, *tmp, *ret;

    /*
     * Allow override of the PuTTY configuration location, and of
     * specific subparts of it, by means of environment variables.
     */
    if (index == INDEX_DIR) {
        struct passwd *pwd;
        char *xdg_dir, *old_dir, *old_dir2, *old_dir3, *home, *pwd_home;

        env = getenv("PUTTYDIR");
        if (env)
            return dupstr(env);

        home = getenv("HOME");
        pwd = getpwuid(getuid());
        if (pwd && pwd->pw_dir) {
            pwd_home = pwd->pw_dir;
        } else {
            pwd_home = NULL;
        }

        xdg_dir = NULL;
        env = getenv("XDG_CONFIG_HOME");
        if (env && *env) {
            xdg_dir = dupprintf("%s/putty", env);
        }
        if (!xdg_dir) {
            if (home) {
                tmp = home;
            } else if (pwd_home) {
                tmp = pwd_home;
            } else {
                tmp = "";
            }
            xdg_dir = dupprintf("%s/.config/putty", tmp);
        }

        /*
         * These three are freed at the `out' label below, so initialise
         * them before any of the gotos that lead there.
         */
        old_dir = old_dir2 = old_dir3 = NULL;

        if (xdg_dir && access(xdg_dir, F_OK) == 0) {
            /*
             * PuTTY-beter: go through `out' rather than returning directly,
             * so that a config-location redirect file in this directory is
             * honoured as well.
             */
            ret = xdg_dir;
            goto out;
        }
        if (home) {
            old_dir = dupprintf("%s/.putty", home);
        }
        if (pwd_home) {
            old_dir2 = dupprintf("%s/.putty", pwd_home);
        }
        old_dir3 = dupstr("/.putty");

        if (old_dir && access(old_dir, F_OK) == 0) {
            ret = old_dir;
            goto out;
        }
        if (old_dir2 && access(old_dir2, F_OK) == 0) {
            ret = old_dir2;
            goto out;
        }
        if (access(old_dir3, F_OK) == 0) {
            ret = old_dir3;
            goto out;
        }
#ifdef XDG_DEFAULT
        if (xdg_dir) {
            ret = xdg_dir;
            goto out;
        }
#endif
        ret = old_dir ? old_dir : (old_dir2 ? old_dir2 : old_dir3);

      out:
        if (ret != old_dir)
            sfree(old_dir);
        if (ret != old_dir2)
            sfree(old_dir2);
        if (ret != old_dir3)
            sfree(old_dir3);
        if (ret != xdg_dir)
            sfree(xdg_dir);
        /*
         * PuTTY-beter: the directory we have just worked out is the *base*
         * location - the place PuTTY has always used. If it contains a
         * config-location file, use the directory named in there instead.
         * config_effective_dir_for takes ownership of ret.
         */
        return config_effective_dir_for(ret);
    }
    if (index == INDEX_SESSIONDIR) {
        env = getenv("PUTTYSESSIONS");
        if (env)
            return dupstr(env);
        tmp = make_filename(INDEX_DIR, NULL);
        ret = dupprintf("%s/sessions", tmp);
        sfree(tmp);
        return ret;
    }
    if (index == INDEX_SESSION) {
        strbuf *sb = strbuf_new();
        tmp = make_filename(INDEX_SESSIONDIR, NULL);
        put_fmt(sb, "%s/", tmp);
        sfree(tmp);
        make_session_filename(subname, sb);
        return strbuf_to_str(sb);
    }
    if (index == INDEX_HOSTKEYS) {
        env = getenv("PUTTYSSHHOSTKEYS");
        if (env)
            return dupstr(env);
        tmp = make_filename(INDEX_DIR, NULL);
        ret = dupprintf("%s/sshhostkeys", tmp);
        sfree(tmp);
        return ret;
    }
    if (index == INDEX_HOSTKEYS_TMP) {
        tmp = make_filename(INDEX_HOSTKEYS, NULL);
        ret = dupprintf("%s.tmp", tmp);
        sfree(tmp);
        return ret;
    }
    if (index == INDEX_RANDSEED) {
        env = getenv("PUTTYRANDOMSEED");
        if (env)
            return dupstr(env);
        tmp = make_filename(INDEX_DIR, NULL);
        ret = dupprintf("%s/randomseed", tmp);
        sfree(tmp);
        return ret;
    }
    if (index == INDEX_HOSTCADIR) {
        env = getenv("PUTTYSSHHOSTCAS");
        if (env)
            return dupstr(env);
        tmp = make_filename(INDEX_DIR, NULL);
        ret = dupprintf("%s/sshhostcas", tmp);
        sfree(tmp);
        return ret;
    }
    if (index == INDEX_HOSTCA) {
        strbuf *sb = strbuf_new();
        tmp = make_filename(INDEX_HOSTCADIR, NULL);
        put_fmt(sb, "%s/", tmp);
        sfree(tmp);
        make_session_filename(subname, sb);
        return strbuf_to_str(sb);
    }
    tmp = make_filename(INDEX_DIR, NULL);
    ret = dupprintf("%s/ERROR", tmp);
    sfree(tmp);
    return ret;
}

/* ----------------------------------------------------------------------
 * PuTTY-beter: relocatable configuration directory.
 *
 * The directory worked out above is the *base* location - the place PuTTY
 * has always kept its configuration. A base location may contain a small
 * text file called `config-location', holding the absolute path of the
 * directory that really holds the sessions, host keys, CA records and so
 * on. If it does, and the directory it names exists, that directory is
 * used for everything instead.
 *
 * Keeping the indirection in a plain file rather than, say, a symlink
 * means that plink, pscp and psftp - which link this file too, but have no
 * GUI - follow the same redirection, and it can be edited by hand.
 *
 * None of this changes anything when the file is absent, so a stock
 * ~/.putty tree behaves exactly as it always has.
 *
 * Set PUTTY_CONFIG_DEBUG=1 in the environment to trace what happens.
 */

#define CONFIG_REDIRECT_FILENAME "config-location"
#define CONFIG_DEFAULT_SUBDIR "Documents/putty-beter-config"

/* The pieces of a configuration that move with it. */
static const char *const config_move_list[] = {
    "sessions", "sshhostkeys", "sshhostkeys.tmp", "sshhostcas", "randomseed",
    NULL
};

static bool config_debug_enabled(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = (getenv("PUTTY_CONFIG_DEBUG") != NULL);
    return cached != 0;
}

/*
 * Expand a leading "~" or "~/" using the current user's home directory.
 * "~user" is deliberately unsupported: nobody types it, and resolving it
 * would mean parsing getpwnam() output for no benefit.
 */
static char *config_expand_home(const char *path)
{
    if (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
        const char *home = getenv("HOME");
        if (!home || !*home) {
            struct passwd *pwd = getpwuid(getuid());
            home = (pwd && pwd->pw_dir) ? pwd->pw_dir : NULL;
        }
        if (home)
            return dupprintf("%s%s", home, path+1);
    }
    return dupstr(path);
}

/*
 * Read the redirect file from a base directory. Returns the target
 * directory (newly allocated) if the file exists, names a directory that
 * is actually there, and has not been commented out; otherwise NULL.
 */
static char *config_redirect_read(const char *base)
{
    char *path, *line, *target = NULL;
    FILE *fp;

    path = dupprintf("%s/" CONFIG_REDIRECT_FILENAME, base);
    fp = fopen(path, "r");
    if (!fp) {
        if (config_debug_enabled())
            fprintf(stderr, "[config-location] no redirect file %s (%s)\n",
                    path, strerror(errno));
        sfree(path);
        return NULL;
    }

    while ((line = fgetline(fp)) != NULL) {
        char *p = line;
        line[strcspn(line, "\r\n")] = '\0';
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p && *p != '#') {
            target = dupstr(p);
            sfree(line);
            break;
        }
        sfree(line);
    }
    fclose(fp);

    if (!target) {
        if (config_debug_enabled())
            fprintf(stderr, "[config-location] %s names no directory\n", path);
        sfree(path);
        return NULL;
    }

    char *expanded = config_expand_home(target);
    sfree(target);

    struct stat st;
    if (stat(expanded, &st) < 0 || !S_ISDIR(st.st_mode)) {
        if (config_debug_enabled())
            fprintf(stderr, "[config-location] %s names %s, which is not a "
                    "directory; ignoring it\n", path, expanded);
        sfree(expanded);
        sfree(path);
        return NULL;
    }

    if (config_debug_enabled())
        fprintf(stderr, "[config-location] %s redirects to %s\n",
                path, expanded);
    sfree(path);
    return expanded;
}

/*
 * Cached results. Remapping INDEX_DIR is on the hot path (every settings
 * read and write goes through it), so avoid re-reading the redirect file
 * every time. config_location_set() invalidates this.
 */
static char *config_base_cache = NULL;
static char *config_effective_cache = NULL;

static char *config_effective_dir_for(char *base)
{
    if (config_base_cache && config_effective_cache &&
        !strcmp(config_base_cache, base)) {
        sfree(base);
        return dupstr(config_effective_cache);
    }

    sfree(config_base_cache);
    sfree(config_effective_cache);
    config_base_cache = config_effective_cache = NULL;

    config_base_cache = dupstr(base);
    char *redirect = config_redirect_read(base);
    config_effective_cache = redirect ? redirect : dupstr(base);

    if (config_debug_enabled())
        fprintf(stderr, "[config-location] base %s -> effective %s\n",
                config_base_cache, config_effective_cache);

    sfree(base);
    return dupstr(config_effective_cache);
}

void config_location_invalidate(void)
{
    sfree(config_base_cache);
    sfree(config_effective_cache);
    config_base_cache = config_effective_cache = NULL;
    if (config_debug_enabled())
        fprintf(stderr, "[config-location] cache invalidated\n");
}

/* The directory that holds the redirect file, ignoring any redirect. */
char *config_location_base(void)
{
    const char *env = getenv("PUTTYDIR");
    if (env)
        return dupstr(env);

    char *eff = make_filename(INDEX_DIR, NULL);
    sfree(eff);
    return dupstr(config_base_cache ? config_base_cache : ".");
}

/* The directory actually in use, after any redirect. */
char *config_location_current(void)
{
    return make_filename(INDEX_DIR, NULL);
}

bool config_location_is_redirected(void)
{
    char *eff = make_filename(INDEX_DIR, NULL);
    bool redirected = (config_base_cache && strcmp(eff, config_base_cache));
    sfree(eff);
    return redirected;
}

/* Proposed home for a fresh installation: $HOME/Documents/putty-beter-config */
char *config_location_default_dir(void)
{
    const char *home = getenv("HOME");
    struct passwd *pwd;

    if (!home || !*home) {
        pwd = getpwuid(getuid());
        home = (pwd && pwd->pw_dir) ? pwd->pw_dir : NULL;
    }
    if (!home || !*home)
        return NULL;

    return dupprintf("%s/" CONFIG_DEFAULT_SUBDIR, home);
}

/*
 * A configuration counts as present once it has a sessions subdirectory.
 * That is the marker the relocation dialog leaves behind, so the dialog
 * does not come back on every launch.
 */
bool config_location_is_configured(void)
{
    char *dir = config_location_current();
    char *sessions = dupprintf("%s/sessions", dir);
    bool configured = (access(sessions, F_OK) == 0);

    if (config_debug_enabled())
        fprintf(stderr, "[config-location] %s is %sconfigured\n",
                dir, configured ? "" : "not ");

    sfree(sessions);
    sfree(dir);
    return configured;
}

/* --- moving an existing configuration ------------------------------- */

static char *config_copy_file(const char *src, const char *dst, mode_t mode)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return dupprintf("Unable to read %s: %s", src, strerror(errno));

    int out = open(dst, O_WRONLY|O_CREAT|O_EXCL, mode & 0777);
    if (out < 0) {
        char *err = dupprintf("Unable to create %s: %s", dst, strerror(errno));
        close(in);
        return err;
    }

    char buf[8192];
    ssize_t n;
    char *err = NULL;

    while (!err && (n = read(in, buf, sizeof(buf))) > 0) {
        char *p = buf;
        ssize_t left = n;
        while (left > 0) {
            ssize_t written = write(out, p, left);
            if (written <= 0) {
                err = dupprintf("Unable to write %s: %s", dst, strerror(errno));
                break;
            }
            p += written;
            left -= written;
        }
    }
    if (!err && n < 0)
        err = dupprintf("Unable to read %s: %s", src, strerror(errno));

    close(in);
    if (close(out) < 0 && !err)
        err = dupprintf("Unable to write %s: %s", dst, strerror(errno));

    if (err)
        unlink(dst);
    return err;
}

/*
 * Recursive copy, used as the fallback when rename() reports EXDEV (the
 * new configuration directory is on a different filesystem, which is
 * easy to do by accident when $HOME/Documents is a symlink to a volume).
 */
static char *config_copy_path(const char *src, const char *dst)
{
    struct stat st;

    if (lstat(src, &st) < 0)
        return dupprintf("Unable to read %s: %s", src, strerror(errno));

    if (S_ISDIR(st.st_mode)) {
        char *err = NULL;
        struct dirent *de;
        DIR *dp;

        if (mkdir(dst, st.st_mode & 0777) < 0 && errno != EEXIST)
            return dupprintf("Unable to create %s: %s", dst, strerror(errno));

        dp = opendir(src);
        if (!dp)
            return dupprintf("Unable to read %s: %s", src, strerror(errno));

        while (!err && (de = readdir(dp)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            char *s = dupprintf("%s/%s", src, de->d_name);
            char *d = dupprintf("%s/%s", dst, de->d_name);
            err = config_copy_path(s, d);
            sfree(s);
            sfree(d);
        }
        closedir(dp);
        return err;
    }

    if (S_ISLNK(st.st_mode)) {
        char linkbuf[FNLEN];
        ssize_t n = readlink(src, linkbuf, sizeof(linkbuf)-1);
        if (n < 0)
            return dupprintf("Unable to read link %s: %s", src, strerror(errno));
        linkbuf[n] = '\0';
        if (symlink(linkbuf, dst) < 0)
            return dupprintf("Unable to create link %s: %s", dst, strerror(errno));
        return NULL;
    }

    if (!S_ISREG(st.st_mode))
        return dupprintf("Unable to move %s: unsupported file type", src);

    return config_copy_file(src, dst, st.st_mode);
}

static char *config_remove_path(const char *path)
{
    struct stat st;

    if (lstat(path, &st) < 0)
        return (errno == ENOENT) ? NULL :
            dupprintf("Unable to remove %s: %s", path, strerror(errno));

    if (S_ISDIR(st.st_mode)) {
        char *err = NULL;
        struct dirent *de;
        DIR *dp = opendir(path);

        if (!dp)
            return dupprintf("Unable to read %s: %s", path, strerror(errno));

        while (!err && (de = readdir(dp)) != NULL) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            char *child = dupprintf("%s/%s", path, de->d_name);
            err = config_remove_path(child);
            sfree(child);
        }
        closedir(dp);

        if (!err && rmdir(path) < 0)
            err = dupprintf("Unable to remove %s: %s", path, strerror(errno));
        return err;
    }

    if (unlink(path) < 0)
        return dupprintf("Unable to remove %s: %s", path, strerror(errno));
    return NULL;
}

/*
 * Move the pieces of the configuration from src to dst. Individual items
 * that already exist in dst are left alone rather than merged or
 * overwritten, and nothing in src is deleted unless it was copied
 * successfully first.
 */
static char *config_move_files(const char *src, const char *dst)
{
    char *err = NULL;

    for (int i = 0; config_move_list[i]; i++) {
        char *s = dupprintf("%s/%s", src, config_move_list[i]);
        char *d = dupprintf("%s/%s", dst, config_move_list[i]);

        if (access(s, F_OK) == 0) {
            if (access(d, F_OK) == 0) {
                /*
                 * Destination already has one of these. Leave both alone:
                 * silently merging or overwriting user data is worse than
                 * leaving the old copy where it was.
                 */
                if (config_debug_enabled())
                    fprintf(stderr, "[config-location] %s exists; leaving "
                            "%s in place\n", d, s);
            } else if (rename(s, d) < 0) {
                if (errno == EXDEV) {
                    if (config_debug_enabled())
                        fprintf(stderr, "[config-location] %s is on another "
                                "filesystem; copying\n", d);
                    err = config_copy_path(s, d);
                    if (!err)
                        err = config_remove_path(s);
                } else {
                    err = dupprintf("Unable to move %s to %s: %s",
                                    s, d, strerror(errno));
                }
            } else if (config_debug_enabled()) {
                fprintf(stderr, "[config-location] moved %s -> %s\n", s, d);
            }
        }

        sfree(s);
        sfree(d);
        if (err)
            break;
    }

    return err;
}

/*
 * Point the configuration at a new directory: normalise the name, move
 * anything that is already saved, never overwrite anything, and then
 * record the location in the redirect file at the base location.
 *
 * Returns NULL on success, or a user-presentable error message.
 */
char *config_location_set(const char *dirtext)
{
    char *err = NULL, *path = NULL, *base = NULL, *current = NULL;
    char *redirect_path = NULL, *sessions = NULL;

    if (!dirtext || !*dirtext)
        return dupstr("Please specify a folder.");

    path = config_expand_home(dirtext);
    if (path[0] != '/') {
        char cwd[FNLEN];
        if (!getcwd(cwd, sizeof(cwd))) {
            err = dupprintf("Unable to resolve the current directory: %s",
                            strerror(errno));
            goto out;
        }
        char *tmp = dupprintf("%s/%s", cwd, path);
        sfree(path);
        path = tmp;
    }
    { /* strip trailing slashes, but keep a bare "/" */
        size_t len = strlen(path);
        while (len > 1 && path[len-1] == '/')
            path[--len] = '\0';
    }

    base = config_location_base();
    current = config_location_current();

    if (config_debug_enabled())
        fprintf(stderr, "[config-location] set: base=%s current=%s new=%s\n",
                base, current, path);

    if (strcmp(path, current) != 0) {
        if ((err = make_dir_path(path, 0700)) != NULL)
            goto out;
        if ((err = config_move_files(current, path)) != NULL)
            goto out;
    }

    /*
     * Create the sessions subdirectory, which marks the folder as
     * configured so that the first-run dialog does not appear again.
     */
    sessions = dupprintf("%s/sessions", path);
    if ((err = make_dir_path(sessions, 0700)) != NULL)
        goto out;

    redirect_path = dupprintf("%s/" CONFIG_REDIRECT_FILENAME, base);

    if (!strcmp(path, base)) {
        /* Back at the base location: the redirect is not needed. */
        if (unlink(redirect_path) < 0 && errno != ENOENT) {
            err = dupprintf("Unable to remove %s: %s",
                            redirect_path, strerror(errno));
            goto out;
        }
    } else {
        FILE *fp = fopen(redirect_path, "w");
        if (!fp) {
            err = dupprintf("Unable to write %s: %s",
                            redirect_path, strerror(errno));
            goto out;
        }
        fprintf(fp, "# PuTTY-beter configuration directory.\n");
        fprintf(fp, "# Delete this file to go back to %s.\n", base);
        fprintf(fp, "%s\n", path);
        if (fclose(fp) < 0)
            err = dupprintf("Unable to write %s: %s",
                            redirect_path, strerror(errno));
    }

  out:
    config_location_invalidate();
    if (config_debug_enabled())
        fprintf(stderr, "[config-location] set: %s\n",
                err ? err : "done");
    sfree(redirect_path);
    sfree(sessions);
    sfree(base);
    sfree(current);
    sfree(path);
    return err;
}

struct settings_w {
    FILE *fp;
};

settings_w *open_settings_w(const char *sessionname, char **errmsg)
{
    char *filename, *err;
    FILE *fp;

    *errmsg = NULL;

    /*
     * Start by making sure the .putty directory and its sessions
     * subdir actually exist.
     */
    filename = make_filename(INDEX_DIR, NULL);
    if ((err = make_dir_path(filename, 0700)) != NULL) {
        *errmsg = dupprintf("Unable to save session: %s", err);
        sfree(err);
        sfree(filename);
        return NULL;
    }
    sfree(filename);

    filename = make_filename(INDEX_SESSIONDIR, NULL);
    if ((err = make_dir_path(filename, 0700)) != NULL) {
        *errmsg = dupprintf("Unable to save session: %s", err);
        sfree(err);
        sfree(filename);
        return NULL;
    }
    sfree(filename);

    filename = make_filename(INDEX_SESSION, sessionname);
    fp = fopen(filename, "w");
    if (!fp) {
        *errmsg = dupprintf("Unable to save session: open(\"%s\") "
                            "returned '%s'", filename, strerror(errno));
        sfree(filename);
        return NULL;                   /* can't open */
    }
    sfree(filename);

    settings_w *toret = snew(settings_w);
    toret->fp = fp;
    return toret;
}

void write_setting_s(settings_w *handle, const char *key, const char *value)
{
    fprintf(handle->fp, "%s=%s\n", key, value);
}

void write_setting_i(settings_w *handle, const char *key, int value)
{
    fprintf(handle->fp, "%s=%d\n", key, value);
}

void close_settings_w(settings_w *handle)
{
    fclose(handle->fp);
    sfree(handle);
}

/* ----------------------------------------------------------------------
 * System for treating X resources as a fallback source of defaults,
 * after data read from a saved-session disk file.
 *
 * The read_setting_* functions will call get_setting(key) as a
 * fallback if the setting isn't in the file they loaded. That in turn
 * will hand on to x_get_default, which the front end application
 * provides, and which actually reads resources from the X server (if
 * appropriate). In between, there's a tree234 of X-resource shaped
 * settings living locally in this file: the front end can call
 * provide_xrm_string() to insert a setting into this tree (typically
 * in response to an -xrm command line option or similar), and those
 * will override the actual X resources.
 */

struct skeyval {
    const char *key;
    const char *value;
};

static tree234 *xrmtree = NULL;

static int keycmp(void *av, void *bv)
{
    struct skeyval *a = (struct skeyval *)av;
    struct skeyval *b = (struct skeyval *)bv;
    return strcmp(a->key, b->key);
}

void provide_xrm_string(const char *string, const char *progname)
{
    const char *p, *q;
    struct skeyval *xrms, *ret;

    p = q = strchr(string, ':');
    if (!q) {
        fprintf(stderr, "%s: expected a colon in resource string"
                " \"%s\"\n", progname, string);
        return;
    }
    xrms = snew(struct skeyval);

    while (p > string && p[-1] != '.' && p[-1] != '*')
        p--;
    xrms->key = mkstr(make_ptrlen(p, q-p));

    q++;
    while (*q && isspace((unsigned char)*q))
        q++;
    xrms->value = dupstr(q);

    if (!xrmtree)
        xrmtree = newtree234(keycmp);

    ret = add234(xrmtree, xrms);
    if (ret) {
        /* Override an existing string. */
        del234(xrmtree, ret);
        add234(xrmtree, xrms);
    }
}

static const char *get_setting(const char *key)
{
    struct skeyval tmp, *ret;
    tmp.key = key;
    if (xrmtree) {
        ret = find234(xrmtree, &tmp, NULL);
        if (ret)
            return ret->value;
    }
    return x_get_default(key);
}

/* ----------------------------------------------------------------------
 * Main code for reading settings from a disk file, calling the above
 * get_setting() as a fallback if necessary.
 */

struct settings_r {
    tree234 *t;
};

settings_r *open_settings_r(const char *sessionname)
{
    char *filename;
    FILE *fp;
    char *line;
    settings_r *toret;

    filename = make_filename(INDEX_SESSION, sessionname);
    fp = fopen(filename, "r");
    sfree(filename);
    if (!fp)
        return NULL;                   /* can't open */

    toret = snew(settings_r);
    toret->t = newtree234(keycmp);

    while ( (line = fgetline(fp)) ) {
        char *value = strchr(line, '=');
        struct skeyval *kv;

        if (!value) {
            sfree(line);
            continue;
        }
        *value++ = '\0';
        value[strcspn(value, "\r\n")] = '\0';   /* trim trailing NL */

        kv = snew(struct skeyval);
        kv->key = dupstr(line);
        kv->value = dupstr(value);
        add234(toret->t, kv);

        sfree(line);
    }

    fclose(fp);

    return toret;
}

char *read_setting_s(settings_r *handle, const char *key)
{
    const char *val;
    struct skeyval tmp, *kv;

    tmp.key = key;
    if (handle != NULL &&
        (kv = find234(handle->t, &tmp, NULL)) != NULL) {
        val = kv->value;
        assert(val != NULL);
    } else
        val = get_setting(key);

    if (!val)
        return NULL;
    else
        return dupstr(val);
}

int read_setting_i(settings_r *handle, const char *key, int defvalue)
{
    const char *val;
    struct skeyval tmp, *kv;

    tmp.key = key;
    if (handle != NULL &&
        (kv = find234(handle->t, &tmp, NULL)) != NULL) {
        val = kv->value;
        assert(val != NULL);
    } else
        val = get_setting(key);

    if (!val)
        return defvalue;
    else
        return atoi(val);
}

FontSpec *read_setting_fontspec(settings_r *handle, const char *name)
{
    /*
     * In GTK1-only PuTTY, we used to store font names simply as a
     * valid X font description string (logical or alias), under a
     * bare key such as "Font".
     *
     * In GTK2 PuTTY, we have a prefix system where "client:"
     * indicates a Pango font and "server:" an X one; existing
     * configuration needs to be reinterpreted as having the
     * "server:" prefix, so we change the storage key from the
     * provided name string (e.g. "Font") to a suffixed one
     * ("FontName").
     */
    char *suffname = dupcat(name, "Name");
    char *tmp;

    if ((tmp = read_setting_s(handle, suffname)) != NULL) {
        FontSpec *fs = fontspec_new(tmp);
        sfree(suffname);
        sfree(tmp);
        return fs;                     /* got new-style name */
    }
    sfree(suffname);

    /* Fall back to old-style name. */
    tmp = read_setting_s(handle, name);
    if (tmp && *tmp) {
        char *tmp2 = dupcat("server:", tmp);
        FontSpec *fs = fontspec_new(tmp2);
        sfree(tmp2);
        sfree(tmp);
        return fs;
    } else {
        sfree(tmp);
        return NULL;
    }
}
Filename *read_setting_filename(settings_r *handle, const char *name)
{
    char *tmp = read_setting_s(handle, name);
    if (tmp) {
        Filename *ret = filename_from_str(tmp);
        sfree(tmp);
        return ret;
    } else
        return NULL;
}

void write_setting_fontspec(settings_w *handle, const char *name, FontSpec *fs)
{
    /*
     * read_setting_fontspec had to handle two cases, but when
     * writing our settings back out we simply always generate the
     * new-style name.
     */
    char *suffname = dupcat(name, "Name");
    write_setting_s(handle, suffname, fs->name);
    sfree(suffname);
}
void write_setting_filename(settings_w *handle,
                            const char *name, Filename *result)
{
    write_setting_s(handle, name, result->path);
}

void close_settings_r(settings_r *handle)
{
    struct skeyval *kv;

    if (!handle)
        return;

    while ( (kv = index234(handle->t, 0)) != NULL) {
        del234(handle->t, kv);
        sfree((char *)kv->key);
        sfree((char *)kv->value);
        sfree(kv);
    }

    freetree234(handle->t);
    sfree(handle);
}

void del_settings(const char *sessionname)
{
    char *filename;
    filename = make_filename(INDEX_SESSION, sessionname);
    unlink(filename);
    sfree(filename);
}

struct settings_e {
    DIR *dp;
};

settings_e *enum_settings_start(void)
{
    DIR *dp;
    char *filename;

    filename = make_filename(INDEX_SESSIONDIR, NULL);
    dp = opendir(filename);
    sfree(filename);

    settings_e *toret = snew(settings_e);
    toret->dp = dp;
    return toret;
}

static bool enum_dir_next(DIR *dp, int index, strbuf *out)
{
    struct dirent *de;
    struct stat st;
    strbuf *fullpath;

    if (!dp)
        return false;

    fullpath = strbuf_new();

    char *sessiondir = make_filename(index, NULL);
    put_dataz(fullpath, sessiondir);
    sfree(sessiondir);
    put_byte(fullpath, '/');

    size_t baselen = fullpath->len;

    while ( (de = readdir(dp)) != NULL ) {
        strbuf_shrink_to(fullpath, baselen);
        put_dataz(fullpath, de->d_name);

        if (stat(fullpath->s, &st) < 0 || !S_ISREG(st.st_mode))
            continue;                  /* try another one */

        decode_session_filename(de->d_name, out);
        strbuf_free(fullpath);
        return true;
    }

    strbuf_free(fullpath);
    return false;
}

bool enum_settings_next(settings_e *handle, strbuf *out)
{
    return enum_dir_next(handle->dp, INDEX_SESSIONDIR, out);
}

void enum_settings_finish(settings_e *handle)
{
    if (handle->dp)
        closedir(handle->dp);
    sfree(handle);
}

struct host_ca_enum {
    DIR *dp;
};

host_ca_enum *enum_host_ca_start(void)
{
    host_ca_enum *handle = snew(host_ca_enum);

    char *filename = make_filename(INDEX_HOSTCADIR, NULL);
    handle->dp = opendir(filename);
    sfree(filename);

    return handle;
}

bool enum_host_ca_next(host_ca_enum *handle, strbuf *out)
{
    return enum_dir_next(handle->dp, INDEX_HOSTCADIR, out);
}

void enum_host_ca_finish(host_ca_enum *handle)
{
    if (handle->dp)
        closedir(handle->dp);
    sfree(handle);
}

host_ca *host_ca_load(const char *name)
{
    char *filename = make_filename(INDEX_HOSTCA, name);
    FILE *fp = fopen(filename, "r");
    sfree(filename);
    if (!fp)
        return NULL;

    host_ca *hca = host_ca_new();
    hca->name = dupstr(name);

    char *line;
    CertExprBuilder *eb = NULL;

    while ( (line = fgetline(fp)) ) {
        char *value = strchr(line, '=');

        if (!value) {
            sfree(line);
            continue;
        }
        *value++ = '\0';
        value[strcspn(value, "\r\n")] = '\0';   /* trim trailing NL */

        if (!strcmp(line, "PublicKey")) {
            hca->ca_public_key = base64_decode_sb(ptrlen_from_asciz(value));
        } else if (!strcmp(line, "MatchHosts")) {
            if (!eb)
                eb = cert_expr_builder_new();
            cert_expr_builder_add(eb, value);
        } else if (!strcmp(line, "Validity")) {
            hca->validity_expression = strbuf_to_str(
                percent_decode_sb(ptrlen_from_asciz(value)));
        } else if (!strcmp(line, "PermitRSASHA1")) {
            hca->opts.permit_rsa_sha1 = atoi(value);
        } else if (!strcmp(line, "PermitRSASHA256")) {
            hca->opts.permit_rsa_sha256 = atoi(value);
        } else if (!strcmp(line, "PermitRSASHA512")) {
            hca->opts.permit_rsa_sha512 = atoi(value);
        }

        sfree(line);
    }

    fclose(fp);

    if (eb) {
        if (!hca->validity_expression) {
            hca->validity_expression = cert_expr_expression(eb);
        }
        cert_expr_builder_free(eb);
    }

    return hca;
}

char *host_ca_save(host_ca *hca)
{
    char *filename, *err;

    if (!*hca->name)
        return dupstr("CA record must have a name");

    filename = make_filename(INDEX_DIR, NULL);
    if ((err = make_dir_path(filename, 0700)) != NULL) {
        char *msg = dupprintf("Unable to save CA record: "
                              "making directory '%s': %s", filename, err);
        sfree(err);
        sfree(filename);
        return msg;
    }
    sfree(filename);

    filename = make_filename(INDEX_HOSTCADIR, NULL);
    if ((err = make_dir_path(filename, 0700)) != NULL) {
        char *msg = dupprintf("Unable to save CA record: "
                              "making directory '%s': %s", filename, err);
        sfree(err);
        sfree(filename);
        return msg;
    }
    sfree(filename);

    filename = make_filename(INDEX_HOSTCA, hca->name);
    FILE *fp = fopen(filename, "w");
    if (!fp)
        return dupprintf("Unable to save CA record: opening file '%s': %s",
                         filename, strerror(errno));

    fprintf(fp, "PublicKey=");
    base64_encode_fp(fp, ptrlen_from_strbuf(hca->ca_public_key), 0);
    fprintf(fp, "\n");

    fprintf(fp, "Validity=");
    percent_encode_fp(fp, ptrlen_from_asciz(hca->validity_expression), NULL);
    fprintf(fp, "\n");

    fprintf(fp, "PermitRSASHA1=%d\n", (int)hca->opts.permit_rsa_sha1);
    fprintf(fp, "PermitRSASHA256=%d\n", (int)hca->opts.permit_rsa_sha256);
    fprintf(fp, "PermitRSASHA512=%d\n", (int)hca->opts.permit_rsa_sha512);

    bool bad = ferror(fp);
    if (fclose(fp) < 0)
        bad = true;

    err = NULL;
    if (bad)
        err = dupprintf("Unable to save CA record: writing file '%s': %s",
                        filename, strerror(errno));

    sfree(filename);
    return err;
}

char *host_ca_delete(const char *name)
{
    if (!*name)
        return dupstr("CA record must have a name");
    char *filename = make_filename(INDEX_HOSTCA, name);
    bool bad = remove(filename) < 0;

    char *err = NULL;
    if (bad)
        err = dupprintf("Unable to delete file '%s'", filename);

    sfree(filename);
    return err;
}

/*
 * Lines in the host keys file are of the form
 *
 *   type@port:hostname keydata
 *
 * e.g.
 *
 *   rsa@22:foovax.example.org 0x23,0x293487364395345345....2343
 */
int check_stored_host_key(const char *hostname, int port,
                          const char *keytype, const char *key)
{
    FILE *fp;
    char *filename;
    char *line;
    int ret;

    filename = make_filename(INDEX_HOSTKEYS, NULL);
    fp = fopen(filename, "r");
    sfree(filename);
    if (!fp)
        return 1;                      /* key does not exist */

    ret = 1;
    while ( (line = fgetline(fp)) ) {
        int i;
        char *p = line;
        char porttext[20];

        line[strcspn(line, "\n")] = '\0';   /* strip trailing newline */

        i = strlen(keytype);
        if (strncmp(p, keytype, i))
            goto done;
        p += i;

        if (*p != '@')
            goto done;
        p++;

        sprintf(porttext, "%d", port);
        i = strlen(porttext);
        if (strncmp(p, porttext, i))
            goto done;
        p += i;

        if (*p != ':')
            goto done;
        p++;

        i = strlen(hostname);
        if (strncmp(p, hostname, i))
            goto done;
        p += i;

        if (*p != ' ')
            goto done;
        p++;

        /*
         * Found the key. Now just work out whether it's the right
         * one or not.
         */
        if (!strcmp(p, key))
            ret = 0;                   /* key matched OK */
        else
            ret = 2;                   /* key mismatch */

      done:
        sfree(line);
        if (ret != 1)
            break;
    }

    fclose(fp);
    return ret;
}

bool have_ssh_host_key(const char *hostname, int port,
                       const char *keytype)
{
    /*
     * If we have a host key, check_stored_host_key will return 0 or 2.
     * If we don't have one, it'll return 1.
     */
    return check_stored_host_key(hostname, port, keytype, "") != 1;
}

void store_host_key(Seat *seat, const char *hostname, int port,
                    const char *keytype, const char *key)
{
    FILE *rfp, *wfp;
    char *newtext, *line;
    int headerlen;
    char *filename, *tmpfilename;

    /*
     * Open both the old file and a new file.
     */
    tmpfilename = make_filename(INDEX_HOSTKEYS_TMP, NULL);
    wfp = fopen(tmpfilename, "w");
    if (!wfp && errno == ENOENT) {
        char *dir, *errmsg;

        dir = make_filename(INDEX_DIR, NULL);
        if ((errmsg = make_dir_path(dir, 0700)) != NULL) {
            seat_nonfatal(seat, "Unable to store host key: %s", errmsg);
            sfree(errmsg);
            sfree(dir);
            sfree(tmpfilename);
            return;
        }
        sfree(dir);

        wfp = fopen(tmpfilename, "w");
    }
    if (!wfp) {
        seat_nonfatal(seat, "Unable to store host key: open(\"%s\") "
                      "returned '%s'", tmpfilename, strerror(errno));
        sfree(tmpfilename);
        return;
    }
    filename = make_filename(INDEX_HOSTKEYS, NULL);
    rfp = fopen(filename, "r");

    newtext = dupprintf("%s@%d:%s %s\n", keytype, port, hostname, key);
    headerlen = 1 + strcspn(newtext, " ");   /* count the space too */

    /*
     * Copy all lines from the old file to the new one that _don't_
     * involve the same host key identifier as the one we're adding.
     */
    if (rfp) {
        while ( (line = fgetline(rfp)) ) {
            if (strncmp(line, newtext, headerlen))
                fputs(line, wfp);
            sfree(line);
        }
        fclose(rfp);
    }

    /*
     * Now add the new line at the end.
     */
    fputs(newtext, wfp);

    fclose(wfp);

    if (rename(tmpfilename, filename) < 0) {
        seat_nonfatal(seat, "Unable to store host key: rename(\"%s\",\"%s\")"
                      " returned '%s'", tmpfilename, filename,
                      strerror(errno));
    }

    sfree(tmpfilename);
    sfree(filename);
    sfree(newtext);
}

void read_random_seed(noise_consumer_t consumer)
{
    int fd;
    char *fname;

    fname = make_filename(INDEX_RANDSEED, NULL);
    fd = open(fname, O_RDONLY);
    sfree(fname);
    if (fd >= 0) {
        char buf[512];
        int ret;
        while ( (ret = read(fd, buf, sizeof(buf))) > 0)
            consumer(buf, ret);
        close(fd);
    }
}

void write_random_seed(void *data, int len)
{
    int fd;
    char *fname;

    fname = make_filename(INDEX_RANDSEED, NULL);
    /*
     * Don't truncate the random seed file if it already exists; if
     * something goes wrong half way through writing it, it would
     * be better to leave the old data there than to leave it empty.
     */
    fd = open(fname, O_CREAT | O_WRONLY, 0600);
    if (fd < 0) {
        if (errno != ENOENT) {
            nonfatal("Unable to write random seed: open(\"%s\") "
                     "returned '%s'", fname, strerror(errno));
            sfree(fname);
            return;
        }
        char *dir, *errmsg;

        dir = make_filename(INDEX_DIR, NULL);
        if ((errmsg = make_dir_path(dir, 0700)) != NULL) {
            nonfatal("Unable to write random seed: %s", errmsg);
            sfree(errmsg);
            sfree(fname);
            sfree(dir);
            return;
        }
        sfree(dir);

        fd = open(fname, O_CREAT | O_WRONLY, 0600);
        if (fd < 0) {
            nonfatal("Unable to write random seed: open(\"%s\") "
                     "returned '%s'", fname, strerror(errno));
            sfree(fname);
            return;
        }
    }

    while (len > 0) {
        int ret = write(fd, data, len);
        if (ret < 0) {
            nonfatal("Unable to write random seed: write "
                     "returned '%s'", strerror(errno));
            break;
        }
        len -= ret;
        data = (char *)data + ret;
    }

    close(fd);
    sfree(fname);
}

void cleanup_all(void)
{
}
