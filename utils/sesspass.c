/*
 * sesspass.c: the pure parts of the saved-session password's stored form.
 *
 * See sesspass.h for what this is for. Nothing here does any I/O, so it can
 * be tested without a configuration directory or a front end.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "defs.h"
#include "misc.h"
#include "utils/sesspass.h"

char *sesspass_blob_new(const char *plain)
{
    strbuf *sb;
    char *ret;

    if (!plain || !*plain)
        return dupstr("");         /* "nothing stored" */

    /*
     * cpl = 0 asks for base64 with no line wrapping. That matters: the blob
     * goes into a settings file, whose format is one "key=value" per line, so
     * a newline would truncate it. Plain base64 is also free of '\\', which
     * keeps it clear of the comment-escape convention used beside it.
     */
    sb = base64_encode_sb(ptrlen_from_asciz(plain), 0);

    {
        strbuf *out = strbuf_new();
        put_dataz(out, SESSPASS_PREFIX);
        put_data(out, sb->s, sb->len);
        ret = strbuf_to_str(out);
    }

    strbuf_free(sb);
    return ret;
}

char *sesspass_blob_open(const char *blob)
{
    const size_t plen = strlen(SESSPASS_PREFIX);
    ptrlen body;
    strbuf *sb;
    char *ret;

    if (!blob || !*blob)
        return dupstr("");         /* nothing stored */

    if (strncmp(blob, SESSPASS_PREFIX, plen) != 0) {
        if (sesspass_debug_enabled())
            fprintf(stderr, "sesspass: blob \"%.8s...\" has an unknown "
                    "version tag\n", blob);
        return NULL;               /* some future, or damaged, version */
    }

    body = ptrlen_from_asciz(blob + plen);
    if (!base64_valid(body)) {
        if (sesspass_debug_enabled())
            fprintf(stderr, "sesspass: body of the blob is not valid "
                    "base64\n");
        return NULL;
    }

    sb = base64_decode_sb(body);
    /*
     * The password was typed text, so it holds no NUL and dupstr() is safe.
     */
    ret = dupstr(sb->s);
    strbuf_free(sb);
    return ret;
}

bool sesspass_debug_enabled(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = (getenv("PUTTY_SESSPASS_DEBUG") != NULL);
    return cached != 0;
}
