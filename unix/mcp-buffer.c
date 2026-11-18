/*
 * PuTTY-beter: the MCP capture buffer.
 *
 * See mcp-buffer.h for the interface and PLAN.md section 7.4 for the design.
 *
 * Three things happen to the byte stream here, in this order:
 *
 *   1. Escape sequences are stripped. A network CLI does not use colour or
 *      cursor addressing as *content*, and the client wants text. This is a
 *      state machine rather than a regular expression because a sequence can
 *      straddle a read boundary, and because an OSC string can contain
 *      anything at all up to BEL or ST.
 *   2. The remaining bytes are assembled into lines. A carriage return with
 *      nothing after it means "back to column 0" - a progress bar, not a line
 *      end - so a bare CR discards the line so far, while CR LF ends it.
 *   3. Completed lines go into a sliding window over absolute ids, evicting
 *      the oldest first when the byte budget is exceeded.
 *
 * The line currently being written is kept separately from the window, because
 * it has no newline yet: that is exactly the state a shell or a router prompt
 * is in when it is waiting for you, and it is the line a client most wants to
 * see. It is reported by mcp_buffer_read() but does not move the cursor, since
 * it can still grow.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "misc.h"
#include "mcp-buffer.h"

/* ------------------------------------------------------------------ */
/* One line of output                                                  */
/* ------------------------------------------------------------------ */

struct McpLine {
    uint64_t id;
    McpAuthor author;
    size_t len;
    size_t alloc;
    char *text;
};

struct McpBuffer {
    /* The sliding window, as a circular array of `count' lines from `head'. */
    struct McpLine *lines;
    size_t head;
    size_t count;
    size_t alloc;
    size_t bytes;

    size_t budget;

    /* The id the next line - completed or in progress - will take. */
    uint64_t next_id;

    /* The line being written, if any. Its id is `next_id'. */
    bool cur_active;
    struct McpLine cur;
    McpAuthor cur_author;

    /* Escape-sequence stripper state. */
    enum {
        ESC_NONE = 0,
        ESC_ESC,          /* seen ESC */
        ESC_CSI,          /* ESC [ ... */
        ESC_INTERMED,     /* ESC <0x20-0x2f> ... */
        ESC_OSC,          /* ESC ] ... BEL or ST */
        ESC_OSC_ESC,      /* ESC ] ... ESC */
        ESC_DCS,          /* ESC P/X/^/_ ... ST */
        ESC_DCS_ESC,      /* ... ESC */
    } esc;

    /* A CR has been seen; the next byte says whether it was a line end. */
    bool pending_cr;
};

char mcp_author_tag(McpAuthor author)
{
    switch (author) {
      case MCP_AUTHOR_HUMAN: return 'h';
      case MCP_AUTHOR_MCP:   return 'm';
      default:               return 'd';
    }
}

/* ------------------------------------------------------------------ */
/* The line being written                                              */
/* ------------------------------------------------------------------ */

static void cur_reserve(McpBuffer *b, size_t want)
{
    if (want <= b->cur.alloc)
        return;
    size_t alloc = b->cur.alloc ? b->cur.alloc : 256;
    while (alloc < want)
        alloc *= 2;
    b->cur.text = sresize(b->cur.text, alloc, char);
    b->cur.alloc = alloc;
}

/* Start a fresh, empty in-progress line. */
static void cur_start(McpBuffer *b)
{
    if (b->cur_active)
        return;
    b->cur_active = true;
    b->cur.id = b->next_id;
    b->cur.author = b->cur_author;
    b->cur.len = 0;
}

/* Throw the in-progress line away: a bare CR went back to column 0. */
static void cur_discard(McpBuffer *b)
{
    b->cur_active = false;
    b->cur.len = 0;
}

static void window_push(McpBuffer *b, struct McpLine *line);
static void window_evict(McpBuffer *b);

/*
 * Turn the in-progress line into a window line. An empty line is a real line
 * (a blank line in the output), so this is called even when nothing was typed
 * since the last newline.
 */
static void cur_finalise(McpBuffer *b)
{
    struct McpLine line;

    line.id = b->next_id;
    line.author = b->cur_active ? b->cur.author : b->cur_author;
    line.len = b->cur_active ? b->cur.len : 0;
    line.alloc = b->cur_active ? b->cur.alloc : 0;
    line.text = b->cur_active ? b->cur.text : NULL;

    /* Ownership moves into the window; the in-progress line starts over. */
    b->cur.text = NULL;
    b->cur.alloc = 0;
    b->cur.len = 0;
    b->cur_active = false;

    b->next_id++;
    window_push(b, &line);
}

