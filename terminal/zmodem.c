/*
 * ZMODEM (sz/rz) file transfer.
 *
 * See zmodem.h for the interface and docs/zmodem-protocol-notes.md for the
 * byte-level protocol reference. This file has no front end, display or
 * filesystem dependency: everything outside goes through ZmodemCallbacks.
 *
 * One correction to the reference document is applied here, and it is
 * important enough to state plainly, because implementing the reference
 * literally produces a protocol that does not interoperate:
 *
 *   The CRC-16 is plain CRC-16/XMODEM - poly 0x1021, init 0, no reflection,
 *   no final XOR, and NO trailing zero bytes. Reference section 3.1 gives an
 *   `updcrc` snippet implying two trailing zero bytes, but that is an artefact
 *   of how rzsz's macro folds the pad into the byte-wise XOR. The standard
 *   form reproduces every published check value - "123456789" -> 0x31C3, five
 *   zero bytes -> 0x0000, and the 0x5209 of the reference's own ZFILE worked
 *   example - while the snippet's form reproduces none of them. The reference
 *   also describes that example as 40 bytes; it is 40 subpacket bytes plus the
 *   frameend byte, and the CRC covers all 41. test/test_zmodem.c pins all of
 *   this down so it cannot regress.
 *
 * Scope of this first version, and what is deliberately left out:
 *
 *  - 16-bit and 32-bit frame check sequences are both supported, so we can
 *    advertise CANFC32 and interoperate with a peer that uses it.
 *  - The sender paces itself with a byte window: it streams with ZCRCG and
 *    asks for an acknowledgement with ZCRCQ at the end of each window. That
 *    avoids a round trip per 1K block, without needing the reverse-channel
 *    sampling that fully non-stop streaming would require.
 *  - Resume (ZCRESUM), text conversion (ZCNL), compression, encryption,
 *    ZCOMMAND and the Attn string are not implemented. ZSINIT is accepted and
 *    acknowledged, but its Attn string is ignored.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "misc.h"
#include "zmodem.h"

/* ------------------------------------------------------------------ */
/* Control bytes and frame types (reference section 1)                 */
/* ------------------------------------------------------------------ */

#define ZPAD    0x2A   /* '*' */
#define ZDLE    0x18   /* CAN / ^X */
#define ZBIN    0x41   /* 'A' */
#define ZHEX    0x42   /* 'B' */
#define ZBIN32  0x43   /* 'C' */
#define ZCRCE   0x68   /* 'h' */
#define ZCRCG   0x69   /* 'i' */
#define ZCRCQ   0x6A   /* 'j' */
#define ZCRCW   0x6B   /* 'k' */
#define ZRUB0   0x6C   /* 'l' -> 0x7F */
#define ZRUB1   0x6D   /* 'm' -> 0xFF */

#define XON     0x11
#define XOFF    0x13

#define ZRQINIT     0
#define ZRINIT      1
#define ZSINIT      2
#define ZACK        3
#define ZFILE       4
#define ZSKIP       5
#define ZNAK        6
#define ZABORT      7
#define ZFIN        8
#define ZRPOS       9
#define ZDATA       10
#define ZEOF        11
#define ZFERR       12
#define ZCRC        13
#define ZCHALLENGE  14
#define ZCOMPL      15
#define ZCAN        16

/* Capability flags advertised in ZRINIT (reference section 1). */
#define CANFDX      0x01
#define CANOVIO     0x02
#define CANFC32     0x20
#define ESCCTL      0x40

/* What we advertise. Matches rzsz, so a peer may use 32-bit FCS. */
#define ZM_RX_FLAGS (CANFC32 | CANFDX | CANOVIO)

/* Limits from the reference's robustness section. */
#define ZM_MAX_SUBPACKET   1024
#define ZM_MAX_ERRORS      20
#define ZM_MAX_CANS        5
#define ZM_DEFAULT_WINDOW  8192
#define ZM_MAX_NAME        256

/* ------------------------------------------------------------------ */
/* CRC                                                                 */
/* ------------------------------------------------------------------ */

static uint16_t crc16_tab[256];
static uint32_t crc32_tab[256];
static bool crc_tabs_ready;

static void crc_tabs_init(void)
{
    unsigned i;
    int k;

    if (crc_tabs_ready)
        return;

    for (i = 0; i < 256; i++) {
        uint16_t c = (uint16_t)(i << 8);
        for (k = 0; k < 8; k++)
            c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021)
                             : (uint16_t)(c << 1);
        crc16_tab[i] = c;
    }

    for (i = 0; i < 256; i++) {
        uint32_t c = i;
        for (k = 0; k < 8; k++)
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320 : c >> 1;
        crc32_tab[i] = c;
    }

    crc_tabs_ready = true;
}

static uint16_t crc16_update(uint16_t crc, unsigned char b)
{
    return (uint16_t)(((crc << 8) & 0xFFFF) ^
                      crc16_tab[((crc >> 8) ^ b) & 0xFF]);
}

static uint16_t crc16(const void *p, size_t n)
{
    const unsigned char *q = (const unsigned char *)p;
    uint16_t crc = 0;

    while (n--)
        crc = crc16_update(crc, *q++);
    return crc;
}

static uint32_t crc32_update(uint32_t crc, unsigned char b)
{
    return crc32_tab[(crc ^ b) & 0xFF] ^ (crc >> 8);
}

/* This is zlib's crc32: init 0xFFFFFFFF, final complement. */
static uint32_t crc32_z(const void *p, size_t n)
{
    const unsigned char *q = (const unsigned char *)p;
    uint32_t crc = 0xFFFFFFFF;

    while (n--)
        crc = crc32_update(crc, *q++);
    return ~crc;
}

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    ZMST_IDLE,          /* no transfer: scanning for a prologue */
    ZMST_RX_READY,      /* we are the receiver, ZRINIT sent, want a file */
    ZMST_RX_DATA,       /* inside a ZDATA burst */
    ZMST_TX_READY,      /* we are the sender, want the peer's ZRINIT */
    ZMST_TX_FILE,       /* ZFILE sent, waiting for ZRPOS or ZSKIP */
    ZMST_TX_DATA,       /* sending data */
    ZMST_TX_ACK,        /* a frame needing an acknowledgement is outstanding */
    ZMST_TX_EOF,        /* ZEOF sent, waiting for ZRINIT or ZRPOS */
    ZMST_TX_FIN,        /* ZFIN sent, waiting for the peer's ZFIN */
    ZMST_DONE,
} ZmState;

typedef struct {
    unsigned char type;
    unsigned char data[4];
} ZmHeader;

typedef struct {
    const unsigned char *data;   /* points into zm->sub */
    size_t len;
    unsigned char frameend;
} ZmSubpacket;

struct Zmodem {
    ZmodemCallbacks cbs;

    ZmState state;
    ZmodemDirection dir;
    bool finished_called;

    /* Prologue detection: always a proper prefix of the pattern. */
    unsigned char pro[8];
    size_t prolen;

    /* Unparsed input. */
    unsigned char *in;
    size_t inlen, inalloc;

