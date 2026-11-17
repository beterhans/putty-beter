/*
 * A real interoperability check for the ZMODEM engine: run the system's own
 * sz (or rz) over a pty, drive it with our engine, and check the file arrives
 * intact.
 *
 * This is the one test that proves interoperability rather than
 * self-consistency. It needs lrzsz installed (Homebrew's "lrzsz"), so it is
 * built but not run by the normal test sweep - run it by hand:
 *
 *     build-arm64/test_zmodem_interop
 *     ZMODEM_INTEROP_TRACE=1 build-arm64/test_zmodem_interop
 *
 * It is macOS-only because it uses forkpty().
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>               /* forkpty */

#include "putty.h"
#include "misc.h"
#include "zmodem.h"

/* The one platform hook linking utils pulls in. */
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

#define FILESIZE 250000
#define BIG (1 << 21)

static int passes, failures;
static FILE *dump_in, *dump_out;   /* ZMODEM_INTEROP_DUMP=1 */

#define CHECK(cond, ...) do {                                   \
        if (cond) { passes++; }                                 \
        else {                                                  \
            failures++;                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);          \
            printf(__VA_ARGS__); printf("\n");                  \
        }                                                       \
    } while (0)

typedef struct {
    int ptyfd;
    Zmodem *zm;

    /* received */
    unsigned char got[BIG];
    size_t gotlen;
    bool opened, closed, complete;
    char name[512];
    uint64_t claimed;

    /* to send */
    unsigned char src[BIG];
    size_t srclen, srcpos;
    const char *srcname;

    bool finished;
    ZmodemOutcome outcome;
    char message[256];
    bool trace;
} Cap;

