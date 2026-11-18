/*
 * sesssearch.h: searching the saved-session list by name, host or comment.
 *
 * The configuration box's "Saved Sessions" box doubles as a search box: if
 * what is typed starts with '/' (or '\\'), the rest of the line is a query
 * and the session list below is filtered down to the sessions that match.
 *
 * These are the pure parts of that - no I/O, no GTK, no globals - so that
 * they can be tested on their own. Reading the host name and the comment out
 * of the session files, and caching the result, is the caller's job.
 *
 * Set PUTTY_SESS_SEARCH_DEBUG=1 in the environment to trace a search.
 */

#ifndef PUTTY_SESSSEARCH_H
#define PUTTY_SESSSEARCH_H

#include <stdbool.h>

/* PuTTY-beter: the configuration box's saved-session search. See
 * utils/sesssearch.c for the whole story. */

#define SESSSEARCH_PREFIX_CHARS "/\\"

/*
 * Is what is in the box a search? Returns the query - a pointer into `box`,
 * just past the prefix character - or NULL if `box` is an ordinary session
 * name. An empty query is still a search (it matches everything).
 */
const char *sesssearch_query(const char *box);

/*
 * The text a session is searched by: its name, its host name and its comment,
 * joined with newlines and folded to lower case. Returns a string the caller
 * must free. NULL fields are treated as empty.
 */
char *sesssearch_haystack(const char *name, const char *host,
                          const char *comment);

/*
 * Does `haystack` (as built by sesssearch_haystack, or any text) contain
 * `query`? An empty query matches anything. Case-insensitive, ASCII only.
 */
bool sesssearch_match(const char *haystack, const char *query);

/* True if a search is switched on (the debug switch); for the caller to trace
 * without duplicating the getenv() call. */
bool sesssearch_debug_enabled(void);

#endif /* PUTTY_SESSSEARCH_H */