    /* Scratch for the subpacket being assembled. */
    unsigned char *sub;
    size_t sublen, suballoc;

    /* Outgoing bytes, batched into one cbs->send call. */
    unsigned char *out;
    size_t outlen, outalloc;
    unsigned char lastsent;

    /* Current file. */
    char *name;
    ZmodemFile *file;
    uint64_t rx_offset;     /* bytes stored for the current file */
    uint64_t rx_claimed;    /* what the peer says the size is */
    uint64_t tx_offset;     /* bytes sent for the current file */
    uint64_t tx_size;
    uint64_t tx_window;     /* bytes to send before asking for an ack */
    uint64_t tx_in_window;
    bool tx_hdr_sent;       /* a ZDATA header is open for the current burst */
    bool tx_first_frame;    /* the next frame is the first of a burst */
    bool tx_sent_any;       /* a file has been sent in this transfer */

    /*
     * Frame check sequence widths. These are two different questions and
     * must not share a field: `fcs32' is what we put on our own output, which
     * as a sender we take from the peer's ZRINIT flags; `rx_fcs32' is what
     * the peer puts on the data subpackets we are receiving, which we learn
     * from the encoding of the incoming headers (reference section 8.5).
     */
    bool fcs32;             /* 32-bit FCS on what we send */
    bool rx_fcs32;          /* the peer is using 32-bit FCS on its data */
    unsigned char peer_flags;
    unsigned long peer_buflen;

    /* Error recovery. */
    int errors;
    int cans;
};

static void zmtrace(Zmodem *zm, const char *fmt, ...)
{
    va_list ap;
    char *msg;

    if (!zm->cbs.trace)
        return;
    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);
    zm->cbs.trace(zm->cbs.ctx, msg);
    sfree(msg);
}

/* ------------------------------------------------------------------ */
/* Growable buffers                                                    */
/* ------------------------------------------------------------------ */

