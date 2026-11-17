/*
 * Tests for the ZMODEM engine.
 *
 * Two kinds of check, deliberately:
 *
 *  - Known-good byte strings from docs/zmodem-protocol-notes.md, which were
 *    computed independently of this code. The ZRINIT header and the ZFILE
 *    subpacket are the important ones: they pin down the CRC-16 and the hex
 *    header layout, which is where the reference document's own worked
 *    example is misleading. The CRC used to build test data here is a
 *    bitwise implementation, not the engine's table-driven one, so a mistake
 *    in either form shows up.
 *  - A full round trip, with one engine sending and another receiving, wired
 *    together in memory. That covers both state machines and, importantly,
 *    the multi-frame and windowing paths that no single vector reaches.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "zmodem.h"

/*
 * The one platform hook this program needs. Linking utils pulls in
 * out_of_memory.c, which calls this when an allocation fails.
 */
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

static int passes, failures;

#define CHECK(cond, ...) do {                                   \
        if (cond) {                                             \
            passes++;                                           \
        } else {                                                \
            failures++;                                         \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);          \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
        }                                                       \
    } while (0)

static void dump(const char *what, const unsigned char *p, size_t n)
{
    size_t i;
    printf("  %s (%lu bytes):", what, (unsigned long)n);
    for (i = 0; i < n && i < 64; i++)
        printf(" %02x", p[i]);
    if (n > 64)
        printf(" ...");
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* An independent, bitwise CRC-16, used only to build test data        */
/* ------------------------------------------------------------------ */

static unsigned short t_crc16(const void *p, size_t n)
{
    const unsigned char *q = (const unsigned char *)p;
    unsigned short crc = 0;
    size_t i;
    int k;

    for (i = 0; i < n; i++) {
        crc ^= (unsigned short)(q[i] << 8);
        for (k = 0; k < 8; k++)
            crc = (crc & 0x8000) ? (unsigned short)((crc << 1) ^ 0x1021)
                                 : (unsigned short)(crc << 1);
    }
    return crc;
}

/* ------------------------------------------------------------------ */
/* Capture                                                             */
/* ------------------------------------------------------------------ */

#define BIG 262144

typedef struct {
    unsigned char term[BIG]; size_t termlen;
    unsigned char sent[BIG]; size_t sentlen;

    /* receiving */
    unsigned char filedata[BIG]; size_t filelen;
    bool file_open, file_closed, file_complete;
    int opens, refuses;
    char opened_name[512];
    uint64_t opened_size;
    unsigned long opened_mtime;
    ZmodemFile *file;

    /* sending */
    unsigned char src[BIG]; size_t srclen, srcpos;
    const char *srcname;
    bool src_used;

    bool finished;
    ZmodemOutcome outcome;
    char message[256];

    /* to catch re-entrancy and stray callbacks */
    Zmodem *zm;
} Cap;

static void cb_send(void *ctx, const void *data, size_t len)
{
    Cap *c = ctx;
    if (c->sentlen + len > BIG) { printf("sent overflow\n"); exit(1); }
    memcpy(c->sent + c->sentlen, data, len);
    c->sentlen += len;
}

static void cb_terminal(void *ctx, const void *data, size_t len)
{
    Cap *c = ctx;
    if (c->termlen + len > BIG) { printf("term overflow\n"); exit(1); }
    memcpy(c->term + c->termlen, data, len);
    c->termlen += len;
}

static ZmodemFile *cb_receive_open(void *ctx, const char *name, uint64_t size,
                                   unsigned long mtime)
{
    Cap *c = ctx;
    c->opens++;
    snprintf(c->opened_name, sizeof(c->opened_name), "%s", name);
    c->opened_size = size;
    c->opened_mtime = mtime;
    c->file_open = true;
    c->filelen = 0;
    c->file = (ZmodemFile *)c;
    return c->file;
}

static void cb_receive_write(void *ctx, ZmodemFile *f, const void *data,
                             size_t len)
{
    Cap *c = ctx;
    (void)f;
    if (c->filelen + len > BIG) { printf("file overflow\n"); exit(1); }
    memcpy(c->filedata + c->filelen, data, len);
    c->filelen += len;
}

static void cb_receive_close(void *ctx, ZmodemFile *f, bool complete)
{
    Cap *c = ctx;
    (void)f;
    c->file_open = false;
    c->file_closed = true;
    c->file_complete = complete;
}

static ZmodemFile *cb_send_open(void *ctx, char **name, uint64_t *size,
                                unsigned long *mtime)
{
    Cap *c = ctx;
    /*
     * One file per transfer. The front end says "no more files" by returning
     * NULL, which is what ends the batch and makes the engine send ZFIN.
     */
    if (c->src_used)
        return NULL;
    c->src_used = true;
    *name = dupstr(c->srcname);
    *size = c->srclen;
    *mtime = 1000000000UL;
    c->srcpos = 0;
    c->file = (ZmodemFile *)c;
    return c->file;
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
    (void)ctx; (void)dir; (void)name; (void)done; (void)total;
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
    (void)ctx;
    if (getenv("ZMODEM_TEST_TRACE"))
        printf("    [zm] %s\n", msg);
}

static void cap_init(Cap *c)
{
    memset(c, 0, sizeof(*c));
    c->srcname = "testfile.bin";
}

static ZmodemCallbacks callbacks_for(Cap *c)
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
    return cb;
}

