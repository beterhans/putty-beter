/*
 * windows/utils/defaults.c: default settings that are specific to Windows.
 */

#include "putty.h"

#include <commctrl.h>

/*
 * PuTTY-beter: where default file names live.
 *
 * The configuration folder is the front end's business - it can be relocated -
 * and this file is part of utils/, which is linked by tools that have no
 * configuration folder at all (plink, pscp, psftp, pageant, puttygen and the
 * test programs). So rather than make utils/ depend on the settings library,
 * the front end installs this hook at startup. NULL means "no configuration
 * folder", which is what those tools want.
 */
char *(*platform_default_filename_dir)(void) = NULL;

/*
 * The user's profile directory, as a run-time variable rather than a literal:
 * %USERPROFILE% is whatever the account actually uses, which is not always on
 * C: (a profile can live on another drive, or be redirected). A GUI logon
 * always has it set.
 */
static char *profile_dir(void)
{
    const char *env = getenv("USERPROFILE");

    if (env && *env)
        return dupstr(env);
    return NULL;
}

/*
 * PuTTY-beter: make a path read like a Windows path.
 *
 * The shared storage code builds paths POSIX-style, with '/' separators, so
 * that a configuration folder is laid out identically on a Mac and a PC. The
 * Windows APIs are happy with either separator, but the log file name and the
 * transfer folder are both *shown* in the configuration box, and there is no
 * good reason to show a user "C:\Users\me/Documents/..." with two kinds of
 * slash in it. Rewriting in place is safe: the string is one we allocated,
 * and any backslash already in it (a drive letter, a UNC prefix) is left
 * alone.
 */
static void win_native_path(char *path)
{
    for (; *path; path++)
        if (*path == '/')
            *path = '\\';
}

FontSpec *platform_default_fontspec(const char *name)
{
    if (!strcmp(name, "Font"))
        return fontspec_new("Courier New", false, 10, ANSI_CHARSET);
    else
        return fontspec_new_default();
}

Filename *platform_default_filename(const char *name)
{
    if (!strcmp(name, "LogFileName")) {
        /*
         * PuTTY-beter: log into <configuration folder>/logs by default, with a
         * dated, per-host file name - the same choice the macOS front end
         * makes. Asking the hook means the logs follow any config-location
         * redirect, so they move with the configuration. Tools with no
         * configuration folder keep the old bare "putty.log".
         */
        char *dir = platform_default_filename_dir ?
            platform_default_filename_dir() : NULL;

        if (dir) {
            char *path = dupprintf("%s/logs/&Y-&M-&D_&T-&H.txt", dir);
            Filename *fn;
            win_native_path(path);
            fn = filename_from_str(path);
            if (getenv("PUTTY_CONFIG_DEBUG"))
                fprintf(stderr, "[config-location] default log file: %s\n",
                        path);
            sfree(path);
            sfree(dir);
            return fn;
        }
        return filename_from_str("putty.log");
    } else if (!strcmp(name, "ZmodemDirectory")) {
        /*
         * PuTTY-beter: ZMODEM transfers default to the user's Downloads
         * folder, the same choice macOS makes and for the same reason: it is a
         * place the user already knows, and unlike the log folder it must not
         * move if the configuration folder is relocated - a download should
         * not end up somewhere else because the configuration moved.
         *
         * Resolved from %USERPROFILE% at run time, so a profile on another
         * drive is followed.
         */
        char *profile = profile_dir();

        if (profile) {
            char *path = dupprintf("%s/Downloads", profile);
            Filename *fn;
            win_native_path(path);
            fn = filename_from_str(path);
            if (getenv("PUTTY_CONFIG_DEBUG"))
                fprintf(stderr, "[config-location] default transfer dir: %s\n",
                        path);
            sfree(path);
            sfree(profile);
            return fn;
        }
        return filename_from_str("");
    } else {
        return filename_from_str("");
    }
}

char *platform_default_s(const char *name)
{
    if (!strcmp(name, "SerialLine"))
        return dupstr("COM1");
    return NULL;
}

bool platform_default_b(const char *name, bool def)
{
    return def;
}

int platform_default_i(const char *name, int def)
{
    return def;
}
