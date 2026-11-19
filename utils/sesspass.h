/*
 * sesspass.h: the stored form of a saved session's password.
 *
 * A saved session can carry its login password, so that connecting to it does
 * not have to ask for one. What goes into the session file is not the password
 * itself but a small, self-describing blob:
 *
 *     SessionPassword=v0;<base64 of the password>
 *
 * The "v0;" tag says how the rest is encoded, so that a later version can
 * encrypt the password under a master passphrase (PLAN.md section 4k) without
 * making files written by this build unreadable: a reader that does not
 * understand a version says so, rather than trying to guess.
 *
 * The encoding in v0 is a wrapper, not a lock: it keeps the password from
 * being readable at a glance (or found by a plain grep), and it keeps the
 * settings-file format safe, but anyone who can read the file can decode it.
 * Section 4k is where the real protection is planned.
 *
 * These are the pure parts - no I/O, no GTK, no globals - so that they can be
 * tested on their own. Reading the blob out of the session file is
 * settings.c's job; deciding when to use the password is the front end's.
 *
 * Set PUTTY_SESSPASS_DEBUG=1 in the environment to trace a decode.
 */

#ifndef PUTTY_SESSPASS_H
#define PUTTY_SESSPASS_H

#include <stdbool.h>

/* PuTTY-beter: the stored form of a saved session's password. See
 * utils/sesspass.c for the whole story. */

/* The version tag this build writes. Always followed by base64. */
#define SESSPASS_PREFIX "v0;"

/*
 * Encode `plain` into the stored form. Returns a string the caller must free.
 * An empty or NULL password becomes an empty string, which the callers treat
 * as "nothing stored".
 */
char *sesspass_blob_new(const char *plain);

/*
 * Decode a stored blob back into the password. Returns a string the caller
 * must free, or NULL if the blob is not one this build understands (an
 * unknown version tag, or the body is not valid base64).
 *
 * NULL, "" and a bare "v0;" all decode to a password of "", not NULL: an
 * empty blob is a legitimate "no password", whereas NULL means "written by a
 * different version, or damaged" - worth telling apart, so the front end can
 * say "this file needs a newer PuTTY-beter" instead of silently ignoring it.
 */
char *sesspass_blob_open(const char *blob);

/* True if the debug switch is set; for the caller to trace without duplicating
 * the getenv() call. */
bool sesspass_debug_enabled(void);

#endif /* PUTTY_SESSPASS_H */