/* ------------------------------------------------------------------ */
/* Test data builders                                                  */
/* ------------------------------------------------------------------ */

/* A hex header, built here rather than by the engine under test. */
static size_t build_hex_hdr(unsigned char *out, unsigned char type,
                            const unsigned char *data)
{
    static const char hexdig[] = "0123456789abcdef";
    unsigned char body[5];
    unsigned short crc;
    size_t n = 0;
    int i;

    body[0] = type;
    memcpy(body + 1, data, 4);
    crc = t_crc16(body, 5);

    out[n++] = 0x2A; out[n++] = 0x2A; out[n++] = 0x18; out[n++] = 0x42;
    for (i = 0; i < 5; i++) {
        out[n++] = (unsigned char)hexdig[body[i] >> 4];
        out[n++] = (unsigned char)hexdig[body[i] & 0xF];
    }
    out[n++] = (unsigned char)hexdig[crc >> 12];
    out[n++] = (unsigned char)hexdig[(crc >> 8) & 0xF];
    out[n++] = (unsigned char)hexdig[(crc >> 4) & 0xF];
    out[n++] = (unsigned char)hexdig[crc & 0xF];
    out[n++] = 0x0D; out[n++] = 0x0A; out[n++] = 0x11;
    return n;
}

/* A 16-bit-FCS data subpacket, built here. */
static size_t build_subpacket(unsigned char *out, const void *data, size_t len,
                              unsigned char frameend)
{
    const unsigned char *p = data;
    unsigned char tmp[BIG];
    size_t n = 0, i;
    unsigned short crc;

    /* Nothing in these test payloads needs escaping; assert that. */
    for (i = 0; i < len; i++)
        if (p[i] == 0x18 || p[i] == 0x11 || p[i] == 0x13 || p[i] == 0x10)
            { printf("test data needs escaping, fix the builder\n"); exit(1); }

    memcpy(out + n, p, len);
    n += len;
    out[n++] = 0x18;
    out[n++] = frameend;

    memcpy(tmp, p, len);
    tmp[len] = frameend;
    crc = t_crc16(tmp, len + 1);
    out[n++] = (unsigned char)(crc >> 8);
    out[n++] = (unsigned char)(crc & 0xFF);
    return n;
}

/* ------------------------------------------------------------------ */
/* 1. The CRC itself, against the published check values               */
/* ------------------------------------------------------------------ */

