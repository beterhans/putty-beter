/*
 * ZMODEM (sz/rz) file transfer.
 *
 * The engine is deliberately free of any front end, display or filesystem
 * dependency: it reaches the outside world only through the callbacks in
 * ZmodemCallbacks. That keeps it unit-testable without a display or a server,
 * and means another front end could adopt it unchanged.
 *
 * The split of responsibility is:
 *
 *  - The front end hands the whole incoming stream to zmodem_feed_in() and
 *    does not decide anything itself: the engine either consumes the bytes as
 *    protocol or passes them back through the terminal_output callback. So a
 *    front end with no engine (a NULL Zmodem) behaves exactly as upstream.
 *  - The front end routes keystrokes to zmodem_key() while a transfer is
 *    running, because typed bytes are not protocol and would corrupt it.
 *  - The engine asks for bytes to send and for files to read or write, and
 *    reports progress and the final outcome.
 *
 * Licensing: this is an original implementation written from the published
 * protocol description and the public-domain original rzsz, and is covered by
 * PuTTY's MIT licence. See PLAN.md section 6 for why lrzsz (GPL-2.0) must not
 * be used as a source.
 */

#ifndef PUTTY_ZMODEM_H
#define PUTTY_ZMODEM_H

#include "defs.h"

typedef struct Zmodem Zmodem;

/* Which way the data is going, from this machine's point of view. */
typedef enum ZmodemDirection {
    ZMDIR_RECEIVE,   /* the far end ran sz: we receive and save files */
    ZMDIR_SEND,      /* the far end ran rz: we send a local file */
} ZmodemDirection;

/* How a transfer ended, so the front end can say so. */
typedef enum ZmodemOutcome {
    ZMOUT_DONE,       /* the transfer completed */
    ZMOUT_CANCELLED,  /* the user or the far end stopped it */
    ZMOUT_FAILED,     /* protocol or I/O error; `message' says what */
} ZmodemOutcome;

/*
 * An open local file, as the engine sees it. The engine never knows what is
 * behind the pointer - a stdio FILE, an in-memory buffer in a test, whatever
 * the front end likes.
 */
typedef struct ZmodemFile ZmodemFile;

typedef struct ZmodemCallbacks {
    /* Passed back to every callback, so the front end can find its state. */
    void *ctx;

    /*
     * Send raw bytes to the far end. This must NOT go through the line
     * discipline: during a transfer the bytes are protocol, not typed input,
     * and local echo or line editing would corrupt them.
     */
    void (*send)(void *ctx, const void *data, size_t len);

    /*
     * Ordinary terminal output. The engine calls this for every byte that is
     * not protocol, including bytes it had to hold back to see whether a
     * prologue was starting - so the caller never has to reconcile anything,
     * and a stream with no transfer in it arrives here byte for byte.
     */
    void (*terminal_output)(void *ctx, const void *data, size_t len);

    /*
     * A file is about to be received. `name' is what the far end called it,
     * and `size' is what it claims (which may be 0, or wrong). Return NULL to
     * refuse it, and the engine will tell the far end to skip the file.
     *
     * This is where the front end deals with the name already existing -
     * asking the user, renaming, or choosing to overwrite.
     */
    ZmodemFile *(*receive_open)(void *ctx, const char *name, uint64_t size,
                                unsigned long mtime);
    void (*receive_write)(void *ctx, ZmodemFile *f, const void *data,
                          size_t len);
    /*
     * `complete' is false if the transfer was cut short, so the front end can
     * remove a partial file rather than leave a truncated one behind.
     */
    void (*receive_close)(void *ctx, ZmodemFile *f, bool complete);

    /*
     * A file is about to be sent. The front end asks the user which one and
     * opens it, storing the remote file name in *name and the size and
     * modification time in *size and *mtime. Return NULL if the user
     * cancelled.
     */
    ZmodemFile *(*send_open)(void *ctx, char **name, uint64_t *size,
                             unsigned long *mtime);
    /*
     * Return 0 at end of file. A short read - fewer bytes than asked for -
     * means end of file, and is how the engine detects it when the size is
     * not known up front, so a front end must not return a short read for any
     * other reason.
     */
    size_t (*send_read)(void *ctx, ZmodemFile *f, void *data, size_t maxlen);
    void (*send_close)(void *ctx, ZmodemFile *f);

    /*
     * Progress, called as data moves. Cheap enough to call often - but not
     * for every frame, since a progress bar redraw per 1K block would be
     * wasteful.
     */
    void (*progress)(void *ctx, ZmodemDirection dir, const char *name,
                     uint64_t done, uint64_t total);

    /* Called exactly once, at the end of a transfer. */
    void (*finished)(void *ctx, ZmodemOutcome outcome, const char *message);

    /*
     * Debug trace, optional. The front end passes NULL unless PUTTY_DIAG is
     * set, matching the convention used elsewhere in this fork. `msg' is
     * already formatted.
     */
    void (*trace)(void *ctx, const char *msg);
} ZmodemCallbacks;

Zmodem *zmodem_new(const ZmodemCallbacks *cbs);
void zmodem_free(Zmodem *zm);

/*
 * Feed bytes that arrived from the far end.
 *
 * The engine decides what to do with them: protocol bytes are consumed, and
 * everything else is handed straight back through the terminal_output
 * callback. The caller therefore does not have to know whether a transfer is
 * running, and must not try to work it out itself.
 *
 * A prologue can straddle a read boundary, so the engine holds a partial
 * match internally and the caller must keep feeding it every buffer. Bytes it
 * is holding are neither passed to the terminal nor lost: they come out
 * through terminal_output as soon as the engine knows they are not protocol.
 */
void zmodem_feed_in(Zmodem *zm, const void *data, size_t len);

/* Is a transfer running (or starting)? */
bool zmodem_active(Zmodem *zm);

/*
 * A key the user typed while a transfer was running. Almost everything is
 * discarded; the cancel key is what matters, because the terminal emulator is
 * not seeing this input and so cannot interpret it itself.
 */
void zmodem_key(Zmodem *zm, const void *data, size_t len);

/* Stop the transfer now, telling the far end. */
void zmodem_cancel(Zmodem *zm);

/*
 * The file name in a prologue or ZFILE header, for a caller that wants to say
 * what is going on. Valid only while a transfer is active. Never NULL.
 */
const char *zmodem_current_name(Zmodem *zm);

#endif /* PUTTY_ZMODEM_H */