static void cur_append(McpBuffer *b, const void *data, size_t len)
{
    const char *p = data;

    while (len) {
        size_t space, take;

        cur_start(b);
        space = MCP_BUFFER_CHUNK - b->cur.len;
        take = len < space ? len : space;

        cur_reserve(b, b->cur.len + take + 1);
        memcpy(b->cur.text + b->cur.len, p, take);
        b->cur.len += take;
        p += take;
        len -= take;

        /*
         * A line longer than the chunk limit is cut into chunks, so that one
         * unterminated line cannot fill the whole budget. The chunks are
         * ordinary lines; nothing renumbers.
         */
        if (b->cur.len == MCP_BUFFER_CHUNK)
            cur_finalise(b);
    }
}

static void cur_backspace(McpBuffer *b)
{
    if (b->cur_active && b->cur.len > 0)
        b->cur.len--;
}

/* ------------------------------------------------------------------ */
/* The sliding window                                                  */
/* ------------------------------------------------------------------ */

static struct McpLine *line_slot(McpBuffer *b, size_t i)
{
    return &b->lines[(b->head + i) % b->alloc];
}

static void window_push(McpBuffer *b, struct McpLine *line)
{
    if (b->count == b->alloc) {
        size_t newalloc = b->alloc ? b->alloc * 2 : 64;
        struct McpLine *newlines = snewn(newalloc, struct McpLine);
        for (size_t i = 0; i < b->count; i++)
            newlines[i] = *line_slot(b, i);
        sfree(b->lines);
        b->lines = newlines;
        b->alloc = newalloc;
        b->head = 0;
    }

    *line_slot(b, b->count) = *line;
    b->count++;
    b->bytes += line->len;

    while (b->bytes > b->budget && b->count > 1)
        window_evict(b);
}

static void window_evict(McpBuffer *b)
{
    struct McpLine *oldest = line_slot(b, 0);

    b->bytes -= oldest->len;
    sfree(oldest->text);
    oldest->text = NULL;
    b->head = (b->head + 1) % b->alloc;
    b->count--;
}

/*
 * The line with this absolute id, or NULL if it is outside the window.
 * Only completed lines live here; the in-progress one is handled separately.
 */
static struct McpLine *line_by_id(McpBuffer *b, uint64_t id)
{
    uint64_t first;

    if (!b->count)
        return NULL;
    first = line_slot(b, 0)->id;
    if (id < first || id >= first + b->count)
        return NULL;
    return line_slot(b, (size_t)(id - first));
}

/* ------------------------------------------------------------------ */
/* Escape stripping and line assembly                                  */
/* ------------------------------------------------------------------ */

static void buf_put_byte(McpBuffer *b, unsigned char c)
{
    /*
     * A CR means "back to column 0" unless a LF follows, which makes it a
     * line ending. Deciding on the *next* byte is what lets a progress bar
     * (which redraws one line with bare CRs) not turn into one line per
     * redraw.
     */
    if (b->pending_cr) {
        b->pending_cr = false;
        if (c == '\n') {
            cur_finalise(b);
            /*
             * A device newline ends the line, and the next one is the
             * device's - which is how "the human typed this" lasts for
             * exactly one line: the echo builds the human's line, and the
             * moment it ends the attribution goes back.
             */
            b->cur_author = MCP_AUTHOR_DEVICE;
            return;
        }
        cur_discard(b);
    }

    switch (c) {
      case '\r':
        b->pending_cr = true;
        return;
      case '\n':
        cur_finalise(b);
        b->cur_author = MCP_AUTHOR_DEVICE;
        return;
      case '\b':
      case 0x7f:
        cur_backspace(b);
        return;
      case '\t':
        cur_append(b, "\t", 1);
        return;
      default:
        if (c < 0x20)
            return;            /* other C0 controls are not content */
        cur_append(b, &c, 1);
        return;
    }
}