static void test_crc_vectors(void)
{
    /* CRC-16/XMODEM check value, and the two the reference states. */
    CHECK(t_crc16("123456789", 9) == 0x31C3,
          "crc16(\"123456789\") = %04x, want 31c3",
          t_crc16("123456789", 9));

    {
        unsigned char zeros[5] = { 0, 0, 0, 0, 0 };
        CHECK(t_crc16(zeros, 5) == 0x0000,
              "crc16(five zeros) = %04x, want 0000", t_crc16(zeros, 5));
    }

    /*
     * The reference's ZFILE worked example. Its stated CRC is 0x5209 over the
     * 40 subpacket bytes plus the frameend byte, and it is the strongest
     * single check we have, because the reference computed it independently.
     */
    {
        static const unsigned char sp[] =
            "foo.txt\0" "1234 7346545000 100644 0 1 1234\0";
        unsigned char tmp[64];
        size_t n = sizeof(sp) - 1;   /* drop the C string's own NUL */

        CHECK(n == 40, "ZFILE example is %lu bytes, want 40",
              (unsigned long)n);
        memcpy(tmp, sp, n);
        tmp[n] = 0x6B;
        CHECK(t_crc16(tmp, n + 1) == 0x5209,
              "ZFILE example CRC = %04x, want 5209", t_crc16(tmp, n + 1));
    }
}

/* ------------------------------------------------------------------ */
/* 2. Prologue detection and pass-through                              */
/* ------------------------------------------------------------------ */

static void test_passthrough(void)
{
    Cap c; ZmodemCallbacks cb; Zmodem *zm;

    cap_init(&c);
    cb = callbacks_for(&c);
    zm = zmodem_new(&cb);
    c.zm = zm;

    zmodem_feed_in(zm, "hello world", 11);
    CHECK(c.termlen == 11 && memcmp(c.term, "hello world", 11) == 0,
          "plain output was not passed through unchanged");
    CHECK(c.sentlen == 0, "plain output provoked %lu bytes of output",
          (unsigned long)c.sentlen);
    CHECK(!zmodem_active(zm), "engine thinks a transfer started");

    /*
     * A lone '*' has to be held: it could be the start of a prologue. It must
     * come out, in the right place, as soon as the next byte rules that out.
     */
    zmodem_feed_in(zm, "*", 1);
    CHECK(c.termlen == 11, "a lone '*' was emitted instead of held");
    zmodem_feed_in(zm, "x", 1);
    CHECK(c.termlen == 13 && memcmp(c.term, "hello world*x", 13) == 0,
          "the held '*' was not released in the right place");
    CHECK(!zmodem_active(zm), "a false start began a transfer");

    /* The same, for a longer prefix of the prologue. */
    zmodem_feed_in(zm, "**\x18\x42", 4);
    CHECK(c.termlen == 13, "a prologue prefix was emitted instead of held");
    zmodem_feed_in(zm, "z", 1);
    CHECK(c.termlen == 18 &&
          memcmp(c.term + 13, "**\x18\x42z", 5) == 0,
          "a released prologue prefix came out wrong");
    CHECK(!zmodem_active(zm), "a longer false start began a transfer");
    CHECK(c.sentlen == 0, "a false start provoked output");

    zmodem_free(zm);
}

