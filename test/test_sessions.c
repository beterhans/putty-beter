/*
 * test_sessions.c: tests for PuTTY-beter's session folders.
 *
 * A session name may contain '/', which separates folder names: "Work/beta"
 * is a session inside the folder "Work", and folders are real directories
 * inside the sessions directory. get_sesslist() reports a folder as a name
 * with a trailing '/'.
 *
 * Everything happens in a private directory chosen by PUTTYDIR, so running
 * this never touches a real PuTTY configuration.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>

#include "putty.h"
#include "storage.h"

/* ---- the platform hooks this program does not otherwise need ---------- */

void modalfatalbox(const char *p, ...)
{
    va_list ap;
    fprintf(stderr, "FATAL ERROR: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

void nonfatal(const char *p, ...)
{
    va_list ap;
    fprintf(stderr, "nonfatal: ");
    va_start(ap, p);
    vfprintf(stderr, p, ap);
    va_end(ap);
    fputc('\n', stderr);
}

char *x_get_default(const char *key) { return NULL; }
char *platform_default_s(const char *name) { return NULL; }
bool platform_default_b(const char *name, bool def) { return def; }
int platform_default_i(const char *name, int def) { return def; }
FontSpec *platform_default_fontspec(const char *name)
{ return fontspec_new_default(); }
Filename *platform_default_filename(const char *name)
{ return filename_from_str(""); }
char *platform_get_x_display(void) { return NULL; }

const bool share_can_be_upstream = false;
const bool share_can_be_downstream = false;

/* No backends or GSS libraries are linked in, but settings.c refers to
 * both lists when saving and loading. */
const int ngsslibs = 0;
const struct keyvalwhere gsslibkeywords[] = { { "", 0, 0, 0 } };

const struct BackendVtable *const backends[] = { NULL };
const size_t n_ui_backends = 0;
const int be_default_protocol = 0;

/*
 * unix/unicode.c is not linked into this program, but the charset
 * marshalling helpers in utils/ refer to these two. Nothing here converts
 * anything, so they never need to do any work.
 */
bool BinarySink_put_mb_to_wc(BinarySink *bs, int codepage,
                             const char *mbstr, int mblen)
{ return true; }
bool BinarySink_put_wc_to_mb(BinarySink *bs, int codepage,
                             const wchar_t *wcstr, int wclen,
                             const char *defchr)
{ return true; }

/* ---- test scaffolding ------------------------------------------------- */

static int nfail = 0;

static void fail(const char *fmt, ...)
{
    va_list ap;
    nfail++;
    fprintf(stderr, "fail: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* Save a session under `name`, with a distinguishing HostName. */
static void save(const char *name, const char *host)
{
    char *errmsg;
    settings_w *w = open_settings_w(name, &errmsg);

    if (!w) {
        fail("open_settings_w(\"%s\"): %s", name, errmsg ? errmsg : "?");
        sfree(errmsg);
        return;
    }
    write_setting_s(w, "HostName", host);
    close_settings_w(w);
}

/* Is `name` in the session list? */
static bool listed(const char *name)
{
    struct sesslist sl;
    bool found = false;

    get_sesslist(&sl, true);
    for (int i = 0; i < sl.nsessions; i++)
        if (!strcmp(sl.sessions[i], name))
            found = true;
    get_sesslist(&sl, false);

    return found;
}

static void expect_listed(const char *name, bool want)
{
    bool got = listed(name);
    if (got != want)
        fail("%s should %sbe listed", name, want ? "" : "not ");
}

/* Does the given path (relative to the sessions directory) exist? */
static bool exists(const char *rel)
{
    char *dir = getenv("PUTTYDIR");
    char *path = dupprintf("%s/sessions/%s", dir, rel);
    struct stat st;
    bool ret = (stat(path, &st) == 0);

    sfree(path);
    return ret;
}

static void expect_err(const char *what, const char *err)
{
    if (!err)
        fail("%s should have failed", what);
    else
        sfree(err);
}

int main(void)
{
    char tmpl[] = "/tmp/putty-sessions-XXXXXX";
    char *dir = mkdtemp(tmpl);

    if (!dir) {
        perror("mkdtemp");
        return 1;
    }
    setenv("PUTTYDIR", dir, 1);
    printf("test configuration directory: %s\n", dir);

    /* Nothing at all to begin with. */
    expect_listed("Default Settings", true);   /* always claimed to exist */
    expect_listed("Alpha", false);

    /* A plain session, as before. */
    save("Alpha", "alpha.example.com");
    expect_listed("Alpha", true);
    if (!exists("Alpha"))
        fail("session file Alpha should exist");

    /* A folder, empty at first. */
    {
        char *err = session_mkdir("Work");
        if (err) { fail("session_mkdir(Work): %s", err); sfree(err); }
    }
    expect_listed("Work/", true);              /* folders end in '/' */
    if (!exists("Work"))
        fail("folder Work should exist on disk");

    /* A session inside it: the folder must exist as a directory. */
    save("Work/beta", "beta.example.com");
    expect_listed("Work/beta", true);
    if (!exists("Work/beta"))
        fail("session file Work/beta should exist");

    /* Nesting. */
    {
        char *err = session_mkdir("Work/Sub");
        if (err) { fail("session_mkdir(Work/Sub): %s", err); sfree(err); }
    }
    save("Work/Sub/gamma", "gamma.example.com");
    expect_listed("Work/Sub/", true);
    expect_listed("Work/Sub/gamma", true);

    /* Odd characters, including spaces, inside a folder. */
    save("Work/My Host (1)", "spaces.example.com");
    expect_listed("Work/My Host (1)", true);

    /* Creating a folder that is already there. */
    expect_err("session_mkdir(Work) a second time", session_mkdir("Work"));

    /* Moving a session out of a folder, as a drag to the root would. */
    {
        char *err = session_move("Work/beta", "beta");
        if (err) { fail("session_move(Work/beta -> beta): %s", err); sfree(err); }
    }
    expect_listed("Work/beta", false);
    expect_listed("beta", true);
    if (!exists("beta") || exists("Work/beta"))
        fail("Work/beta should have moved to beta");

    /* Moving onto an existing name must be refused, not clobber it. */
    expect_err("session_move(beta -> Alpha)", session_move("beta", "Alpha"));
    expect_listed("Alpha", true);
    expect_listed("beta", true);

    /* Moving into a folder. */
    {
        char *err = session_move("beta", "Work/beta");
        if (err) { fail("session_move(beta -> Work/beta): %s", err); sfree(err); }
    }
    expect_listed("Work/beta", true);

    /* Moving a whole folder into another folder, as a drag would. */
    {
        char *err = session_mkdir("Other");
        if (err) { fail("session_mkdir(Other): %s", err); sfree(err); }
    }
    save("Other/stays", "stays.example.com");
    {
        char *err = session_move("Work/Sub/", "Other/Sub/");
        if (err) { fail("session_move(Work/Sub/ -> Other/Sub/): %s", err);
                   sfree(err); }
    }
    expect_listed("Work/Sub/", false);
    expect_listed("Other/Sub/", true);
    expect_listed("Other/Sub/gamma", true);    /* contents moved with it */
    if (!exists("Other/Sub/gamma") || exists("Work/Sub"))
        fail("Work/Sub should have moved into Other");

    /* A folder must not be moved inside itself. */
    expect_err("session_move(Other/Sub/ into itself)",
               session_move("Other/Sub/", "Other/Sub/Inner/"));
    expect_listed("Other/Sub/", true);

    /* Moving a folder back out to the top level. */
    {
        char *err = session_move("Other/Sub/", "Sub/");
        if (err) { fail("session_move(Other/Sub/ -> Sub/): %s", err);
                   sfree(err); }
    }
    expect_listed("Sub/", true);
    expect_listed("Sub/gamma", true);
    {
        char *err = session_move("Sub/", "Work/Sub/");
        if (err) { fail("session_move(Sub/ -> Work/Sub/): %s", err);
                   sfree(err); }
    }
    expect_listed("Work/Sub/gamma", true);

    /* Tidying up the extra folder from this section. */
    del_settings("Other/stays");
    {
        char *err = session_rmdir("Other");
        if (err) { fail("session_rmdir(Other): %s", err); sfree(err); }
    }

    /* Deleting a session inside a folder. */
    del_settings("Work/Sub/gamma");
    expect_listed("Work/Sub/gamma", false);
    expect_listed("Work/Sub/", true);          /* the folder remains */

    /* Removing a folder that still has things in it. */
    expect_err("session_rmdir(Work) while it has sessions",
               session_rmdir("Work"));

    /* Removing an empty folder, with and without the trailing slash. */
    {
        char *err = session_rmdir("Work/Sub/");
        if (err) { fail("session_rmdir(Work/Sub/): %s", err); sfree(err); }
    }
    expect_listed("Work/Sub/", false);

    del_settings("Work/beta");
    del_settings("Work/My Host (1)");
    {
        char *err = session_rmdir("Work");
        if (err) { fail("session_rmdir(Work): %s", err); sfree(err); }
    }
    expect_listed("Work/", false);

    /* A folder name cannot escape the sessions directory. */
    {
        char *err = session_mkdir("../escaped");
        sfree(err);                            /* may fail; what matters is */
        if (exists("../escaped"))
            fail("session_mkdir(../escaped) escaped the sessions directory");
    }
    /* Saving such a name must be refused outright. */
    {
        char *errmsg = NULL;
        settings_w *w = open_settings_w("../evil", &errmsg);
        if (w) {
            fail("saving \"../evil\" should have been refused");
            close_settings_w(w);
        }
        sfree(errmsg);
    }
    if (exists("../evil") || exists("..%2Fevil"))
        fail("saving \"../evil\" created a file anyway");
    expect_listed("../evil", false);
    if (session_name_is_valid("../evil"))
        fail("\"../evil\" should be an invalid session name");
    if (session_name_is_valid("Work//beta"))
        fail("\"Work//beta\" should be an invalid session name");
    if (!session_name_is_valid("Work/beta"))
        fail("\"Work/beta\" should be a valid session name");

    /* "Default Settings" still works, and stays top level. */
    save("Default Settings", "default.example.com");
    expect_listed("Default Settings", true);

    /* Force-removing a folder and everything in it. */
    {
        char *err;
        int n;

        err = session_mkdir("Doomed");
        if (err) { fail("session_mkdir(Doomed): %s", err); sfree(err); }
        save("Doomed/one", "one.example.com");
        save("Doomed/two", "two.example.com");
        err = session_mkdir("Doomed/Inner");
        if (err) { fail("session_mkdir(Doomed/Inner): %s", err); sfree(err); }
        save("Doomed/Inner/three", "three.example.com");

        n = session_folder_entries("Doomed/");
        if (n != 3)
            fail("Doomed should hold 3 things, counted %d", n);
        if (session_folder_entries("Alpha") != -1)
            fail("a session should not count as a folder");
        if (session_folder_entries("No Such Folder") != -1)
            fail("a missing folder should count as -1");

        /* Plain rmdir refuses while it has anything in it. */
        expect_err("session_rmdir(Doomed)", session_rmdir("Doomed"));
        expect_listed("Doomed/one", true);

        /* The forced version takes the lot. */
        err = session_rmdir_force("Doomed/");
        if (err) { fail("session_rmdir_force(Doomed/): %s", err); sfree(err); }
        expect_listed("Doomed/", false);
        expect_listed("Doomed/one", false);
        expect_listed("Doomed/Inner/three", false);
        if (exists("Doomed"))
            fail("the Doomed folder should be gone from disk");

        /* And refuses a name that is not a folder. */
        expect_err("session_rmdir_force(Alpha)", session_rmdir_force("Alpha"));
        expect_err("session_rmdir_force(nothing)",
                   session_rmdir_force("Not There"));
    }

    /* The names a drag between rows would produce. */
    {
        static const struct {
            const char *src, *dst, *want;
        } cases[] = {
            { "beta", "Work/", "Work/beta" },      /* into a folder */
            { "Work/beta", NULL, "beta" },         /* out to the top level */
            { "Work/beta", "Other/", "Other/beta" },
            { "Work/Sub/", "Other/", "Other/Sub/" }, /* folders keep their / */
            { "Work/Sub/", NULL, "Sub/" },
            { "Work/Sub/gamma", "Home Routers/", "Home Routers/gamma" },
        };

        for (size_t i = 0; i < lenof(cases); i++) {
            char *got = session_move_target_name(cases[i].src, cases[i].dst);
            if (strcmp(got, cases[i].want))
                fail("move %s into %s: got \"%s\", wanted \"%s\"",
                     cases[i].src, cases[i].dst ? cases[i].dst : "(root)",
                     got, cases[i].want);
            sfree(got);
        }
    }

    if (nfail) {
        printf("%d failure(s)\n", nfail);
        return 1;
    }
    printf("all session-folder tests passed\n");
    return 0;
}
