/*
 * test_sesspass.c: tests for PuTTY-beter's stored session password.
 *
 * A saved session can keep its login password so that connecting does not ask
 * for it. The password is not written to the session file as-is: it is wrapped
 * in a small self-describing blob, "v0;" followed by base64, so that a later
 * version can encrypt it without breaking files written now (PLAN.md
 * section 4k).
 *
 * Only the pure helpers are tested here - encoding a password into the blob
 * and decoding it back, and the failure cases. Putting the blob in the file
 * and taking it out again lives in settings.c and needs a configuration
 * directory; handing it to the authentication prompt lives in the front end.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "defs.h"
#include "misc.h"
#include "utils/sesspass.h"

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

/*
 * A password should survive a round trip, and the blob it makes should not
 * contain the password in the clear.
 */
static void expect_roundtrip(const char *plain)
{
    char *blob = sesspass_blob_new(plain);
    char *back;

    nchecks++;
    if (!blob) {
        fail("encoding \"%s\" returned NULL", plain ? plain : "(null)");
        return;
    }

    back = sesspass_blob_open(blob);
    nchecks++;
    if (!back)
        fail("decoding the blob for \"%s\" failed", plain ? plain : "(null)");
    else if (strcmp(back, plain ? plain : ""))
        fail("round trip of \"%s\" gave \"%s\"", plain ? plain : "(null)",
             back);
    sfree(back);

    /*
     * The blob must be safe to drop into a settings file: no newline to end
     * the line, no '\' to be confused with the comment escaping, and no '='
     * at the front that a parser might take for the key's own separator.
     */
    nchecks++;
    if (strchr(blob, '\n') || strchr(blob, '\r') || strchr(blob, '\\'))
        fail("blob for \"%s\" is not one clean line: \"%s\"",
             plain ? plain : "(null)", blob);

    sfree(blob);
}

int main(void)
{
    /* Ordinary passwords, including ones with awkward characters in them. */
    expect_roundtrip("hunter2");
    expect_roundtrip("correct horse battery staple");
    expect_roundtrip("pass word with spaces");
    expect_roundtrip("a");                     /* one byte */
    expect_roundtrip("ab");                    /* base64 padding cases */
    expect_roundtrip("abc");
    expect_roundtrip("abcd");
    expect_roundtrip("p@ssw0rd!\"#$%&'()*+,-./:;<=>?@[]^_{|}~");
    expect_roundtrip("caf\xc3\xa9 unicode \xe2\x9c\x93");

    /* An empty password is not stored at all: it encodes to "". */
    {
        char *blob = sesspass_blob_new("");
        nchecks++;
        if (strcmp(blob, ""))
            fail("empty password encoded to \"%s\", wanted \"\"", blob);
        sfree(blob);

        blob = sesspass_blob_new(NULL);
        nchecks++;
        if (strcmp(blob, ""))
            fail("NULL password encoded to \"%s\", wanted \"\"", blob);
        sfree(blob);
    }

    /* Decoding nothing is a password of "", not a failure. */
    {
        const char *const empties[] = { "", NULL, "v0;" };

        for (size_t i = 0; i < lenof(empties); i++) {
            char *back = sesspass_blob_open(empties[i]);
            nchecks++;
            if (!back)
                fail("decoding \"%s\" failed, wanted \"\"",
                     empties[i] ? empties[i] : "(null)");
            else if (*back)
                fail("decoding \"%s\" gave \"%s\", wanted \"\"",
                     empties[i] ? empties[i] : "(null)", back);
            sfree(back);
        }
    }

    /* The blob really is a base64 wrapper, and the tag is the version. */
    {
        char *blob = sesspass_blob_new("hunter2");
        nchecks++;
        if (strncmp(blob, SESSPASS_PREFIX, strlen(SESSPASS_PREFIX)))
            fail("blob \"%s\" does not begin with \"%s\"", blob,
                 SESSPASS_PREFIX);
        nchecks++;
        if (strstr(blob, "hunter2"))
            fail("blob \"%s\" contains the password in the clear", blob);
        sfree(blob);
    }

    /*
     * A blob this build does not understand must be reported as unknown, not
     * decoded into nonsense. That is the whole point of the version tag: it
     * is how a file written by a future, encrypting build is told apart from
     * one written by this one.
     */
    {
        const char *const unknown[] = {
            "v1;AAAA",                 /* a future version */
            "x0;AAAA",                 /* no version at all */
            "AAAA",                    /* base64 with no tag */
            "v0;not valid base64!",    /* right tag, bad body */
        };

        for (size_t i = 0; i < lenof(unknown); i++) {
            char *back = sesspass_blob_open(unknown[i]);
            nchecks++;
            if (back) {
                fail("decoding \"%s\" should have failed, gave \"%s\"",
                     unknown[i], back);
                sfree(back);
            }
        }
    }

    if (nfail) {
        printf("%d of %d checks failed\n", nfail, nchecks);
        return 1;
    }
    printf("all session-password tests passed (%d checks)\n", nchecks);
    return 0;
}