static void test_prologue_split(void)
{
    /*
     * The prologue must be found even when it straddles read boundaries, in
     * every possible split.
     */
    static const unsigned char full[] = {
        0x2A, 0x2A, 0x18, 0x42, 0x30, 0x30,     /* ** ZDLE B 0 0 */
        0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,   /* data */
        0x30, 0x30, 0x30, 0x30,                 /* CRC 0000 */
        0x0D, 0x0A, 0x11
    };
    int split;

    for (split = 1; split < (int)sizeof(full); split++) {
        Cap c; ZmodemCallbacks cb; Zmodem *zm;

        cap_init(&c);
        cb = callbacks_for(&c);
        zm = zmodem_new(&cb);
        c.zm = zm;

        zmodem_feed_in(zm, full, split);
        zmodem_feed_in(zm, full + split, sizeof(full) - split);

        CHECK(zmodem_active(zm), "split %d: prologue not detected", split);
        CHECK(c.termlen == 0, "split %d: %lu bytes leaked to the terminal",
              split, (unsigned long)c.termlen);
        /* A receiver answers ZRQINIT with a ZRINIT. */
        CHECK(c.sentlen > 0, "split %d: no ZRINIT sent", split);

        zmodem_free(zm);
    }

    /* And with text before it, which must reach the terminal. */
    {
        Cap c; ZmodemCallbacks cb; Zmodem *zm;
        unsigned char buf[64];
        size_t n = 0;

        cap_init(&c);
        cb = callbacks_for(&c);
        zm = zmodem_new(&cb);
        c.zm = zm;

        memcpy(buf, "rz\r", 3); n = 3;
        memcpy(buf + n, full, sizeof(full)); n += sizeof(full);
        zmodem_feed_in(zm, buf, n);

        CHECK(c.termlen == 3 && memcmp(c.term, "rz\r", 3) == 0,
              "the \"rz\\r\" before the prologue did not reach the terminal");
        CHECK(zmodem_active(zm), "prologue after text not detected");

        zmodem_free(zm);
    }
}

/* ------------------------------------------------------------------ */
/* 3. The ZRINIT we send, against the reference's byte string          */
/* ------------------------------------------------------------------ */

static void test_zrinit_bytes(void)
{
    static const unsigned char expect[] = {
        0x2A, 0x2A, 0x18, 0x42,
        '0', '1',                       /* type 1 = ZRINIT */
        '0', '0', '0', '0', '0', '0',   /* ZF3 ZF2 ZF1 */
        '2', '3',                       /* ZF0 = CANFC32|CANFDX|CANOVIO */
        'b', 'e', '5', '0',             /* CRC-16 */
        0x0D, 0x0A, 0x11
    };
    Cap c; ZmodemCallbacks cb; Zmodem *zm;
    unsigned char pro[64];   /* prologue + hex body + CR LF XON */
    size_t n;

    cap_init(&c);
    cb = callbacks_for(&c);
    zm = zmodem_new(&cb);
    c.zm = zm;

    /* The reference's own ZRQINIT prologue. */
    memcpy(pro, "**\x18\x42", 4);
    pro[4] = '0'; pro[5] = '0';
    n = 6;
    memset(pro + n, '0', 12); n += 12;
    pro[n++] = 0x0D; pro[n++] = 0x0A; pro[n++] = 0x11;

    zmodem_feed_in(zm, pro, n);

    if (c.sentlen != sizeof(expect) ||
        memcmp(c.sent, expect, sizeof(expect)) != 0) {
        dump("got", c.sent, c.sentlen);
        dump("want", expect, sizeof(expect));
    }
    CHECK(c.sentlen == sizeof(expect) && memcmp(c.sent, expect,
                                                sizeof(expect)) == 0,
          "ZRINIT does not match the reference byte string");

    zmodem_free(zm);
}

/* ------------------------------------------------------------------ */
/* 4. Receiving, using the reference's ZFILE example                   */
/* ------------------------------------------------------------------ */