static void buf_append(unsigned char **buf, size_t *len, size_t *alloc,
                       const void *data, size_t n)
{
    if (*len + n > *alloc) {
        size_t want = *alloc ? *alloc : 256;
        while (want < *len + n)
            want *= 2;
        *buf = sresize(*buf, want, unsigned char);
        *alloc = want;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
}

static void buf_consume(unsigned char *buf, size_t *len, size_t n)
{
    assert(n <= *len);
    memmove(buf, buf + n, *len - n);
    *len -= n;
}

/* ------------------------------------------------------------------ */
/* Outgoing bytes                                                      */
/* ------------------------------------------------------------------ */

static void zm_out_raw(Zmodem *zm, const void *data, size_t n)
{
    buf_append(&zm->out, &zm->outlen, &zm->outalloc, data, n);
    if (n)
        zm->lastsent = ((const unsigned char *)data)[n - 1];
}

static void zm_out_byte(Zmodem *zm, unsigned char c)
{
    zm_out_raw(zm, &c, 1);
}

/*
 * Escape a content byte, per reference section 4. `lastsent' is the byte
 * before escaping, which matters only for CR.
 */
static void zm_out_escaped(Zmodem *zm, unsigned char c)
{
    unsigned char prev = zm->lastsent;

    if (c & 0x60) {
        /* 0x20..0x7F and 0xA0..0xFF go raw. */
        zm_out_byte(zm, c);
    } else if (c == ZDLE) {
        zm_out_byte(zm, ZDLE);
        zm_out_byte(zm, ZDLE ^ 0x40);
    } else if (c == 0x10 || c == 0x11 || c == 0x13 ||
               c == 0x90 || c == 0x91 || c == 0x93) {
        zm_out_byte(zm, ZDLE);
        zm_out_byte(zm, c ^ 0x40);
    } else if (c == 0x0D || c == 0x8D) {
        /* Only escaped straight after '@', to protect Telenet CR-@-CR. */
        if ((prev & 0x7F) == 0x40) {
            zm_out_byte(zm, ZDLE);
            zm_out_byte(zm, c ^ 0x40);
        } else {
            zm_out_byte(zm, c);
        }
    } else {
        zm_out_byte(zm, c);
    }
}

static void zm_out_flush(Zmodem *zm)
{
    if (zm->outlen && zm->cbs.send)
        zm->cbs.send(zm->cbs.ctx, zm->out, zm->outlen);
    zm->outlen = 0;
}

static void zm_send_can(Zmodem *zm)
{
    /*
     * The abort sequence: ten CANs, then ten backspaces to try to erase them
     * if a shell or command interpreter sees them.
     */
    int i;
    for (i = 0; i < 10; i++)
        zm_out_byte(zm, ZDLE);
    for (i = 0; i < 10; i++)
        zm_out_byte(zm, 0x08);
    zm_out_flush(zm);
    zmtrace(zm, "sent CAN abort sequence");
}

/* ------------------------------------------------------------------ */
/* Header construction                                                 */
/* ------------------------------------------------------------------ */

/*
 * Hex header: always CRC-16, always followed by CR LF, and by XON except
 * after ZACK and ZFIN (reference sections 2.3 and 4.2).
 */
static void zm_send_hex_hdr(Zmodem *zm, unsigned char type,
                            const unsigned char *data)
{
    static const char hexdig[] = "0123456789abcdef";
    unsigned char body[5];
    uint16_t crc;
    int i;
    bool want_xon = (type != ZACK && type != ZFIN);

    body[0] = type;
    memcpy(body + 1, data, 4);
    crc = crc16(body, 5);

    zm_out_byte(zm, ZPAD);
    zm_out_byte(zm, ZPAD);
    zm_out_byte(zm, ZDLE);
    zm_out_byte(zm, ZHEX);

    for (i = 0; i < 5; i++) {
        zm_out_byte(zm, (unsigned char)hexdig[body[i] >> 4]);
        zm_out_byte(zm, (unsigned char)hexdig[body[i] & 0xF]);
    }
    zm_out_byte(zm, (unsigned char)hexdig[crc >> 12]);
    zm_out_byte(zm, (unsigned char)hexdig[(crc >> 8) & 0xF]);
    zm_out_byte(zm, (unsigned char)hexdig[(crc >> 4) & 0xF]);
    zm_out_byte(zm, (unsigned char)hexdig[crc & 0xF]);

    zm_out_byte(zm, 0x0D);
    zm_out_byte(zm, 0x0A);
    if (want_xon)
        zm_out_byte(zm, XON);
}

static void zm_pos(unsigned char *data, uint64_t pos)
{
    data[0] = (unsigned char)(pos & 0xFF);
    data[1] = (unsigned char)((pos >> 8) & 0xFF);
    data[2] = (unsigned char)((pos >> 16) & 0xFF);
    data[3] = (unsigned char)((pos >> 24) & 0xFF);
}

static uint64_t zm_get_pos(const unsigned char *data)
{
    return (uint64_t)data[0] | ((uint64_t)data[1] << 8) |
        ((uint64_t)data[2] << 16) | ((uint64_t)data[3] << 24);
}

static void zm_send_hdr_pos(Zmodem *zm, unsigned char type, uint64_t pos)
{
    unsigned char d[4];
    zm_pos(d, pos);
    zm_send_hex_hdr(zm, type, d);
}

static void zm_send_zrinit(Zmodem *zm)
{
    unsigned char d[4];
    d[0] = 0;                        /* ZP0: buffer size low */
    d[1] = 0;                        /* ZP1: buffer size high (0 = no limit) */
    d[2] = 0;                        /* ZF1 */
    d[3] = ZM_RX_FLAGS;              /* ZF0: capabilities */
    zm_send_hex_hdr(zm, ZRINIT, d);
}

static void zm_send_zfin(Zmodem *zm)
{
    unsigned char d[4] = { 0, 0, 0, 0 };
    zm_send_hex_hdr(zm, ZFIN, d);
}

static void zm_send_zack(Zmodem *zm, uint64_t pos)
{
    zm_send_hdr_pos(zm, ZACK, pos);
}

static void zm_send_znak(Zmodem *zm)
{
    unsigned char d[4] = { 0, 0, 0, 0 };
    zm_send_hex_hdr(zm, ZNAK, d);
}

static void zm_send_zskip(Zmodem *zm)
{
    unsigned char d[4] = { 0, 0, 0, 0 };
    zm_send_hex_hdr(zm, ZSKIP, d);
}

/*
 * A binary header, with the content and the frame check sequence escaped.
 * Only used for ZFILE and ZDATA, so that the peer can see which FCS width we
 * are using for the data subpackets that follow (reference section 8.5).
 */
static void zm_send_bin_hdr(Zmodem *zm, unsigned char type,
                            const unsigned char *data)
{
    unsigned char body[5];
    int i;

    body[0] = type;
    memcpy(body + 1, data, 4);

    zm_out_byte(zm, ZPAD);
    zm_out_byte(zm, ZDLE);
    zm_out_byte(zm, zm->fcs32 ? ZBIN32 : ZBIN);

    for (i = 0; i < 5; i++)
        zm_out_escaped(zm, body[i]);

    if (zm->fcs32) {
        uint32_t crc = crc32_z(body, 5);
        for (i = 0; i < 4; i++)
            zm_out_escaped(zm, (unsigned char)(crc >> (8 * i)));
    } else {
        uint16_t crc = crc16(body, 5);
        zm_out_escaped(zm, (unsigned char)(crc >> 8));
        zm_out_escaped(zm, (unsigned char)(crc & 0xFF));
    }
}

/* ------------------------------------------------------------------ */
/* Data subpacket construction                                         */
/* ------------------------------------------------------------------ */

/*
 * Emit one data subpacket: the data, then ZDLE and the terminator, then the
 * escaped frame check sequence, which covers the data and the terminator.
 */
static void zm_send_subpacket(Zmodem *zm, const unsigned char *data, size_t n,
                              unsigned char frameend)
{
    size_t i;

    assert(n <= ZM_MAX_SUBPACKET);

    for (i = 0; i < n; i++)
        zm_out_escaped(zm, data[i]);

    zm_out_byte(zm, ZDLE);
    zm_out_byte(zm, frameend);

    if (zm->fcs32) {
        uint32_t crc = 0xFFFFFFFF;
        for (i = 0; i < n; i++)
            crc = crc32_update(crc, data[i]);
        crc = crc32_update(crc, frameend);
        crc = ~crc;
        for (i = 0; i < 4; i++)
            zm_out_escaped(zm, (unsigned char)(crc >> (8 * i)));
    } else {
        uint16_t crc = 0;
        for (i = 0; i < n; i++)
            crc = crc16_update(crc, data[i]);
        crc = crc16_update(crc, frameend);
        zm_out_escaped(zm, (unsigned char)(crc >> 8));
        zm_out_escaped(zm, (unsigned char)(crc & 0xFF));
    }
}

/* ------------------------------------------------------------------ */
/* Input decoding                                                      */
/* ------------------------------------------------------------------ */

typedef enum {
    ZMGR_BYTE,      /* produced a content byte */
    ZMGR_FRAMEEND,  /* produced a data subpacket terminator */
    ZMGR_MORE,      /* need more input */
    ZMGR_BAD,       /* malformed escape */
} ZmGetResult;

/*
 * Decode one content byte of a binary header or data subpacket, starting at
 * zm->in[*pos]. Reference section 4.1.
 */
static ZmGetResult zm_decode(Zmodem *zm, size_t *pos, unsigned char *out)
{
    unsigned char c, d;

    if (*pos >= zm->inlen)
        return ZMGR_MORE;
    c = zm->in[(*pos)++];

    if (c & 0x60) {
        *out = c;
        return ZMGR_BYTE;
    }

    if (c == ZDLE) {
        if (*pos >= zm->inlen)
            return ZMGR_MORE;
        d = zm->in[(*pos)++];

        if (d == ZCRCE || d == ZCRCG || d == ZCRCQ || d == ZCRCW) {
            *out = d;
            return ZMGR_FRAMEEND;
        }
        if (d == ZRUB0) {
            *out = 0x7F;
            return ZMGR_BYTE;
        }
        if (d == ZRUB1) {
            *out = 0xFF;
            return ZMGR_BYTE;
        }
        if (d == XON || d == 0x91 || d == XOFF || d == 0x93) {
            /* Stray flow control inside an escape: skip and read again. */
            return zm_decode(zm, pos, out);
        }
        if ((d & 0x60) == 0x40) {
            *out = d ^ 0x40;
            return ZMGR_BYTE;
        }
        return ZMGR_BAD;
    }

    if (c == XON || c == 0x91 || c == XOFF || c == 0x93)
        return zm_decode(zm, pos, out);   /* stray flow control */

    *out = c;
    return ZMGR_BYTE;
}

/*
 * Bytes that may legitimately sit between frames, and so must be skipped
 * while looking for the start of a header: padding, the XON that follows a
 * ZCRCW subpacket, stray flow control (including the 8-bit-set forms), and
 * the NULs sz may emit to flush a modem (reference sections 2.3 and 10).
 *
 * Note this is NOT the same set as inside a header, where a stray XON/XOFF
 * is discarded by zm_decode() instead.
 */
static bool zm_ignorable(unsigned char c)
{
    /*
     * Compare with the 8th bit ignored. The reference says the receiver
     * "strips the 8th bit" while hunting for a header, and it matters in
     * practice: lrzsz's sz emits its hex-header trailer as CR, LF|0x80, XON,
     * so the 0x8a has to be recognised as a line feed. Treating it as a stray
     * byte costs an error and, worse, used to provoke a needless resync.
     */
    unsigned char d = c & 0x7F;
    return d == ZPAD || d == XON || d == XOFF || d == 0x00 ||
        d == 0x0D || d == 0x0A;
}

/*
 * Find the start of a header.
 *
 * Returns 1 with *pos just past the frame indicator, 0 if there is not enough
 * input to decide yet, and -1 if there is input but it is not a header - a
 * stray byte, which the caller should drop and try again. Distinguishing 0
 * from -1 matters: treating a stray byte as "need more" leaves it at the
 * front of the buffer for ever, and the parser then never makes progress.
 *
 * A run of five CANs is recorded in zm->cans, which the caller checks.
 */
static int zm_find_header(Zmodem *zm, size_t *pos, bool *is_hex, bool *bin32)
{
    size_t p = 0;

    while (p < zm->inlen && zm_ignorable(zm->in[p]))
        p++;

    /* Count a run of CANs: five in a row is the peer aborting. */
    if (p < zm->inlen && zm->in[p] == ZDLE) {
        size_t q = p;
        int n = 0;
        while (q < zm->inlen && zm->in[q] == ZDLE) {
            n++;
            q++;
        }
        if (q >= zm->inlen)
            return 0;           /* the run may still be growing */
        if (n >= ZM_MAX_CANS) {
            zm->cans = n;
            *pos = q;
            return 0;
        }
    }

    if (p >= zm->inlen)
        return 0;               /* nothing left to look at */
    if (zm->in[p] != ZDLE)
        return -1;              /* a stray byte, not a header */
    if (p + 1 >= zm->inlen)
        return 0;               /* the indicator has not arrived yet */

    if (zm->in[p + 1] == ZHEX) {
        *is_hex = true;
        *bin32 = false;
        *pos = p + 2;
        return 1;
    }
    if (zm->in[p + 1] == ZBIN || zm->in[p + 1] == ZBIN32) {
        *is_hex = false;
        *bin32 = (zm->in[p + 1] == ZBIN32);
        *pos = p + 2;
        return 1;
    }

    return -1;                  /* ZDLE followed by something else */
}

/*
 * Try to parse one header from the front of zm->in.
 *
 * Returns 1 on success, 0 if more input is needed, -1 on a bad header (which
 * the caller answers with ZNAK or a nudge), -2 on a CAN abort.
 */
static int zm_parse_header(Zmodem *zm, ZmHeader *hdr, size_t *consumed)
{
    size_t p = 0;               /* zm_find_header reads this as its start */
    bool is_hex = false, bin32 = false;
    unsigned char body[5];
    int i;

    {
        int fr = zm_find_header(zm, &p, &is_hex, &bin32);
        if (fr <= 0) {
            if (zm->cans >= ZM_MAX_CANS)
                return -2;
            return fr;          /* 0 = need more input, -1 = a stray byte */
        }
    }

    if (is_hex) {
        unsigned char digits[14];
        uint16_t got, want;

        if (p + 14 > zm->inlen)
            return 0;
        for (i = 0; i < 14; i++) {
            unsigned char c = zm->in[p + i];
            if (c >= '0' && c <= '9')
                digits[i] = (unsigned char)(c - '0');
            else if (c >= 'a' && c <= 'f')
                digits[i] = (unsigned char)(c - 'a' + 10);
            else
                return -1;      /* upper case is forbidden, and so is junk */
        }
        p += 14;

        for (i = 0; i < 5; i++)
            body[i] = (unsigned char)((digits[2 * i] << 4) |
                                      digits[2 * i + 1]);

        got = (uint16_t)((digits[10] << 12) | (digits[11] << 8) |
                         (digits[12] << 4) | digits[13]);
        want = crc16(body, 5);
        if (got != want) {
            zmtrace(zm, "hex header bad CRC: got %04x want %04x", got, want);
            return -1;
        }

        /*
         * CR LF, and possibly XON, follow. zm_ignorable() tolerates the 8th
         * bit being set, which lrzsz needs, and also padding of the next
         * header, which is harmless to skip here.
         */
        while (p < zm->inlen && zm_ignorable(zm->in[p]))
            p++;
    } else {
        size_t q = p;
        ZmGetResult r;
        unsigned char b;

        for (i = 0; i < 5; i++) {
            r = zm_decode(zm, &q, &b);
            if (r != ZMGR_BYTE) {
                if (r == ZMGR_MORE)
                    return 0;
                return -1;
            }
            body[i] = b;
        }

        if (bin32) {
            uint32_t got = 0, want;
            for (i = 0; i < 4; i++) {
                r = zm_decode(zm, &q, &b);
                if (r == ZMGR_MORE)
                    return 0;
                if (r != ZMGR_BYTE)
                    return -1;
                got |= (uint32_t)b << (8 * i);
            }
            want = crc32_z(body, 5);
            if (got != want) {
                zmtrace(zm, "ZBIN32 header bad CRC: got %08lx want %08lx",
                        (unsigned long)got, (unsigned long)want);
                return -1;
            }
        } else {
            uint16_t got = 0, want;
            for (i = 0; i < 2; i++) {
                r = zm_decode(zm, &q, &b);
                if (r == ZMGR_MORE)
                    return 0;
                if (r != ZMGR_BYTE)
                    return -1;
                got = (uint16_t)((got << 8) | b);
            }
            want = crc16(body, 5);
            if (got != want) {
                zmtrace(zm, "ZBIN header bad CRC: got %04x want %04x",
                        got, want);
                return -1;
            }
        }
        p = q;
    }

    hdr->type = body[0];
    memcpy(hdr->data, body + 1, 4);
    *consumed = p;

    /*
     * The FCS width the peer uses on its data subpackets follows the encoding
     * of its headers, and is sticky for the session.
     */
    zm->rx_fcs32 = bin32;

    zmtrace(zm, "header type %d data %02x %02x %02x %02x (%s)",
            hdr->type, hdr->data[0], hdr->data[1], hdr->data[2],
            hdr->data[3], is_hex ? "hex" : (bin32 ? "bin32" : "bin16"));
    return 1;
}

/*
 * Try to parse one data subpacket starting at zm->in[start]. The decoded data
 * is assembled into zm->sub, and is valid until the next call.
 *
 * On success *consumed is the offset just past the subpacket, counted from the
 * START OF THE BUFFER and not from `start' - so a caller that began at a
 * non-zero `start' can consume the header and the subpacket in one go.
 *
 * Returns 1, 0 (need more) or -1 (bad frame check sequence).
 */
static int zm_parse_subpacket(Zmodem *zm, size_t start, ZmSubpacket *sp,
                              size_t *consumed, bool fcs32)
{
    size_t p = start;
    ZmGetResult r;
    unsigned char b;
    unsigned char frameend;

    zm->sublen = 0;

    for (;;) {
        if (zm->sublen > ZM_MAX_SUBPACKET)
            return -1;
        r = zm_decode(zm, &p, &b);
        if (r == ZMGR_MORE)
            return 0;
        if (r == ZMGR_FRAMEEND) {
            frameend = b;
            break;
        }
        if (r == ZMGR_BAD)
            return -1;
        buf_append(&zm->sub, &zm->sublen, &zm->suballoc, &b, 1);
    }

    if (fcs32) {
        uint32_t got = 0, crc = 0xFFFFFFFF, want;
        int i;
        size_t k;
        for (i = 0; i < 4; i++) {
            r = zm_decode(zm, &p, &b);
            if (r == ZMGR_MORE)
                return 0;
            if (r != ZMGR_BYTE)
                return -1;
            got |= (uint32_t)b << (8 * i);
        }
        for (k = 0; k < zm->sublen; k++)
            crc = crc32_update(crc, zm->sub[k]);
        crc = crc32_update(crc, frameend);
        want = ~crc;
        if (got != want) {
            zmtrace(zm, "subpacket bad CRC-32: got %08lx want %08lx",
                    (unsigned long)got, (unsigned long)want);
            return -1;
        }
    } else {
        uint16_t got = 0, crc = 0, want;
        int i;
        size_t k;
        for (i = 0; i < 2; i++) {
            r = zm_decode(zm, &p, &b);
            if (r == ZMGR_MORE)
                return 0;
            if (r != ZMGR_BYTE)
                return -1;
            got = (uint16_t)((got << 8) | b);
        }
        for (k = 0; k < zm->sublen; k++)
            crc = crc16_update(crc, zm->sub[k]);
        crc = crc16_update(crc, frameend);
        want = crc;
        if (got != want) {
            zmtrace(zm, "subpacket bad CRC-16: got %04x want %04x", got, want);
            return -1;
        }
    }

    sp->data = zm->sub;
    sp->len = zm->sublen;
    sp->frameend = frameend;
    *consumed = p;

    zmtrace(zm, "subpacket %lu bytes, frameend %02x",
            (unsigned long)sp->len, frameend);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Transfer control                                                    */
/* ------------------------------------------------------------------ */

static void zm_finish(Zmodem *zm, ZmodemOutcome outcome, const char *msg)
{
    if (zm->file) {
        if (zm->dir == ZMDIR_RECEIVE)
            zm->cbs.receive_close(zm->cbs.ctx, zm->file,
                                  outcome == ZMOUT_DONE);
        else
            zm->cbs.send_close(zm->cbs.ctx, zm->file);
        zm->file = NULL;
    }
    zm->state = ZMST_DONE;
    if (!zm->finished_called) {
        zm->finished_called = true;
        if (zm->cbs.finished)
            zm->cbs.finished(zm->cbs.ctx, outcome, msg);
    }
}

static void zm_set_name(Zmodem *zm, const char *name, size_t len)
{
    if (len > ZM_MAX_NAME)
        len = ZM_MAX_NAME;
    sfree(zm->name);
    zm->name = dupprintf("%.*s", (int)len, name);
}

/* Strip any directory part: the peer should not dictate our path. */
static const char *zm_basename(const char *path, size_t *len)
{
    const char *slash = strrchr(path, '/');
    const char *name = slash ? slash + 1 : path;
    if (len)
        *len = strlen(name);
    return name;
}

/*
 * The peer sent ZFILE. Parse the file information subpacket and ask the front
 * end to open the file.
 */
static void zm_rx_zfile(Zmodem *zm, ZmSubpacket *sp)
{
    const unsigned char *q;
    size_t n, i;
    const char *p;
    uint64_t size = 0;
    unsigned long mtime = 0;
    char *raw = NULL;
    const char *base;
    size_t baselen;

    if (sp->frameend != ZCRCW) {
        zmtrace(zm, "ZFILE subpacket frameend %02x, not ZCRCW", sp->frameend);
        zm_send_znak(zm);
        return;
    }

    /*
     * Layout: pathname NUL length SP moddate [SP mode [SP serial ...]] NUL
     * (reference section 6). Only the first three fields are required.
     */
    q = sp->data;
    n = sp->len;
    for (i = 0; i < n && q[i] != '\0'; i++)
        ;
    if (i >= n) {
        zmtrace(zm, "ZFILE subpacket has no NUL after the name");
        zm_send_znak(zm);
        return;
    }
    raw = dupprintf("%.*s", (int)i, (const char *)q);

    p = (const char *)q + i + 1;
    if (i + 1 < n) {
        size = strtoull(p, NULL, 10);
        {
            const char *sp2 = strchr(p, ' ');
            if (sp2)
                mtime = strtoul(sp2 + 1, NULL, 8);
        }
    }

    base = zm_basename(raw, &baselen);
    zmtrace(zm, "ZFILE name=\"%s\" (base \"%s\") size=%llu mtime=%lu",
            raw, base, (unsigned long long)size, mtime);

    zm_set_name(zm, base, baselen);
    sfree(raw);

    zm->rx_claimed = size;
    zm->rx_offset = 0;

    zm->file = zm->cbs.receive_open(zm->cbs.ctx, zm->name, size, mtime);
    if (!zm->file) {
        zmtrace(zm, "refused %s, sending ZSKIP", zm->name);
        zm_send_zskip(zm);
        return;
    }

    zm_send_hdr_pos(zm, ZRPOS, 0);
    /*
     * Wait for a header, not for data: the sender answers ZRPOS with a ZDATA
     * header and only then do subpackets follow. Going straight to RX_DATA
     * here makes the ZDATA header get parsed as if it were a subpacket, whose
     * frame check sequence then fails and costs a whole error-recovery round
     * trip - and the transfer only recovers by luck.
     */
    zm->state = ZMST_RX_READY;
}

/* ------------------------------------------------------------------ */
/* The receiver                                                        */
/* ------------------------------------------------------------------ */

static void zm_rx_header(Zmodem *zm, ZmHeader *hdr)
{
    switch (hdr->type) {
      case ZRQINIT:
        /* The sender is announcing itself; repeat our ZRINIT. */
        zm_send_zrinit(zm);
        break;

      case ZSINIT:
        /* Accepted and acknowledged; the Attn string is not used. */
        zm_send_zack(zm, 1);
        break;

      case ZDATA:
        if (zm_get_pos(hdr->data) != zm->rx_offset) {
            zmtrace(zm, "ZDATA offset %llu, have %llu: asking for a resync",
                    (unsigned long long)zm_get_pos(hdr->data),
                    (unsigned long long)zm->rx_offset);
            zm_send_hdr_pos(zm, ZRPOS, zm->rx_offset);
        } else {
            zm->state = ZMST_RX_DATA;
        }
        break;

      case ZEOF:
        if (zm_get_pos(hdr->data) == zm->rx_offset) {
            zmtrace(zm, "ZEOF at %llu: file complete",
                    (unsigned long long)zm->rx_offset);
            if (zm->file) {
                zm->cbs.receive_close(zm->cbs.ctx, zm->file, true);
                zm->file = NULL;
            }
            zm_send_zrinit(zm);       /* ready for the next file */
            zm->state = ZMST_RX_READY;
        } else {
            zmtrace(zm, "ZEOF at %llu but have %llu: ignoring",
                    (unsigned long long)zm_get_pos(hdr->data),
                    (unsigned long long)zm->rx_offset);
        }
        break;

      case ZFIN:
        zm_send_zfin(zm);
        zm_finish(zm, ZMOUT_DONE, "transfer complete");
        break;

      case ZCHALLENGE:
        zm_send_zack(zm, zm_get_pos(hdr->data));
        break;

      case ZABORT:
        zm_finish(zm, ZMOUT_CANCELLED, "the far end aborted the transfer");
        break;

      case ZFERR:
        zm_finish(zm, ZMOUT_FAILED, "the far end reported an error");
        break;

      case ZCAN:
        zm_finish(zm, ZMOUT_CANCELLED, "the far end cancelled the transfer");
        break;

      default:
        zmtrace(zm, "unexpected header type %d as receiver", hdr->type);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* The sender                                                          */
/* ------------------------------------------------------------------ */

static bool zm_tx_open_file(Zmodem *zm)
{
    char *name = NULL;
    uint64_t size = 0;
    unsigned long mtime = 0;
    char *nums;
    const char *base;
    size_t baselen, numlen;
    unsigned char *info;
    unsigned char d[4];

    zm->file = zm->cbs.send_open(zm->cbs.ctx, &name, &size, &mtime);
    if (!zm->file) {
        sfree(name);
        return false;
    }

    base = zm_basename(name ? name : "file", &baselen);
    zm_set_name(zm, base, baselen);
    sfree(name);

    zm->tx_size = size;
    zm->tx_offset = 0;
    zm->tx_sent_any = true;
    zm->tx_in_window = 0;
    zm->tx_hdr_sent = false;
    zm->tx_first_frame = true;

    /*
     * The ZFILE subpacket: name NUL length SP moddate SP mode SP serial NUL,
     * sent as a ZCRCW subpacket (reference section 6). The numeric fields are
     * decimal length, octal moddate, octal mode, octal serial.
     */
    nums = dupprintf("%llu %lo %o 0", (unsigned long long)size, mtime, 0100644);
    numlen = strlen(nums);

    info = snewn(baselen + 1 + numlen + 1, unsigned char);
    memcpy(info, zm->name, baselen);
    info[baselen] = '\0';
    memcpy(info + baselen + 1, nums, numlen);
    info[baselen + 1 + numlen] = '\0';

    zm_pos(d, 0);
    zm_send_bin_hdr(zm, ZFILE, d);
    zm_send_subpacket(zm, info, baselen + 1 + numlen + 1, ZCRCW);
    zm_out_byte(zm, XON);
    zm_out_flush(zm);

    sfree(info);
    sfree(nums);

    zmtrace(zm, "sent ZFILE for %s (%llu bytes)", zm->name,
            (unsigned long long)size);

    zm->state = ZMST_TX_FILE;
    return true;
}

/*
 * Send data frames until the window is exhausted or the file ends. Called on
 * entering the data phase and again after each acknowledgement.
 */
static void zm_tx_pump(Zmodem *zm)
{
    for (;;) {
        unsigned char buf[ZM_MAX_SUBPACKET];
        size_t n;
        unsigned char frameend;
        bool last;
        unsigned char d[4];

        if (!zm->file)
            return;

        if (zm->tx_size && zm->tx_offset >= zm->tx_size) {
            zm_send_hdr_pos(zm, ZEOF, zm->tx_offset);
            zm_out_flush(zm);
            zm->state = ZMST_TX_EOF;
            return;
        }

        n = zm->cbs.send_read(zm->cbs.ctx, zm->file, buf, sizeof(buf));
        if (n == 0) {
            zm_send_hdr_pos(zm, ZEOF, zm->tx_offset);
            zm_out_flush(zm);
            zm->state = ZMST_TX_EOF;
            return;
        }

        /*
         * A short read means end of file, which is what ends the burst with
         * a ZCRCE when the size is not known up front.
         */
        last = (n < sizeof(buf)) ||
            (zm->tx_size && zm->tx_offset + n >= zm->tx_size);

        /* A ZDATA header opens a burst; a new one is needed after each. */
        if (!zm->tx_hdr_sent) {
            zm_pos(d, zm->tx_offset);
            zm_send_bin_hdr(zm, ZDATA, d);
            zm->tx_hdr_sent = true;
            zm->tx_in_window = 0;
        }

        if (last) {
            frameend = ZCRCE;
        } else if (zm->tx_first_frame) {
            /*
             * The first subpacket of a burst is a ZCRCW, so the link is
             * flushed before the burst (reference section 8.2).
             */
            frameend = ZCRCW;
        } else if (zm->tx_in_window + n >= zm->tx_window) {
            frameend = ZCRCQ;       /* end of the window: ask for an ack */
        } else {
            frameend = ZCRCG;       /* keep streaming */
        }
        zm->tx_first_frame = false;

        zm_send_subpacket(zm, buf, n, frameend);
        zm->tx_offset += n;
        zm->tx_in_window += n;
        if (frameend == ZCRCW)
            zm_out_byte(zm, XON);
        zm_out_flush(zm);

        if (zm->cbs.progress)
            zm->cbs.progress(zm->cbs.ctx, ZMDIR_SEND, zm->name,
                             zm->tx_offset, zm->tx_size);

        if (frameend == ZCRCW) {
            /*
             * The frame is over and an acknowledgement is required, so the
             * next frame needs a fresh ZDATA header.
             */
            zm->tx_hdr_sent = false;
            zm->state = ZMST_TX_ACK;
            return;
        }
        if (frameend == ZCRCQ) {
            /*
             * We want an acknowledgement, but the frame CONTINUES: the peer is
             * still reading subpackets and is not looking for a header. So the
             * ZDATA header stays open, and the next frame must not repeat it -
             * doing so makes the peer parse a header as a subpacket, fail its
             * frame check sequence, and ask for a resync.
             */
            zm->state = ZMST_TX_ACK;
            return;
        }
        if (frameend == ZCRCE) {
            /* The burst is over; the loop sends ZEOF next. */
            zm->tx_hdr_sent = false;
            continue;
        }
        /* ZCRCG: keep streaming. */
    }
}

/* Send ZFIN and wait for the peer's reply. */
static void zm_tx_all_done(Zmodem *zm)
{
    if (zm->file) {
        zm->cbs.send_close(zm->cbs.ctx, zm->file);
        zm->file = NULL;
    }
    zm_send_zfin(zm);
    zm_out_flush(zm);
    zm->state = ZMST_TX_FIN;
}

static void zm_tx_header(Zmodem *zm, ZmHeader *hdr)
{
    switch (hdr->type) {
      case ZRINIT:
        zm->peer_flags = hdr->data[3];
        zm->peer_buflen = (unsigned long)hdr->data[0] |
            ((unsigned long)hdr->data[1] << 8);
        zm->fcs32 = (hdr->data[3] & CANFC32) != 0;
        zm->tx_window = zm->peer_buflen ? zm->peer_buflen : ZM_DEFAULT_WINDOW;

        zmtrace(zm, "ZRINIT flags %02x buflen %lu -> fcs32=%d window=%llu",
                zm->peer_flags, zm->peer_buflen, (int)zm->fcs32,
                (unsigned long long)zm->tx_window);

        if (zm->state == ZMST_TX_READY || zm->state == ZMST_TX_EOF) {
            /* Either the first file, or the previous one was accepted. */
            if (zm->file) {
                zm->cbs.send_close(zm->cbs.ctx, zm->file);
                zm->file = NULL;
            }
            if (!zm_tx_open_file(zm)) {
                /*
                 * No file to send - which is two different things, and the
                 * caller cannot tell them apart, so the state has to:
                 *
                 *  - nothing has been sent yet, so the user refused the file
                 *    (or it could not be opened). That is an abort. Ending
                 *    with ZFIN instead - as this used to - tells the peer the
                 *    transfer completed, it answers ZFIN, and the user is told
                 *    that a transfer which never happened was "complete".
                 *
                 *  - a file has already been sent and the peer is asking for
                 *    another one. That is an ordinary end of session, and ZFIN
                 *    is exactly right.
                 */
                if (!zm->tx_sent_any) {
                    zmtrace(zm, "no file to send: aborting");
                    zm_send_can(zm);
                    zm_finish(zm, ZMOUT_CANCELLED, "no file was sent");
                    return;
                }
                zm_tx_all_done(zm);
            }
        }
        /* A repeat while mid-file is an echo: ignore it. */
        break;

      case ZRPOS:
        {
            uint64_t pos = zm_get_pos(hdr->data);
            zmtrace(zm, "ZRPOS %llu (have %llu)",
                    (unsigned long long)pos,
                    (unsigned long long)zm->tx_offset);
            zm->tx_offset = pos;
            zm->tx_in_window = 0;
            zm->tx_hdr_sent = false;
            zm->tx_first_frame = true;
            zm->state = ZMST_TX_DATA;
            zm_tx_pump(zm);
        }
        break;

      case ZACK:
        if (zm->state == ZMST_TX_ACK || zm->state == ZMST_TX_DATA) {
            zm->tx_in_window = 0;
            zm->state = ZMST_TX_DATA;
            zm_tx_pump(zm);
        } else if (zm->state == ZMST_TX_EOF) {
            /* The peer wants the ZEOF again. */
            zm_send_hdr_pos(zm, ZEOF, zm->tx_offset);
        }
        break;

      case ZSKIP:
        zmtrace(zm, "ZSKIP: the far end refused the file");
        zm_tx_all_done(zm);
        break;

      case ZNAK:
        /* Our last header was garbled: repeat it. */
        zm_send_znak(zm);
        break;

      case ZCHALLENGE:
        zm_send_zack(zm, zm_get_pos(hdr->data));
        break;

      case ZABORT:
      case ZCAN:
        zm_finish(zm, ZMOUT_CANCELLED, "the far end cancelled the transfer");
        break;

      case ZFERR:
        zm_finish(zm, ZMOUT_FAILED, "the far end reported an error");
        break;

      case ZFIN:
        /*
         * The peer accepted our ZFIN. "Over and out" is the sender's last
         * word: two literal characters, and no header (section 8.4).
         */
        zm_out_raw(zm, "OO", 2);
        zm_out_flush(zm);
        zm_finish(zm, ZMOUT_DONE, "transfer complete");
        break;

      default:
        zmtrace(zm, "unexpected header type %d as sender", hdr->type);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* The main pump                                                       */
/* ------------------------------------------------------------------ */

static void zm_run(Zmodem *zm)
{
    for (;;) {
        size_t consumed = 0;
        int r;

        if (zm->state == ZMST_DONE || zm->state == ZMST_IDLE)
            return;

        if (zm->cans >= ZM_MAX_CANS) {
            zm_finish(zm, ZMOUT_CANCELLED,
                      "the far end sent a cancel sequence");
            return;
        }

        /*
         * Inside a ZDATA burst, subpackets come first and a header only after
         * the burst ends. Outside one, we are always looking for a header.
         */
        if (zm->state == ZMST_RX_DATA) {
            ZmSubpacket sp;
            r = zm_parse_subpacket(zm, 0, &sp, &consumed, zm->rx_fcs32);
            if (r == 0)
                return;                 /* need more input */
            if (r < 0) {
                if (++zm->errors > ZM_MAX_ERRORS) {
                    zm_finish(zm, ZMOUT_FAILED, "too many bad subpackets");
                    return;
                }
                /*
                 * The subpacket boundary is unknown after a bad frame check
                 * sequence, so ask the sender to restart the burst at the
                 * last good offset and go back to reading headers.
                 */
                if (zm->errors == 1)
                    zm_send_hdr_pos(zm, ZRPOS, zm->rx_offset);
                zm_out_flush(zm);
                buf_consume(zm->in, &zm->inlen, 1);
                zm->state = ZMST_RX_READY;
                continue;
            }
            zm->errors = 0;

            if (zm->file && sp.len) {
                zm->cbs.receive_write(zm->cbs.ctx, zm->file, sp.data, sp.len);
                zm->rx_offset += sp.len;
                if (zm->cbs.progress)
                    zm->cbs.progress(zm->cbs.ctx, ZMDIR_RECEIVE, zm->name,
                                     zm->rx_offset, zm->rx_claimed);
            }
            buf_consume(zm->in, &zm->inlen, consumed);

            switch (sp.frameend) {
              case ZCRCG:
                continue;               /* no response */
              case ZCRCQ:
                zm_send_zack(zm, zm->rx_offset);
                zm_out_flush(zm);
                continue;
              case ZCRCW:
                zm_send_zack(zm, zm->rx_offset);
                zm_out_byte(zm, XON);
                zm_out_flush(zm);
                zm->state = ZMST_RX_READY;
                break;
              default:                  /* ZCRCE */
                zm->state = ZMST_RX_READY;
                break;
            }
            continue;
        }

        /* Looking for a header. */
        {
            ZmHeader hdr;
            r = zm_parse_header(zm, &hdr, &consumed);
            if (r == 0)
                return;
            if (r == -2) {
                zm_finish(zm, ZMOUT_CANCELLED,
                          "the far end sent a cancel sequence");
                return;
            }
            if (r < 0) {
                if (++zm->errors > ZM_MAX_ERRORS) {
                    zm_finish(zm, ZMOUT_FAILED, "too many bad headers");
                    return;
                }
                buf_consume(zm->in, &zm->inlen, 1);
                /*
                 * Ask for a resync only when a file is actually open. Before
                 * that there is nothing to resync to, and a ZRPOS arriving
                 * out of context makes the sender restart from zero - the
                 * sender reads it after its next data frame, by which time it
                 * has already moved on.
                 */
                if (zm->dir == ZMDIR_RECEIVE && zm->file && zm->errors == 1)
                    zm_send_hdr_pos(zm, ZRPOS, zm->rx_offset);
                zm_out_flush(zm);
                continue;
            }
            zm->errors = 0;
            zm->cans = 0;

            if (zm->dir == ZMDIR_RECEIVE && hdr.type == ZFILE) {
                /*
                 * ZFILE is followed by a ZCRCW subpacket holding the file
                 * information. Nothing is consumed until both are available,
                 * so a partial read simply re-parses later.
                 */
                ZmSubpacket sp;
                size_t sc = 0;
                int sr = zm_parse_subpacket(zm, consumed, &sp, &sc,
                                            zm->rx_fcs32);
                if (sr == 0)
                    return;
                /* sc is absolute, so it already covers the header too. */
                if (sr < 0) {
                    if (++zm->errors > ZM_MAX_ERRORS) {
                        zm_finish(zm, ZMOUT_FAILED, "too many bad subpackets");
                        return;
                    }
                    /*
                     * Drop the header as well: sc is not set on failure, and
                     * consuming nothing would re-parse the same header for
                     * ever.
                     */
                    buf_consume(zm->in, &zm->inlen, consumed);
                    zm_send_znak(zm);
                    zm_out_flush(zm);
                    continue;
                }
                buf_consume(zm->in, &zm->inlen, sc);
                zm_rx_zfile(zm, &sp);
            } else {
                buf_consume(zm->in, &zm->inlen, consumed);
                if (zm->dir == ZMDIR_RECEIVE)
                    zm_rx_header(zm, &hdr);
                else
                    zm_tx_header(zm, &hdr);
            }
            zm_out_flush(zm);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Public interface                                                    */
/* ------------------------------------------------------------------ */

Zmodem *zmodem_new(const ZmodemCallbacks *cbs)
{
    Zmodem *zm;

    crc_tabs_init();

    zm = snew(Zmodem);
    memset(zm, 0, sizeof(*zm));
    zm->cbs = *cbs;
    zm->state = ZMST_IDLE;
    zm->tx_window = ZM_DEFAULT_WINDOW;

    zmtrace(zm, "engine created");
    return zm;
}

void zmodem_free(Zmodem *zm)
{
    if (!zm)
        return;
    if (zm->file) {
        /* Cut short: tell the front end so it can clean up. */
        if (zm->dir == ZMDIR_RECEIVE)
            zm->cbs.receive_close(zm->cbs.ctx, zm->file, false);
        else
            zm->cbs.send_close(zm->cbs.ctx, zm->file);
    }
    sfree(zm->name);
    sfree(zm->in);
    sfree(zm->sub);
    sfree(zm->out);
    sfree(zm);
}

/* The prologue: "**" ZDLE 'B' '0' then '0' (ZRQINIT) or '1' (ZRINIT). */
static const unsigned char zm_prologue[] = { 0x2A, 0x2A, 0x18, 0x42, 0x30 };
#define ZM_PROLOGUE_LEN 6

/* True if c continues a partial prologue of length `have'. */
static bool zm_pro_continues(size_t have, unsigned char c)
{
    if (have < 5)
        return c == zm_prologue[have];
    if (have == 5)
        return c == '0' || c == '1';
    return false;
}

static void zm_emit(Zmodem *zm, const void *data, size_t len)
{
    if (len && zm->cbs.terminal_output)
        zm->cbs.terminal_output(zm->cbs.ctx, data, len);
}

/*
 * Put the engine back to looking for a prologue. Called once the previous
 * transfer has been reported, so that a session can carry more than one: the
 * engine lives for the whole session, and without this it would sit in DONE
 * and quietly swallow everything that followed.
 */
static void zm_reset(Zmodem *zm)
{
    assert(zm->file == NULL);   /* zm_finish() closed it */
    sfree(zm->name);
    zm->name = NULL;
    zm->inlen = 0;
    zm->sublen = 0;
    zm->outlen = 0;
    zm->prolen = 0;
    zm->cans = 0;
    zm->errors = 0;
    zm->tx_sent_any = false;
    zm->finished_called = false;
    zm->state = ZMST_IDLE;
}

void zmodem_feed_in(Zmodem *zm, const void *vdata, size_t len)
{
    const unsigned char *data = (const unsigned char *)vdata;
    size_t i = 0, run = 0;

    if (zm->state == ZMST_DONE)
        zm_reset(zm);

    if (zm->state != ZMST_IDLE) {
        /* Already in a transfer: everything is protocol. */
        buf_append(&zm->in, &zm->inlen, &zm->inalloc, data, len);
        zm_run(zm);
        return;
    }

    /*
     * Scan for the prologue.
     *
     * zm->pro holds a partial match, which may have started in a previous
     * buffer; the held bytes are always a proper prefix of the pattern, so at
     * most five are held. `run' is the start of the region of *this* buffer
     * that has not yet been passed to the terminal.
     *
     * When a match fails, the held bytes have to come out first, and the part
     * of them that lives in this buffer is data[run..i) - so the count from
     * previous buffers is zm->prolen - (i - run).
     */
    while (i < len) {
        unsigned char c = data[i];

        if (zm_pro_continues(zm->prolen, c)) {
            if (zm->prolen == 0) {
                /* The attempt starts here: release what is before it. */
                zm_emit(zm, data + run, i - run);
                run = i;
            }
            zm->pro[zm->prolen++] = c;
            i++;

            if (zm->prolen == ZM_PROLOGUE_LEN) {
                /* Found it: everything from here on is protocol. */
                zm->dir = (zm->pro[5] == '0') ? ZMDIR_RECEIVE : ZMDIR_SEND;
                zm->state = (zm->dir == ZMDIR_RECEIVE) ? ZMST_RX_READY
                                                       : ZMST_TX_READY;
                zmtrace(zm, "prologue found: %s",
                        zm->dir == ZMDIR_RECEIVE ? "receiving" : "sending");

                buf_append(&zm->in, &zm->inlen, &zm->inalloc,
                           zm->pro, ZM_PROLOGUE_LEN);
                zm->prolen = 0;
                buf_append(&zm->in, &zm->inlen, &zm->inalloc,
                           data + i, len - i);
                zm_run(zm);
                return;
            }
            continue;
        }

        if (zm->prolen) {
            /* False start: release the held bytes, oldest first. */
            size_t in_this_buf = i - run;
            zm_emit(zm, zm->pro, zm->prolen - in_this_buf);
            zm_emit(zm, data + run, in_this_buf);
            zm->prolen = 0;
            run = i;
        }

        if (zm_pro_continues(0, c)) {
            zm->pro[0] = c;
            zm->prolen = 1;
            run = i;
        }
        i++;
    }

    if (zm->prolen == 0)
        zm_emit(zm, data + run, len - run);
    /* Otherwise data[run..len) is held until the match resolves. */
}

bool zmodem_active(Zmodem *zm)
{
    return zm && zm->state != ZMST_IDLE && zm->state != ZMST_DONE;
}

void zmodem_key(Zmodem *zm, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i;

    if (!zmodem_active(zm))
        return;

    for (i = 0; i < len; i++) {
        if (p[i] == 0x03) {         /* Ctrl-C cancels, as in rzsz */
            zmodem_cancel(zm);
            return;
        }
    }
    zmtrace(zm, "ignored %lu bytes of typed input during a transfer",
            (unsigned long)len);
}

void zmodem_cancel(Zmodem *zm)
{
    if (!zmodem_active(zm))
        return;
    zmtrace(zm, "cancelled by the user");
    zm_send_can(zm);
    zm_finish(zm, ZMOUT_CANCELLED, "cancelled");
}

const char *zmodem_current_name(Zmodem *zm)
{
    return zm->name ? zm->name : "";
}
