/*
 * sesssearch.c: the pure parts of the saved-session search.
 *
 * See sesssearch.h for what this is for. Nothing here does any I/O, so it can
 * be tested without a configuration directory or a front end.
 */

#include <stdlib.h>
#include <string.h>

#include "defs.h"
#include "misc.h"
#include "utils/sesssearch.h"

const char *sesssearch_query(const char *box)
{
    if (box && box[0] && strchr(SESSSEARCH_PREFIX_CHARS, box[0]))
        return box + 1;
    return NULL;
}

char *sesssearch_haystack(const char *name, const char *host,
                          const char *comment)
{
    const char *const parts[3] = { name, host, comment };
    char *hay, *p;

    /*
     * Every byte is either kept as it is or lower-cased, so the result is no
     * longer than the pieces plus one newline and one terminator each.
     */
    size_t len = 1;
    for (size_t i = 0; i < lenof(parts); i++)
        if (parts[i])
            len += strlen(parts[i]) + 1;

    p = hay = snewn(len, char);

    for (size_t i = 0; i < lenof(parts); i++) {
        const unsigned char *s = (const unsigned char *)parts[i];

        if (i > 0)
            *p++ = '\n';
        while (s && *s) {
            unsigned char c = *s++;
            if (c >= 'A' && c <= 'Z')
                c = c - 'A' + 'a';
            *p++ = c;
        }
    }
    *p = '\0';

    return hay;
}

bool sesssearch_match(const char *haystack, const char *query)
{
    if (!query || !*query)
        return true;               /* "search for anything" */
    if (!haystack)
        return false;

    for (; *haystack; haystack++) {
        const unsigned char *h = (const unsigned char *)haystack;
        const unsigned char *q = (const unsigned char *)query;

        while (*q) {
            unsigned char hc = *h, qc = *q;

            if (!hc)
                break;             /* ran off the end: no match here */
            if (hc >= 'A' && hc <= 'Z')
                hc = hc - 'A' + 'a';
            if (qc >= 'A' && qc <= 'Z')
                qc = qc - 'A' + 'a';
            if (hc != qc)
                break;

            h++;
            q++;
        }

        if (!*q)
            return true;           /* the whole query matched */
    }

    return false;
}

bool sesssearch_debug_enabled(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = (getenv("PUTTY_SESS_SEARCH_DEBUG") != NULL);
    return cached != 0;
}