static void test_receive(void)
{
    Cap c; ZmodemCallbacks cb; Zmodem *zm;
    unsigned char buf[BIG];
    size_t n;
    static const unsigned char zfile_sp[] =
        "foo.txt\0" "1234 7346545000 100644 0 1 1234\0";
    static const unsigned char payload[] = "hello, this is the file body";
    unsigned char d[4];

    cap_init(&c);
    cb = callbacks_for(&c);
    zm = zmodem_new(&cb);
    c.zm = zm;

    /* Prologue. */
    memcpy(buf, "**\x18\x42", 4);
    buf[4] = '0'; buf[5] = '0';
    n = 6;
    memset(buf + n, '0', 12); n += 12;
    buf[n++] = 0x0D; buf[n++] = 0x0A; buf[n++] = 0x11;
    zmodem_feed_in(zm, buf, n);
    CHECK(c.sentlen > 0, "no ZRINIT");
    c.sentlen = 0;

    /* ZFILE, followed by the reference's file information subpacket. */
    d[0] = d[1] = d[2] = d[3] = 0;
    n = build_hex_hdr(buf, 4 /* ZFILE */, d);
    memcpy(buf + n, zfile_sp, sizeof(zfile_sp) - 1); n += sizeof(zfile_sp) - 1;
    buf[n++] = 0x18; buf[n++] = 0x6B;       /* ZDLE ZCRCW */
    buf[n++] = 0x52; buf[n++] = 0x09;       /* the reference's CRC */
    zmodem_feed_in(zm, buf, n);

    CHECK(c.opens == 1, "receive_open called %d times, want 1", c.opens);
    CHECK(strcmp(c.opened_name, "foo.txt") == 0,
          "opened \"%s\", want \"foo.txt\"", c.opened_name);
    CHECK(c.opened_size == 1234, "size %llu, want 1234",
          (unsigned long long)c.opened_size);
    CHECK(c.opened_mtime == 1000000000UL, "mtime %lu, want 1000000000",
          c.opened_mtime);
    CHECK(c.sentlen > 0, "no ZRPOS sent after ZFILE");
    c.sentlen = 0;

    /* ZDATA at offset 0, then one ZCRCW subpacket. */
    d[0] = d[1] = d[2] = d[3] = 0;
    n = build_hex_hdr(buf, 10 /* ZDATA */, d);
    n += build_subpacket(buf + n, payload, sizeof(payload) - 1, 0x6B);
    zmodem_feed_in(zm, buf, n);

    CHECK(c.filelen == sizeof(payload) - 1 &&
          memcmp(c.filedata, payload, sizeof(payload) - 1) == 0,
          "received %lu bytes, want %lu", (unsigned long)c.filelen,
          (unsigned long)(sizeof(payload) - 1));
    CHECK(c.sentlen > 0, "no ZACK sent after a ZCRCW subpacket");
    c.sentlen = 0;

    /* ZEOF at the right offset closes the file. */
    {
        uint64_t pos = sizeof(payload) - 1;
        d[0] = (unsigned char)pos;
        d[1] = (unsigned char)(pos >> 8);
        d[2] = (unsigned char)(pos >> 16);
        d[3] = (unsigned char)(pos >> 24);
    }
    n = build_hex_hdr(buf, 11 /* ZEOF */, d);
    zmodem_feed_in(zm, buf, n);

    CHECK(c.file_closed && c.file_complete,
          "file not closed as complete at ZEOF");
    CHECK(!c.finished, "finished fired at ZEOF, should wait for ZFIN");

    /* ZFIN ends the session. */
    d[0] = d[1] = d[2] = d[3] = 0;
    n = build_hex_hdr(buf, 8 /* ZFIN */, d);
    zmodem_feed_in(zm, buf, n);

    CHECK(c.finished && c.outcome == ZMOUT_DONE,
          "not finished cleanly: finished=%d outcome=%d",
          (int)c.finished, (int)c.outcome);

    zmodem_free(zm);
}

/* ------------------------------------------------------------------ */
/* 5. A full round trip, sender to receiver                            */
/* ------------------------------------------------------------------ */

static void pump(Cap *from, Zmodem *to, int *steps)
{
    while (from->sentlen && (*steps)++ < 200) {
        unsigned char tmp[BIG];
        size_t n = from->sentlen;
        memcpy(tmp, from->sent, n);
        from->sentlen = 0;
        zmodem_feed_in(to, tmp, n);
    }
}