static void cb_send(void *ctx, const void *data, size_t len)
{
    Cap *c = ctx;
    const unsigned char *p = data;
    size_t left = len;

    if (dump_out)
        fwrite(data, 1, len, dump_out);

    /*
     * Write it all. A pty's input buffer is small, so a single non-blocking
     * write can return short and silently truncate a frame - which looks
     * exactly like a peer rejecting perfectly good data.
     */
    while (left) {
        ssize_t n = write(c->ptyfd, p, left);
        if (n > 0) {
            p += n;
            left -= (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            fd_set wf;
            struct timeval tv;
            FD_ZERO(&wf);
            FD_SET(c->ptyfd, &wf);
            tv.tv_sec = 1;
            tv.tv_usec = 0;
            if (select(c->ptyfd + 1, NULL, &wf, NULL, &tv) <= 0)
                break;
            continue;
        }
        break;
    }
}

static void cb_terminal(void *ctx, const void *data, size_t len)
{
    Cap *c = ctx;
    if (c->trace)
        fprintf(stderr, "    [term] %.*s\n", (int)len, (const char *)data);
}

static ZmodemFile *cb_receive_open(void *ctx, const char *name, uint64_t size,
                                   unsigned long mtime)
{
    Cap *c = ctx;
    (void)mtime;
    c->opened = true;
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->claimed = size;
    c->gotlen = 0;
    return (ZmodemFile *)c;
}

static void cb_receive_write(void *ctx, ZmodemFile *f, const void *data,
                             size_t len)
{
    Cap *c = ctx;
    (void)f;
    if (c->gotlen + len <= BIG) {
        memcpy(c->got + c->gotlen, data, len);
        c->gotlen += len;
    }
}

static void cb_receive_close(void *ctx, ZmodemFile *f, bool complete)
{
    Cap *c = ctx;
    (void)f;
    c->closed = true;
    c->complete = complete;
}

static ZmodemFile *cb_send_open(void *ctx, char **name, uint64_t *size,
                                unsigned long *mtime)
{
    Cap *c = ctx;
    if (c->srcpos || !c->srclen)
        return NULL;
    *name = dupstr(c->srcname);
    *size = c->srclen;
    *mtime = 1700000000UL;
    return (ZmodemFile *)c;
}

static size_t cb_send_read(void *ctx, ZmodemFile *f, void *data, size_t maxlen)
{
    Cap *c = ctx;
    size_t left = c->srclen - c->srcpos;
    size_t n = left < maxlen ? left : maxlen;
    (void)f;
    memcpy(data, c->src + c->srcpos, n);
    c->srcpos += n;
    return n;
}

static void cb_send_close(void *ctx, ZmodemFile *f)
{
    (void)ctx; (void)f;
}

static void cb_progress(void *ctx, ZmodemDirection dir, const char *name,
                        uint64_t done, uint64_t total)
{
    Cap *c = ctx;
    if (c->trace && total && done % 65536 < 1024)
        fprintf(stderr, "    [progress] %s %llu/%llu\n",
                dir == ZMDIR_RECEIVE ? "recv" : "send",
                (unsigned long long)done, (unsigned long long)total);
    (void)name;
}

static void cb_finished(void *ctx, ZmodemOutcome outcome, const char *message)
{
    Cap *c = ctx;
    c->finished = true;
    c->outcome = outcome;
    snprintf(c->message, sizeof(c->message), "%s", message ? message : "");
}

static void cb_trace(void *ctx, const char *msg)
{
    Cap *c = ctx;
    if (c->trace)
        fprintf(stderr, "    [zm] %s\n", msg);
}

static Zmodem *make_engine(Cap *c)
{
    ZmodemCallbacks cb;
    memset(&cb, 0, sizeof(cb));
    cb.ctx = c;
    cb.send = cb_send;
    cb.terminal_output = cb_terminal;
    cb.receive_open = cb_receive_open;
    cb.receive_write = cb_receive_write;
    cb.receive_close = cb_receive_close;
    cb.send_open = cb_send_open;
    cb.send_read = cb_send_read;
    cb.send_close = cb_send_close;
    cb.progress = cb_progress;
    cb.finished = cb_finished;
    cb.trace = cb_trace;
    return zmodem_new(&cb);
}

/* The pattern we send and expect back. */
static unsigned char pattern(size_t i)
{
    /* ZMODEM_INTEROP_PRINTABLE=1 restricts it to plain ASCII. */
    if (getenv("ZMODEM_INTEROP_PRINTABLE"))
        return (unsigned char)('A' + (i % 26));
    return (unsigned char)((i * 31 + (i >> 8) * 7 + 11) & 0xFF);
}

static const char *lrzsz(const char *tool)
{
    static char path[256];
    const char *dirs[] = { "/opt/homebrew/bin", "/usr/local/bin", NULL };
    int i;
    for (i = 0; dirs[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", dirs[i], tool);
        if (access(path, X_OK) == 0)
            return path;
    }
    return NULL;
}

/*
 * Run one direction. `tool` is "sz" (the far end sends, we receive) or "rz"
 * (the far end receives, we send).
 */
static void run_one(const char *tool, bool we_receive)
{
    Cap c;
    char dir[] = "/tmp/zmxferXXXXXX";
    char path[512], base[64];
    const char *toolpath = lrzsz(tool);
    int master = -1;
    pid_t pid;
    FILE *fp;
    size_t i;
    int elapsed_ms = 0;

    if (!toolpath) {
        printf("  %s: lrzsz not installed, skipping\n", tool);
        return;
    }

    memset(&c, 0, sizeof(c));
    c.trace = getenv("ZMODEM_INTEROP_TRACE") != NULL;
    c.ptyfd = -1;

    /* Fresh dump files for each direction, so they cannot contaminate. */
    if (getenv("ZMODEM_INTEROP_DUMP")) {
        char q[64];
        if (dump_in) fclose(dump_in);
        if (dump_out) fclose(dump_out);
        snprintf(q, sizeof(q), "/tmp/zm-in-%s.bin", tool);
        dump_in = fopen(q, "wb");
        snprintf(q, sizeof(q), "/tmp/zm-out-%s.bin", tool);
        dump_out = fopen(q, "wb");
    }

    if (!mkdtemp(dir)) {
        printf("  %s: mkdtemp failed\n", tool);
        failures++;
        return;
    }

    snprintf(base, sizeof(base), "payload-%s.bin", tool);
    snprintf(path, sizeof(path), "%s/%s", dir, base);

    if (we_receive) {
        /* The far end sends this file to us. */
        fp = fopen(path, "wb");
        if (!fp) { printf("  cannot create %s\n", path); failures++; return; }
        for (i = 0; i < FILESIZE; i++)
            fputc(pattern(i), fp);
        fclose(fp);
    } else {
        /* We send this file to the far end. */
        for (i = 0; i < FILESIZE; i++)
            c.src[i] = pattern(i);
        c.srclen = FILESIZE;
        c.srcname = base;
    }

    pid = forkpty(&master, NULL, NULL, NULL);
    if (pid < 0) {
        printf("  %s: forkpty failed: %s\n", tool, strerror(errno));
        failures++;
        return;
    }
    if (pid == 0) {
        if (chdir(dir) != 0)
            _exit(127);
        /* rz takes no file name: it is told by the protocol. */
        if (we_receive)
            execl(toolpath, tool, base, (char *)NULL);
        else
            execl(toolpath, tool, (char *)NULL);
        _exit(127);
    }

    c.ptyfd = master;
    fcntl(master, F_SETFL, O_NONBLOCK);
    c.zm = make_engine(&c);

    /*
     * Drive it until the engine says it is done, or we give up.
     *
     * The budget is real wall-clock time and the pty is drained completely on
     * each pass. Counting loop iterations instead lets a fast loop burn the
     * whole budget in milliseconds, which looks exactly like a stall in the
     * engine and is not one.
     */
    {
        struct timespec t0, now;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            fd_set rf;
            struct timeval tv;
            int r;

            FD_ZERO(&rf);
            FD_SET(master, &rf);
            tv.tv_sec = 0;
            tv.tv_usec = 20000;
            r = select(master + 1, &rf, NULL, NULL, &tv);

            if (r > 0 && FD_ISSET(master, &rf)) {
                bool eof = false;
                for (;;) {
                    unsigned char buf[65536];
                    ssize_t n = read(master, buf, sizeof(buf));
                    if (n > 0) {
                        if (dump_in)
                            fwrite(buf, 1, (size_t)n, dump_in);
                        zmodem_feed_in(c.zm, buf, (size_t)n);
                    } else {
                        if (n == 0)
                            eof = true;
                        break;
                    }
                }
                if (eof)
                    break;
            }

            if (c.finished)
                break;
            clock_gettime(CLOCK_MONOTONIC, &now);
            elapsed_ms = (int)((now.tv_sec - t0.tv_sec) * 1000 +
                               (now.tv_nsec - t0.tv_nsec) / 1000000);
            if (elapsed_ms >= 30000)
                break;
        }
    }

    if (!c.finished) {
        printf("  %s: timed out after %d ms\n", tool, elapsed_ms);
        failures++;
    } else if (c.outcome != ZMOUT_DONE) {
        printf("  %s: engine reported outcome %d (%s)\n", tool,
               (int)c.outcome, c.message);
        failures++;
    }

    if (we_receive) {
        CHECK(c.opened, "%s: no file was offered", tool);
        CHECK(strcmp(c.name, base) == 0, "%s: name \"%s\" want \"%s\"",
              tool, c.name, base);
        CHECK(c.claimed == FILESIZE, "%s: claimed size %llu want %d",
              tool, (unsigned long long)c.claimed, FILESIZE);
        CHECK(c.gotlen == FILESIZE, "%s: received %lu bytes, want %d",
              tool, (unsigned long)c.gotlen, FILESIZE);
        if (c.gotlen == FILESIZE) {
            size_t bad = 0;
            for (i = 0; i < FILESIZE; i++)
                if (c.got[i] != pattern(i))
                    bad++;
            CHECK(bad == 0, "%s: %lu bytes differ", tool,
                  (unsigned long)bad);
        }
        CHECK(c.closed && c.complete, "%s: file not closed as complete",
              tool);
    } else {
        CHECK(c.srcpos == FILESIZE, "%s: sent %lu of %d bytes", tool,
              (unsigned long)c.srcpos, FILESIZE);
    }

    zmodem_free(c.zm);
    close(master);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    if (!we_receive) {
        /* The far end wrote the file it received; check it. */
        fp = fopen(path, "rb");
        if (!fp) {
            CHECK(false, "%s: the far end wrote no file at %s", tool, path);
        } else {
            unsigned char *back = snewn(FILESIZE, unsigned char);
            size_t n = fread(back, 1, FILESIZE, fp);
            size_t bad = 0;
            fclose(fp);
            CHECK(n == FILESIZE, "%s: far end file is %lu bytes, want %d",
                  tool, (unsigned long)n, FILESIZE);
            for (i = 0; i < n && i < FILESIZE; i++)
                if (back[i] != pattern(i))
                    bad++;
            CHECK(bad == 0, "%s: %lu bytes differ at the far end", tool,
                  (unsigned long)bad);
            sfree(back);
        }
    }

    /* Clean up. */
    {
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        if (system(cmd) != 0)
            fprintf(stderr, "cleanup of %s failed\n", dir);
    }
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("ZMODEM interoperability against the system's lrzsz\n");

    if (getenv("ZMODEM_INTEROP_DUMP")) {
        dump_in = fopen("/tmp/zm-in.bin", "wb");
        dump_out = fopen("/tmp/zm-out.bin", "wb");
    }

    printf("  [1] sz: the far end sends, we receive\n");
    run_one("sz", true);

    printf("  [2] rz: the far end receives, we send\n");
    run_one("rz", false);

    printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