void mcp_buffer_feed(McpBuffer *b, const void *vdata, size_t len)
{
    const unsigned char *data = vdata;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = data[i];

        switch (b->esc) {
          case ESC_NONE:
            if (c == 0x1b) {
                b->esc = ESC_ESC;
            } else {
                buf_put_byte(b, c);
            }
            break;

          case ESC_ESC:
            switch (c) {
              case '[':
                b->esc = ESC_CSI;
                break;
              case ']':
                b->esc = ESC_OSC;
                break;
              case 'P': case 'X': case '^': case '_':
                b->esc = ESC_DCS;
                break;
              default:
                /*
                 * ESC followed by an intermediate byte (0x20-0x2f) - charset
                 * selection, "ESC # 8" and so on - runs on to a final byte.
                 * Anything else (ESC =, ESC 7, ESC M, ESC c ...) is a
                 * complete two-byte sequence and is simply dropped.
                 */
                b->esc = (c >= 0x20 && c <= 0x2f) ? ESC_INTERMED : ESC_NONE;
                break;
            }
            break;

          case ESC_CSI:
            if (c == 0x1b)
                b->esc = ESC_ESC;              /* aborted sequence */
            else if (c >= 0x40 && c <= 0x7e)
                b->esc = ESC_NONE;             /* final byte */
            break;

          case ESC_INTERMED:
            if (c == 0x1b)
                b->esc = ESC_ESC;
            else if (c >= 0x30 && c <= 0x7e)
                b->esc = ESC_NONE;
            break;

          case ESC_OSC:
            if (c == 0x1b)
                b->esc = ESC_OSC_ESC;
            else if (c == 0x07)
                b->esc = ESC_NONE;             /* BEL terminates an OSC */
            break;

          case ESC_OSC_ESC:
            if (c == '\\')
                b->esc = ESC_NONE;             /* ST */
            else if (c != 0x1b)
                b->esc = ESC_OSC;
            break;

          case ESC_DCS:
            if (c == 0x1b)
                b->esc = ESC_DCS_ESC;
            break;

          case ESC_DCS_ESC:
            if (c == '\\')
                b->esc = ESC_NONE;             /* ST */
            else if (c != 0x1b)
                b->esc = ESC_DCS;
            break;
        }
    }
}

void mcp_buffer_note_sent(McpBuffer *b, McpAuthor author,
                          const void *data, size_t len)
{
    const unsigned char *p = data;

    /*
     * Start a line of our own.
     *
     * If the far end is part-way through one - a prompt with no newline after
     * it is the normal case - close it first. Otherwise what we send is merged
     * into device output and cannot be told apart from it, which defeats the
     * whole point of tagging. The screen is still available through screen()
     * for what it literally looks like.
     */
    if (b->cur_active && b->cur.len)
        cur_finalise(b);
    b->pending_cr = false;

    b->cur_author = author;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = p[i];

        /*
         * A CR from us *is* a line ending, and must be treated as one here
         * rather than left for the next byte to decide. We sent the Return;
         * the far end's echo of it arrives as its own bytes, and if we were
         * still holding a pending CR when those arrived they would be read as
         * "back to column 0" and discard the line we had just written.
         */
        if (c == '\r' || c == '\n') {
            if (b->cur_active && b->cur.len)
                cur_finalise(b);
            else
                cur_discard(b);
        } else {
            buf_put_byte(b, c);
        }
    }
    b->cur_author = MCP_AUTHOR_DEVICE;
}

/*
 * A line the human is typing.
 *
 * The bytes are deliberately **not** written here. The device echoes what the
 * human types, and that echo is the record of it: one copy, in the device's
 * own words, instead of our copy plus the echo - which is how "ls" turned into
 * "llss", because the echo of each character landed inside the line we had just
 * written.
 *
 * Not writing them also keeps a password out of the transcript. A device does
 * not echo a password, so nothing is written for one - and the tap, which does
 * see those keystrokes, never records them. Writing them and trying to drop the
 * echo instead would have put the password in the transcript in clear text.
 *
 * So all this does is say who the next line belongs to.
 */