static void round_trip(size_t payload_len)
{
    Cap tx, rx;
    ZmodemCallbacks cbt, cbr;
    Zmodem *ztx, *zrx;
    unsigned char zrinit[BIG];
    unsigned char d[4];
    size_t n, i;
    int steps = 0;

    cap_init(&tx);
    cap_init(&rx);
    tx.srcname = "payload.bin";
    for (i = 0; i < payload_len; i++)
        tx.src[i] = (unsigned char)((i * 7 + 3) & 0xFF);
    tx.srclen = payload_len;

    cbt = callbacks_for(&tx);
    cbr = callbacks_for(&rx);
    ztx = zmodem_new(&cbt);
    zrx = zmodem_new(&cbr);
    tx.zm = ztx;
    rx.zm = zrx;

    /*
     * Prime the receiver the way a real sz does: the far end announces itself
     * with a ZRQINIT, which is also what starts our engine, since it is the
     * only thing that comes before the binary headers. The receiver answers
     * with its ZRINIT, and that is what starts our sender.
     */
    d[0] = d[1] = d[2] = d[3] = 0;
    n = build_hex_hdr(zrinit, 0 /* ZRQINIT */, d);
    zmodem_feed_in(zrx, zrinit, n);
    pump(&rx, ztx, &steps);

    CHECK(tx.sentlen > 0 || steps > 0, "%lu bytes: sender never started",
          (unsigned long)payload_len);

    /* Now let the two talk. */
    for (i = 0; i < 400; i++) {
        pump(&tx, zrx, &steps);
        pump(&rx, ztx, &steps);
        if (tx.finished && rx.finished)
            break;
    }

    CHECK(rx.opens == 1, "%lu bytes: receive_open called %d times",
          (unsigned long)payload_len, rx.opens);
    CHECK(rx.filelen == payload_len,
          "%lu bytes: received %lu", (unsigned long)payload_len,
          (unsigned long)rx.filelen);
    CHECK(rx.filelen == payload_len &&
          memcmp(rx.filedata, tx.src, payload_len) == 0,
          "%lu bytes: contents differ", (unsigned long)payload_len);
    CHECK(rx.file_closed && rx.file_complete,
          "%lu bytes: file not closed complete",
          (unsigned long)payload_len);
    CHECK(rx.finished && rx.outcome == ZMOUT_DONE,
          "%lu bytes: receiver did not finish cleanly (%d, %s)",
          (unsigned long)payload_len, (int)rx.outcome, rx.message);
    CHECK(tx.finished && tx.outcome == ZMOUT_DONE,
          "%lu bytes: sender did not finish cleanly (%d, %s)",
          (unsigned long)payload_len, (int)tx.outcome, tx.message);
    CHECK(strcmp(rx.opened_name, "payload.bin") == 0,
          "%lu bytes: name \"%s\"", (unsigned long)payload_len,
          rx.opened_name);

    zmodem_free(ztx);
    zmodem_free(zrx);
}

/* ------------------------------------------------------------------ */
/* 6. Cancelling, and a refused file                                   */
/* ------------------------------------------------------------------ */

static void test_cancel(void)
{
    Cap c; ZmodemCallbacks cb; Zmodem *zm;
    unsigned char pro[64];   /* prologue + hex body + CR LF XON */
    size_t n;

    cap_init(&c);
    cb = callbacks_for(&c);
    zm = zmodem_new(&cb);
    c.zm = zm;

    memcpy(pro, "**\x18\x42", 4);
    pro[4] = '0'; pro[5] = '0';
    n = 6;
    memset(pro + n, '0', 12); n += 12;
    pro[n++] = 0x0D; pro[n++] = 0x0A; pro[n++] = 0x11;
    zmodem_feed_in(zm, pro, n);

    c.sentlen = 0;
    zmodem_cancel(zm);

    CHECK(c.finished && c.outcome == ZMOUT_CANCELLED,
          "cancel did not report ZMOUT_CANCELLED");
    /* The abort sequence is ten CANs followed by ten backspaces. */
    CHECK(c.sentlen == 20, "abort sequence is %lu bytes, want 20",
          (unsigned long)c.sentlen);
    {
        int i, ok = 1;
        for (i = 0; i < 10; i++)
            if (c.sent[i] != 0x18) ok = 0;
        for (i = 10; i < 20; i++)
            if (c.sent[i] != 0x08) ok = 0;
        CHECK(ok, "abort sequence is not ten CANs then ten backspaces");
    }
    CHECK(!zmodem_active(zm), "engine still active after cancel");

    zmodem_free(zm);
}

