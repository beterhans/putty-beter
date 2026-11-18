/*
 * test_sesssearch.c: tests for PuTTY-beter's saved-session search.
 *
 * The search is triggered by typing '/' (or '\\') in the "Saved Sessions" box
 * of the configuration dialog; the rest of the line is a query that the
 * session list is then filtered by. A session matches if its name, its host
 * name or its comment contains the query, case-insensitively.
 *
 * Only the pure helpers are tested here - building the text a session is
 * searched by, and matching against it. The file reading and the list
 * refilling live in config.c and need the dialog.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "defs.h"
#include "misc.h"
#include "utils/sesssearch.h"

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

/* ---- test scaffolding ------------------------------------------------- */

static int nfail = 0;
static int nchecks = 0;

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

/* What the box being typed in means: a search, and with what query. */
static void expect_query(const char *box, const char *want)
{
    const char *got = sesssearch_query(box);

    nchecks++;
    if (!want) {
        if (got)
            fail("box \"%s\" should not be a search (got query \"%s\")",
                 box, got);
        return;
    }
    if (!got)
        fail("box \"%s\" should be a search for \"%s\"", box, want);
    else if (strcmp(got, want))
        fail("box \"%s\": query \"%s\", wanted \"%s\"", box, got, want);
}

static void expect_match(const char *haystack, const char *query, bool want)
{
    bool got = sesssearch_match(haystack, query);

    nchecks++;
    if (got != want)
        fail("\"%s\" containing \"%s\": got %s, wanted %s",
             haystack, query, got ? "yes" : "no", want ? "yes" : "no");
}

int main(void)
{
    /*
     * What counts as a search. A session name is a '/'-separated path, and
     * no valid path begins with '/', so a leading '/' is free to mean
     * "search"; '\\' can never appear in a session name at all.
     */
    expect_query("", NULL);
    expect_query("Work/beta", NULL);       /* an ordinary session name */
    expect_query("/", "");                 /* search mode, nothing typed yet */
    expect_query("/beta", "beta");
    expect_query("\\beta", "beta");
    expect_query("/10.0.1", "10.0.1");
    expect_query("/ Uppercase", " Uppercase");
    expect_query(" /spaced", NULL);        /* the prefix must be first */

    /* The text a session is searched by: name, host, comment, folded. */
    {
        char *hay = sesssearch_haystack("Work/Beta", "10.0.1.7",
                                        "Prod box, ask Dana");
        nchecks++;
        if (strcmp(hay, "work/beta\n10.0.1.7\nprod box, ask dana"))
            fail("haystack was \"%s\"", hay);
        expect_match(hay, "", true);
        expect_match(hay, "beta", true);
        expect_match(hay, "BETA", true);
        expect_match(hay, "WORK/beta", true);   /* the name, whole path */
        expect_match(hay, "10.0.1.7", true);
        expect_match(hay, "10.0.1", true);      /* a prefix of the host */
        expect_match(hay, "dana", true);
        expect_match(hay, "DANA", true);        /* comments fold too */
        expect_match(hay, "prod box, ask", true);
        expect_match(hay, "zzz", false);
        expect_match(hay, "0.1.7 work", false); /* not across the joins */
        sfree(hay);
    }

    /* Missing host or comment: NULL is an empty part, not a crash. */
    {
        char *hay = sesssearch_haystack("Alpha", NULL, NULL);
        nchecks++;
        if (strcmp(hay, "alpha\n\n"))
            fail("haystack without host/comment was \"%s\"", hay);
        expect_match(hay, "alpha", true);
        expect_match(hay, "beta", false);
        sfree(hay);
    }

    /*
     * A folder-name query finds everything inside it, because a session is
     * searched by its whole path: "Work" is in "Work/beta".
     */
    {
        char *hay = sesssearch_haystack("Work/beta", "beta.example.com",
                                        "a note");
        expect_match(hay, "work", true);
        expect_match(hay, "work/", true);
        expect_match(hay, "k/b", true);        /* spanning the component */
        sfree(hay);
    }

    /* An empty or NULL query matches anything; NULL text matches nothing. */
    expect_match("anything", "", true);
    expect_match("anything", NULL, true);
    expect_match(NULL, "", true);
    expect_match(NULL, "x", false);

    /* A query longer than the text, and one that runs off the end. */
    expect_match("ab", "abc", false);
    expect_match("abc", "abc", true);
    expect_match("ab", "b", true);

    if (nfail) {
        printf("%d of %d checks failed\n", nfail, nchecks);
        return 1;
    }
    printf("all session-search tests passed (%d checks)\n", nchecks);
    return 0;
}