void mcp_buffer_note_typed(McpBuffer *b, const void *data, size_t len)
{
    (void)data;

    if (!len)
        return;

    /*
     * Close a line the *device* is part-way through - a prompt with no newline
     * after it is the normal case - so that what the human types is not merged
     * into device output. A line the human is already typing is left alone:
     * their keystrokes arrive one at a time and belong on the same line.
     */
    if (b->cur_active && b->cur.len && b->cur.author != MCP_AUTHOR_HUMAN)
        cur_finalise(b);

    b->cur_author = MCP_AUTHOR_HUMAN;

    /*
     * Not a pending CR either: it belongs to whatever the device was doing,
     * and leaving it set would discard the human's first character.
     */
    b->pending_cr = false;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

McpBuffer *mcp_buffer_new(size_t budget)
{
    McpBuffer *b = snew(McpBuffer);

    memset(b, 0, sizeof(*b));
    b->budget = budget ? budget : MCP_BUFFER_DEFAULT_BUDGET;
    b->next_id = 1;
    b->cur_author = MCP_AUTHOR_DEVICE;
    return b;
}

void mcp_buffer_free(McpBuffer *b)
{
    if (!b)
        return;
    for (size_t i = 0; i < b->count; i++)
        sfree(line_slot(b, i)->text);
    sfree(b->lines);
    sfree(b->cur.text);
    sfree(b);
}

uint64_t mcp_buffer_first_id(McpBuffer *b)
{
    return b->count ? line_slot(b, 0)->id : 0;
}

uint64_t mcp_buffer_last_id(McpBuffer *b)
{
    return b->next_id - 1;
}

uint64_t mcp_buffer_total_lines(McpBuffer *b)
{
    return b->next_id - 1;
}

size_t mcp_buffer_count(McpBuffer *b)
{
    return b->count;
}

size_t mcp_buffer_bytes(McpBuffer *b)
{
    return b->bytes;
}

/* ------------------------------------------------------------------ */
/* Reading                                                             */
/* ------------------------------------------------------------------ */

static void rl_add(McpReadResult *r, size_t *alloc,
                   uint64_t id, McpAuthor author, const char *text, size_t len)
{
    McpReadLine *l;

    if (r->nlines == *alloc) {
        *alloc = *alloc ? *alloc * 2 : 16;
        r->lines = sresize(r->lines, *alloc, McpReadLine);
    }

    l = &r->lines[r->nlines++];
    l->id = id;
    l->author = author;
    l->len = len;
    l->text = snewn(len + 1, char);
    if (len)
        memcpy(l->text, text, len);
    l->text[len] = '\0';
}

/*
 * A single line too long for the whole read budget: keep its head and its
 * tail, because the head is the command echo and the tail is the prompt.
 */
static void rl_add_truncated(McpReadResult *r, size_t *alloc,
                             uint64_t id, McpAuthor author,
                             const char *text, size_t len, size_t budget)
{
    const char *mark = "\n... [line truncated] ...\n";
    size_t marklen = strlen(mark);
    size_t head, tail, total;
    char *buf;

    if (budget <= marklen + 2) {
        rl_add(r, alloc, id, author, text, len < budget ? len : budget);
        return;
    }

    head = (budget - marklen) / 4;
    tail = budget - marklen - head;
    if (head > len)
        head = len;
    if (tail > len - head)
        tail = len - head;

    total = head + marklen + tail;
    buf = snewn(total + 1, char);
    memcpy(buf, text, head);
    memcpy(buf + head, mark, marklen);
    memcpy(buf + head + marklen, text + len - tail, tail);
    buf[total] = '\0';

    rl_add(r, alloc, id, author, buf, total);
    sfree(buf);
}

McpReadResult *mcp_buffer_read(McpBuffer *b, uint64_t since, size_t max_bytes)
{
    McpReadResult *r = snew(McpReadResult);
    size_t alloc = 0, used = 0;
    uint64_t first, last, start;

    memset(r, 0, sizeof(*r));
    if (!max_bytes)
        max_bytes = MCP_READ_DEFAULT_BUDGET;

    first = mcp_buffer_first_id(b);
    last = mcp_buffer_last_id(b);
    r->cursor = since;
    r->total_lines = last;

    /* Everything the caller asked for that has already been evicted. */
    if (first && since + 1 < first) {
        r->missing_from = since + 1;
        r->missing_to = first - 1;
    }

    start = since + 1;
    if (start < first)
        start = first;

    if (first) {
        for (uint64_t id = start; id <= last; id++) {
            struct McpLine *l = line_by_id(b, id);
            if (!l)
                break;

            if (r->nlines > 0 && used + l->len > max_bytes)
                break;                 /* paging, not truncation */

            if (l->len > max_bytes) {
                rl_add_truncated(r, &alloc, id, l->author, l->text, l->len,
                                 max_bytes);
                used = max_bytes;
                r->cursor = id;
                break;
            }

            rl_add(r, &alloc, id, l->author, l->text, l->len);
            used += l->len;
            r->cursor = id;
        }
    }

    r->omitted = (last > r->cursor) ? last - r->cursor : 0;

    /*
     * The line in progress, if any. Appended whatever the cursor says, because
     * it is the line that can still grow - and it is usually the prompt the
     * caller is waiting for.
     *
     * It is never truncated, though. A fragment of a growing line is worse
     * than nothing: it would tell the caller the prompt is "Rout". It is not
     * counted in `omitted' either, and nothing is lost by leaving it out,
     * because it does not move the cursor - the next read shows it in full.
     */
    if (b->cur_active && b->cur.len > 0 &&
        used + b->cur.len <= max_bytes) {
        rl_add(r, &alloc, b->next_id, b->cur.author,
               b->cur.text, b->cur.len);
    }

    return r;
}

void mcp_read_result_free(McpReadResult *r)
{
    if (!r)
        return;
    for (size_t i = 0; i < r->nlines; i++)
        sfree(r->lines[i].text);
    sfree(r->lines);
    sfree(r);
}