/* ------------------------------------------------------------------ */
/* 7. A refused file                                                   */
/* ------------------------------------------------------------------ */

/*
 * A refused file must not be reported as a completed transfer.
 *
 * The front end answers "no more files" by returning NULL from send_open -
 * and that is the same answer it gives when the user cancels the file chooser.
 * If nothing has been sent yet, the transfer has to be reported as cancelled
 * and the session aborted. Ending it with ZFIN instead, which is what this
 * used to do, makes the peer answer ZFIN, and the user is then told that a
 * transfer which never happened "completed".
 */
static void test_refuse_file(void)
{
    Cap tx, rx;
    ZmodemCallbacks cbt, cbr;
    Zmodem *ztx, *zrx;
    unsigned char zrinit[BIG];
    unsigned char d[4];
    size_t n, i;
    int steps = 0, run = 0, found = 0;

    cap_init(&tx);
    cap_init(&rx);
    tx.srcname = "refused.bin";
    tx.src[0] = 'x';
    tx.srclen = 1;
    tx.src_used = true;         /* the front end refuses the file outright */

    cbt = callbacks_for(&tx);
    cbr = callbacks_for(&rx);
    ztx = zmodem_new(&cbt);
    zrx = zmodem_new(&cbr);
    tx.zm = ztx;
    rx.zm = zrx;

    /*
     * Prime a receiver so that it emits the ZRINIT our sender waits for, and
     * pass that straight to the sender, which is then asked for a file it
     * will not provide.
     */
    d[0] = d[1] = d[2] = d[3] = 0;
    n = build_hex_hdr(zrinit, 0 /* ZRQINIT */, d);
    zmodem_feed_in(zrx, zrinit, n);
    pump(&rx, ztx, &steps);

    CHECK(tx.finished && tx.outcome == ZMOUT_CANCELLED,
          "refused file reported (%d, %s), want cancelled",
          (int)tx.outcome, tx.message);
    CHECK(!zmodem_active(ztx), "engine still active after a refused file");

    /* The abort sequence - ten CANs - rather than a ZFIN. */
    for (i = 0; i < tx.sentlen; i++) {
        if (tx.sent[i] == 0x18) {
            if (++run == 10) { found = 1; break; }
        } else {
            run = 0;
        }
    }
    CHECK(found, "no ten-CAN abort sequence in %lu bytes of output",
          (unsigned long)tx.sentlen);

    zmodem_free(ztx);
    zmodem_free(zrx);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    /* Unbuffered, so a crash still shows how far we got. */
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("ZMODEM engine tests\n");
    printf("  (set ZMODEM_TEST_TRACE=1 for the engine's own trace)\n\n");

    printf("  [test] crc_vectors\n");   test_crc_vectors();
    printf("  [test] passthrough\n");     test_passthrough();
    printf("  [test] prologue_split\n");  test_prologue_split();
    printf("  [test] zrinit_bytes\n");    test_zrinit_bytes();
    printf("  [test] receive\n");         test_receive();
    printf("  [test] cancel\n");          test_cancel();
    printf("  [test] refuse_file\n");     test_refuse_file();

    printf("  [test] round_trip 1\n");       round_trip(1);
    printf("  [test] round_trip 1024\n");    round_trip(1024);
    printf("  [test] round_trip 4096\n");    round_trip(4096);
    printf("  [test] round_trip 20000\n");   round_trip(20000);
    printf("  [test] round_trip 100000\n");  round_trip(100000);

    printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
