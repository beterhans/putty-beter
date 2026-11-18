/*
 * Tests for the MCP capture buffer (unix/mcp-buffer.c).
 *
 * The buffer is the part of the MCP server that decides what a client is told
 * a session printed, so it is checked without a socket, a display or a server:
 * escape stripping, line assembly (including a bare CR, which is a progress
 * bar rather than a line ending), absolute ids that survive eviction, the
 * explicit gap when a cursor has been evicted, and paging under a read budget.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "mcp-buffer.h"

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

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void feed(McpBuffer *b, const char *text)
{
    mcp_buffer_feed(b, text, strlen(text));
}

/* Feed a byte string with escapes in it, so it is not mistaken for C. */
static void feed_raw(McpBuffer *b, const char *bytes, size_t len)
{
    mcp_buffer_feed(b, bytes, len);
}

static const char *line_at(McpReadResult *r, size_t i)
{
    return (i < r->nlines) ? r->lines[i].text : "<no such line>";
}

/* The whole window, one line per entry, joined with '|'. */
static char *join_lines(McpReadResult *r)
{
    size_t len = 1;
    char *out;

    for (size_t i = 0; i < r->nlines; i++)
        len += r->lines[i].len + 1;
    out = snewn(len, char);
    out[0] = '\0';
    for (size_t i = 0; i < r->nlines; i++) {
        if (i)
            strcat(out, "|");
        strcat(out, r->lines[i].text);
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static void test_plain_lines(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;

    feed(b, "hello\nworld\r\n");
    CHECK(mcp_buffer_total_lines(b) == 2, "total_lines %llu, want 2",
          (unsigned long long)mcp_buffer_total_lines(b));
    CHECK(mcp_buffer_count(b) == 2, "count %lu, want 2",
          (unsigned long)mcp_buffer_count(b));
    CHECK(mcp_buffer_first_id(b) == 1, "first_id %llu, want 1",
          (unsigned long long)mcp_buffer_first_id(b));
    CHECK(mcp_buffer_last_id(b) == 2, "last_id %llu, want 2",
          (unsigned long long)mcp_buffer_last_id(b));

    r = mcp_buffer_read(b, 0, 0);
    CHECK(r->nlines == 2, "nlines %lu, want 2", (unsigned long)r->nlines);
    CHECK(!strcmp(line_at(r, 0), "hello"), "line 0 is \"%s\"", line_at(r, 0));
    CHECK(!strcmp(line_at(r, 1), "world"), "line 1 is \"%s\"", line_at(r, 1));
    CHECK(r->cursor == 2, "cursor %llu, want 2", (unsigned long long)r->cursor);
    CHECK(r->total_lines == 2, "total_lines %llu, want 2",
          (unsigned long long)r->total_lines);
    CHECK(r->omitted == 0, "omitted %llu, want 0",
          (unsigned long long)r->omitted);
    CHECK(r->missing_to == 0, "unexpected gap %llu..%llu",
          (unsigned long long)r->missing_from,
          (unsigned long long)r->missing_to);
    mcp_read_result_free(r);

    /* Reading again from the cursor returns nothing new. */
    r = mcp_buffer_read(b, 2, 0);
    CHECK(r->nlines == 0, "second read returned %lu lines",
          (unsigned long)r->nlines);
    CHECK(r->cursor == 2, "second read cursor %llu, want 2",
          (unsigned long long)r->cursor);
    mcp_read_result_free(r);

    mcp_buffer_free(b);
}

static void test_blank_lines(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;
    char *joined;

    feed(b, "a\n\nb\n");
    CHECK(mcp_buffer_total_lines(b) == 3, "total_lines %llu, want 3",
          (unsigned long long)mcp_buffer_total_lines(b));

    r = mcp_buffer_read(b, 0, 0);
    joined = join_lines(r);
    CHECK(!strcmp(joined, "a||b"), "window is \"%s\", want \"a||b\"", joined);
    sfree(joined);
    mcp_read_result_free(r);

    mcp_buffer_free(b);
}

/*
 * The prompt is the line the client is waiting for, and it has no newline
 * after it. It must be visible, and it must not move the cursor, because it
 * can still grow.
 */
static void test_partial_line(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;

    feed(b, "Router>");
    CHECK(mcp_buffer_total_lines(b) == 0, "a partial line counted as complete");
    CHECK(mcp_buffer_last_id(b) == 0, "last_id %llu, want 0",
          (unsigned long long)mcp_buffer_last_id(b));

    r = mcp_buffer_read(b, 0, 0);
    CHECK(r->nlines == 1, "nlines %lu, want 1", (unsigned long)r->nlines);
    CHECK(!strcmp(line_at(r, 0), "Router>"), "line is \"%s\"", line_at(r, 0));
    CHECK(r->lines[0].id == 1, "partial id %llu, want 1",
          (unsigned long long)r->lines[0].id);
    CHECK(r->cursor == 0, "cursor %llu, want 0 (a partial must not advance it)",
          (unsigned long long)r->cursor);
    CHECK(r->omitted == 0, "omitted %llu, want 0",
          (unsigned long long)r->omitted);
    mcp_read_result_free(r);

    /* Even with a cursor past it, the growing line is still reported. */
    r = mcp_buffer_read(b, 1, 0);
    CHECK(r->nlines == 1 && !strcmp(line_at(r, 0), "Router>"),
          "the partial line was dropped when since >= its id");
    mcp_read_result_free(r);

    /* Once the newline arrives it becomes an ordinary line, id unchanged. */
    feed(b, " show version\n");
    CHECK(mcp_buffer_total_lines(b) == 1, "total_lines %llu, want 1",
          (unsigned long long)mcp_buffer_total_lines(b));

    r = mcp_buffer_read(b, 0, 0);
    CHECK(r->nlines == 1, "nlines %lu, want 1", (unsigned long)r->nlines);
    CHECK(!strcmp(line_at(r, 0), "Router> show version"),
          "line is \"%s\"", line_at(r, 0));
    CHECK(r->cursor == 1, "cursor %llu, want 1", (unsigned long long)r->cursor);
    mcp_read_result_free(r);

    /* And now a read from the cursor is empty, as expected. */
    r = mcp_buffer_read(b, 1, 0);
    CHECK(r->nlines == 0, "nlines %lu, want 0", (unsigned long)r->nlines);
    mcp_read_result_free(r);

    mcp_buffer_free(b);
}

/*
 * A growing line that does not fit is left out, not cut short: a fragment
 * would invite the caller to believe the prompt is "Rout".
 */
static void test_partial_not_truncated(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;

    feed(b, "Router#> show running-config");

    r = mcp_buffer_read(b, 0, 8);
    CHECK(r->nlines == 0, "a partial line was cut short to %lu bytes",
          (unsigned long)(r->nlines ? r->lines[0].len : 0));
    CHECK(r->cursor == 0, "cursor %llu, want 0",
          (unsigned long long)r->cursor);
    mcp_read_result_free(r);

    /* With room for it, it comes back in full. */
    r = mcp_buffer_read(b, 0, 0);
    CHECK(r->nlines == 1 &&
          !strcmp(line_at(r, 0), "Router#> show running-config"),
          "the partial line is missing when it fits: nlines %lu",
          (unsigned long)r->nlines);
    mcp_read_result_free(r);

    mcp_buffer_free(b);
}

static void test_escape_stripping(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;
    char *joined;

    feed(b, "\033[31mred\033[0m\n");              /* SGR colour */
    feed(b, "\033]0;window title\007text\n");     /* OSC terminated by BEL */
    feed(b, "\033]0;window title\033\\text2\n");  /* OSC terminated by ST */
    feed(b, "\033(Bplain\n");                     /* charset selection */
    feed(b, "\033#8linedraw\n");                  /* DEC screen alignment */
    feed(b, "\033[1;2Hmoved\n");                  /* cursor addressing */
    feed(b, "\0337\0338saved\n");                 /* save/restore cursor */
    feed(b, "\033Pq#0;dcs payload\033\\after\n"); /* DCS until ST */

    r = mcp_buffer_read(b, 0, 0);
    joined = join_lines(r);
    CHECK(!strcmp(joined, "red|text|text2|plain|linedraw|moved|saved|after"),
          "window is \"%s\"", joined);
    sfree(joined);
    mcp_read_result_free(r);

    mcp_buffer_free(b);
}

/* An escape sequence can be split across two reads from the socket. */
static void test_escape_split(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;
    char *joined;

    feed_raw(b, "\033", 1);
    feed_raw(b, "[3", 2);
    feed_raw(b, "1mred", 5);
    feed_raw(b, "\033[0", 3);
    feed_raw(b, "m\n", 2);

    r = mcp_buffer_read(b, 0, 0);
    joined = join_lines(r);
    CHECK(!strcmp(joined, "red"), "window is \"%s\", want \"red\"", joined);
    sfree(joined);
    mcp_read_result_free(r);

    /* A split CR LF is still one line ending, not a redraw. */
    feed_raw(b, "second\r", 7);
    feed_raw(b, "\n", 1);
    r = mcp_buffer_read(b, 1, 0);
    joined = join_lines(r);
    CHECK(!strcmp(joined, "second"), "window is \"%s\", want \"second\"",
          joined);
    sfree(joined);
    mcp_read_result_free(r);

    mcp_buffer_free(b);
}

/*
 * A bare CR goes back to column 0: a progress bar redrawing one line must
 * stay one line, not become one line per redraw.
 */
static void test_cr_overwrite(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;
    char *joined;

    feed(b, "progress  10%\rprogress  50%\rprogress 100%\n");
    CHECK(mcp_buffer_total_lines(b) == 1, "total_lines %llu, want 1",
          (unsigned long long)mcp_buffer_total_lines(b));

    r = mcp_buffer_read(b, 0, 0);
    joined = join_lines(r);
    CHECK(!strcmp(joined, "progress 100%"), "window is \"%s\"", joined);
    sfree(joined);
    mcp_read_result_free(r);

    mcp_buffer_free(b);
}

static void test_backspace(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;
    char *joined;

    feed_raw(b, "abc\bd\n", 6);           /* ^H erases */
    feed_raw(b, "xyz\177q\n", 6);         /* DEL erases too */
    feed_raw(b, "a\b\b\b\bb\n", 7);       /* more erases than characters */

    r = mcp_buffer_read(b, 0, 0);
    joined = join_lines(r);
    CHECK(!strcmp(joined, "abd|xyq|b"), "window is \"%s\"", joined);
    sfree(joined);
    mcp_read_result_free(r);

    mcp_buffer_free(b);
}

/* One very long line must not fill the whole budget. */
static void test_long_line_chunked(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;
    char *big = snewn(5001, char);

    memset(big, 'a', 5000);
    big[5000] = '\0';
    feed(b, big);
    feed(b, "\n");

    CHECK(mcp_buffer_total_lines(b) == 2, "total_lines %llu, want 2",
          (unsigned long long)mcp_buffer_total_lines(b));
    r = mcp_buffer_read(b, 0, 0);
    CHECK(r->nlines == 2, "nlines %lu, want 2", (unsigned long)r->nlines);
    CHECK(r->lines[0].len == 4096, "first chunk %lu bytes, want 4096",
          (unsigned long)r->lines[0].len);
    CHECK(r->lines[1].len == 904, "second chunk %lu bytes, want 904",
          (unsigned long)r->lines[1].len);
    mcp_read_result_free(r);

    sfree(big);
    mcp_buffer_free(b);
}

/*
 * Eviction drops the oldest lines but renumbers nothing, and a cursor that
 * has fallen out of the window is told exactly what it missed.
 */
static void test_eviction_and_gap(void)
{
    McpBuffer *b = mcp_buffer_new(8);   /* two four-byte lines at most */
    McpReadResult *r;

    feed(b, "aaaa\nbbbb\ncccc\n");

    CHECK(mcp_buffer_total_lines(b) == 3, "total_lines %llu, want 3",
          (unsigned long long)mcp_buffer_total_lines(b));
    CHECK(mcp_buffer_count(b) == 2, "count %lu, want 2",
          (unsigned long)mcp_buffer_count(b));
    CHECK(mcp_buffer_first_id(b) == 2, "first_id %llu, want 2 (never renumbered)",
          (unsigned long long)mcp_buffer_first_id(b));
    CHECK(mcp_buffer_last_id(b) == 3, "last_id %llu, want 3",
          (unsigned long long)mcp_buffer_last_id(b));
    CHECK(mcp_buffer_bytes(b) <= 8, "bytes %lu, want <= 8",
          (unsigned long)mcp_buffer_bytes(b));

    /* A reader that has seen nothing gets an explicit gap. */
    r = mcp_buffer_read(b, 0, 0);
    CHECK(r->nlines == 2, "nlines %lu, want 2", (unsigned long)r->nlines);
    CHECK(r->lines[0].id == 2 && r->lines[1].id == 3,
          "ids %llu,%llu, want 2,3",
          (unsigned long long)r->lines[0].id,
          (unsigned long long)r->lines[1].id);
    CHECK(r->missing_from == 1 && r->missing_to == 1,
          "gap %llu..%llu, want 1..1",
          (unsigned long long)r->missing_from,
          (unsigned long long)r->missing_to);
    CHECK(r->cursor == 3, "cursor %llu, want 3",
          (unsigned long long)r->cursor);
    mcp_read_result_free(r);

    /* A reader that has seen line 1 has missed nothing. */
    r = mcp_buffer_read(b, 1, 0);
    CHECK(r->missing_to == 0, "unexpected gap %llu..%llu",
          (unsigned long long)r->missing_from,
          (unsigned long long)r->missing_to);
    CHECK(r->nlines == 2, "nlines %lu, want 2", (unsigned long)r->nlines);
    mcp_read_result_free(r);

    /* A reader that has seen line 2 gets only line 3. */
    r = mcp_buffer_read(b, 2, 0);
    CHECK(r->nlines == 1 && r->lines[0].id == 3,
          "nlines %lu, want 1 line with id 3", (unsigned long)r->nlines);
    CHECK(r->omitted == 0, "omitted %llu, want 0",
          (unsigned long long)r->omitted);
    mcp_read_result_free(r);

    mcp_buffer_free(b);
}

/* Paging: a read returns as many lines as fit, and says how many are left. */
static void test_paging(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;
    uint64_t cursor = 0;
    int got = 0;

    feed(b, "1111\n2222\n3333\n4444\n5555\n");

    r = mcp_buffer_read(b, cursor, 8);
    CHECK(r->nlines == 2, "nlines %lu, want 2", (unsigned long)r->nlines);
    CHECK(r->cursor == 2, "cursor %llu, want 2",
          (unsigned long long)r->cursor);
    CHECK(r->omitted == 3, "omitted %llu, want 3",
          (unsigned long long)r->omitted);
    cursor = r->cursor;
    got += r->nlines;
    mcp_read_result_free(r);

    r = mcp_buffer_read(b, cursor, 8);
    CHECK(r->nlines == 2, "nlines %lu, want 2", (unsigned long)r->nlines);
    CHECK(r->cursor == 4, "cursor %llu, want 4",
          (unsigned long long)r->cursor);
    CHECK(r->omitted == 1, "omitted %llu, want 1",
          (unsigned long long)r->omitted);
    cursor = r->cursor;
    got += r->nlines;
    mcp_read_result_free(r);

    r = mcp_buffer_read(b, cursor, 8);
    CHECK(r->nlines == 1, "nlines %lu, want 1", (unsigned long)r->nlines);
    CHECK(r->cursor == 5, "cursor %llu, want 5",
          (unsigned long long)r->cursor);
    CHECK(r->omitted == 0, "omitted %llu, want 0",
          (unsigned long long)r->omitted);
    got += r->nlines;
    mcp_read_result_free(r);

    CHECK(got == 5, "paging returned %d lines in total, want 5", got);

    mcp_buffer_free(b);
}

/* A single line bigger than the whole read budget is truncated, not dropped. */
static void test_truncation(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;
    char *big = snewn(101, char);

    memset(big, 'x', 100);
    big[100] = '\0';
    feed(b, big);
    feed(b, "\n");

    r = mcp_buffer_read(b, 0, 40);
    CHECK(r->nlines == 1, "nlines %lu, want 1", (unsigned long)r->nlines);
    CHECK(r->lines[0].len == 40, "truncated to %lu bytes, want 40",
          (unsigned long)r->lines[0].len);
    CHECK(strstr(line_at(r, 0), "[line truncated]") != NULL,
          "truncation marker missing from \"%s\"", line_at(r, 0));
    CHECK(r->cursor == 1, "cursor %llu, want 1",
          (unsigned long long)r->cursor);
    mcp_read_result_free(r);

    sfree(big);
    mcp_buffer_free(b);
}

/* The window is bounded by the budget, whatever is thrown at it. */
static void test_budget_bounded(void)
{
    McpBuffer *b = mcp_buffer_new(100);

    for (int i = 0; i < 1000; i++)
        feed(b, "0123456789\n");

    CHECK(mcp_buffer_bytes(b) <= 100 + 11, "bytes %lu, want <= 111",
          (unsigned long)mcp_buffer_bytes(b));
    CHECK(mcp_buffer_total_lines(b) == 1000, "total_lines %llu, want 1000",
          (unsigned long long)mcp_buffer_total_lines(b));
    CHECK(mcp_buffer_first_id(b) > 1, "nothing was evicted (first_id %llu)",
          (unsigned long long)mcp_buffer_first_id(b));
    CHECK(mcp_buffer_last_id(b) == 1000, "last_id %llu, want 1000",
          (unsigned long long)mcp_buffer_last_id(b));

    mcp_buffer_free(b);
}

/*
 * What we send gets a line of its own, so it can be told apart from device
 * output - including when the far end is sitting at a prompt with no newline
 * after it, which is the normal case.
 */
static void test_sent_lines(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;
    char *joined;

    /* The device prints a prompt and stops. */
    feed(b, "Router>");

    /* We type a command and press Return. */
    mcp_buffer_note_sent(b, MCP_AUTHOR_MCP, "show version", 12);
    mcp_buffer_note_sent(b, MCP_AUTHOR_MCP, "\r", 1);

    r = mcp_buffer_read(b, 0, 0);
    joined = join_lines(r);
    CHECK(!strcmp(joined, "Router>|show version"),
          "a prompt is closed and our command gets its own line: \"%s\"",
          joined);
    CHECK(r->nlines == 2 && r->lines[0].author == MCP_AUTHOR_DEVICE &&
          r->lines[1].author == MCP_AUTHOR_MCP,
          "the prompt stays the device's and the command is ours");
    CHECK(r->cursor == 2, "cursor %llu, want 2",
          (unsigned long long)r->cursor);
    mcp_read_result_free(r);

    /*
     * The far end echoes what we sent. The echo must not be swallowed by a
     * pending CR left over from our Return - that is what would happen if
     * note_sent() left the CR for the next byte to interpret, and it discards
     * the line rather than ending it.
     */
    feed(b, "show version\r\n");
    r = mcp_buffer_read(b, 2, 0);
    joined = join_lines(r);
    CHECK(!strcmp(joined, "show version"), "the echo survives: \"%s\"", joined);
    CHECK(r->nlines == 1 && r->lines[0].author == MCP_AUTHOR_DEVICE,
          "the echo is attributed to the device");
    mcp_read_result_free(r);

    /* Our text is not renumbered or merged into the next device line. */
    feed(b, "IOS version 15.2\r\n");
    r = mcp_buffer_read(b, 3, 0);
    joined = join_lines(r);
    CHECK(!strcmp(joined, "IOS version 15.2"), "next output: \"%s\"", joined);
    mcp_read_result_free(r);

    /* An Enter with nothing typed makes no line of its own. */
    size_t before = mcp_buffer_total_lines(b);
    mcp_buffer_note_sent(b, MCP_AUTHOR_MCP, "\r", 1);
    CHECK(mcp_buffer_total_lines(b) == before,
          "an empty Return adds no line (was %lu, now %llu)",
          (unsigned long)before,
          (unsigned long long)mcp_buffer_total_lines(b));

    /*
     * A client's text that contains a Return keeps the client's tag on both
     * sides of it. The attribution that returns to the device after a newline
     * is about the *human's* keystrokes, which arrive one at a time and are
     * finished by the device's echo - not about text a client sent whole.
     */
    mcp_buffer_note_sent(b, MCP_AUTHOR_MCP, "first\rsecond", 12);
    r = mcp_buffer_read(b, 0, 0);
    CHECK(r->nlines >= 2 &&
          r->lines[r->nlines - 2].author == MCP_AUTHOR_MCP &&
          r->lines[r->nlines - 1].author == MCP_AUTHOR_MCP,
          "both halves of a client's multi-line text are the client's");
    mcp_read_result_free(r);

    mcp_buffer_free(b);
}

/* Each line carries who wrote it. */
static void test_authors(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;

    mcp_buffer_note_sent(b, MCP_AUTHOR_HUMAN, "ls\n", 3);
    feed(b, "file1  file2\n");
    mcp_buffer_note_sent(b, MCP_AUTHOR_MCP, "exit\n", 5);

    CHECK(mcp_author_tag(MCP_AUTHOR_DEVICE) == 'd', "device tag is not 'd'");
    CHECK(mcp_author_tag(MCP_AUTHOR_HUMAN) == 'h', "human tag is not 'h'");
    CHECK(mcp_author_tag(MCP_AUTHOR_MCP) == 'm', "mcp tag is not 'm'");

    r = mcp_buffer_read(b, 0, 0);
    CHECK(r->nlines == 3, "nlines %lu, want 3", (unsigned long)r->nlines);
    CHECK(r->lines[0].author == MCP_AUTHOR_HUMAN, "line 1 author %d, want human",
          (int)r->lines[0].author);
    CHECK(r->lines[1].author == MCP_AUTHOR_DEVICE,
          "line 2 author %d, want device", (int)r->lines[1].author);
    CHECK(r->lines[2].author == MCP_AUTHOR_MCP, "line 3 author %d, want mcp",
          (int)r->lines[2].author);
    mcp_read_result_free(r);

    mcp_buffer_free(b);
}

static void test_empty(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;

    CHECK(mcp_buffer_first_id(b) == 0, "first_id %llu, want 0",
          (unsigned long long)mcp_buffer_first_id(b));
    CHECK(mcp_buffer_last_id(b) == 0, "last_id %llu, want 0",
          (unsigned long long)mcp_buffer_last_id(b));
    CHECK(mcp_buffer_count(b) == 0, "count %lu, want 0",
          (unsigned long)mcp_buffer_count(b));

    r = mcp_buffer_read(b, 0, 0);
    CHECK(r->nlines == 0, "nlines %lu, want 0", (unsigned long)r->nlines);
    CHECK(r->cursor == 0, "cursor %llu, want 0",
          (unsigned long long)r->cursor);
    CHECK(r->total_lines == 0, "total_lines %llu, want 0",
          (unsigned long long)r->total_lines);
    CHECK(r->missing_to == 0, "unexpected gap");
    mcp_read_result_free(r);

    /* A read past the end of an empty session is still clean. */
    r = mcp_buffer_read(b, 99, 0);
    CHECK(r->nlines == 0, "nlines %lu, want 0", (unsigned long)r->nlines);
    CHECK(r->missing_to == 0, "unexpected gap %llu..%llu",
          (unsigned long long)r->missing_from,
          (unsigned long long)r->missing_to);
    mcp_read_result_free(r);

    mcp_buffer_free(b);
}

/*
 * What the human types, keystroke by keystroke, and the device's echo of it.
 *
 * This is the case note_sent() cannot handle: human input arrives one character
 * at a time, and the device echoes each character straight back. Writing the
 * keystrokes *and* keeping the echo would record "ls" as "llss"; writing them
 * and dropping the echo would put a password in the transcript. So the
 * keystrokes are not written at all, and the echo is the record.
 */
static void test_typed_lines(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;
    char *joined;

    /* The device is sitting at a prompt. */
    feed(b, "Router>");

    /*
     * The human types "ls", one keystroke at a time, and the device echoes
     * each character as it arrives. Every keystroke lands on one line, once.
     */
    const char *typed = "ls";
    for (const char *p = typed; *p; p++) {
        mcp_buffer_note_typed(b, p, 1);
        feed_raw(b, p, 1);                   /* the echo, straight back */
    }

    r = mcp_buffer_read(b, 0, 0);
    joined = join_lines(r);
    CHECK(!strcmp(joined, "Router>|ls"),
          "typed characters are one line, not one per keystroke, and not "
          "doubled by the echo: \"%s\"", joined);
    CHECK(r->nlines == 2 && r->lines[1].author == MCP_AUTHOR_HUMAN,
          "and the line is attributed to the human");
    mcp_read_result_free(r);

    /* Return: the echo comes back as CR LF, and neither half adds a line. */
    mcp_buffer_note_typed(b, "\r", 1);
    feed(b, "\r\n");

    r = mcp_buffer_read(b, 0, 0);
    joined = join_lines(r);
    CHECK(!strcmp(joined, "Router>|ls"),
          "a typed Return does not add an empty line: \"%s\"", joined);
    CHECK(mcp_buffer_total_lines(b) == 2, "two lines in total, got %llu",
          (unsigned long long)mcp_buffer_total_lines(b));
    mcp_read_result_free(r);

    /* The command's output is ordinary device output again. */
    feed(b, "file-a  file-b\r\nRouter>");
    r = mcp_buffer_read(b, 2, 0);
    joined = join_lines(r);
    CHECK(!strcmp(joined, "file-a  file-b|Router>"),
          "output after the human's line is the device's again: \"%s\"", joined);
    CHECK(r->lines[1].author == MCP_AUTHOR_DEVICE,
          "and is attributed to the device");
    mcp_read_result_free(r);

    mcp_buffer_free(b);
}

/*
 * A device that does not echo. This is the password case, and the important
 * one: what the human typed must not end up in the transcript, because a
 * password is the one thing an agent must never be handed.
 */
static void test_typed_not_echoed(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;
    char *joined;

    feed(b, "Password: ");

    /* The human types a password. The device echoes none of it. */
    const char *secret = "hunter2";
    for (const char *p = secret; *p; p++)
        mcp_buffer_note_typed(b, p, 1);

    /* The device does echo the Return. */
    feed(b, "\r\n");

    r = mcp_buffer_read(b, 0, 0);
    joined = join_lines(r);
    CHECK(strstr(joined, secret) == NULL,
          "a password the human types is never in the transcript: \"%s\"",
          joined);
    CHECK(strstr(joined, "Password: ") != NULL,
          "the prompt itself is kept: \"%s\"", joined);
    CHECK(r->nlines == 2 && r->lines[0].author == MCP_AUTHOR_DEVICE &&
          r->lines[1].author == MCP_AUTHOR_HUMAN && r->lines[1].len == 0,
          "and the human's Return is an empty line of their own");
    mcp_read_result_free(r);

    /* The next device output is the device's again, not the human's. */
    feed(b, "Welcome\r\n");
    r = mcp_buffer_read(b, 2, 0);
    joined = join_lines(r);
    CHECK(!strcmp(joined, "Welcome"), "then output resumes: \"%s\"", joined);
    CHECK(r->lines[0].author == MCP_AUTHOR_DEVICE, "attributed to the device");
    mcp_read_result_free(r);

    mcp_buffer_free(b);
}

/*
 * A human typing while the device is mid-line, and a human's line next to a
 * device line: the authors must stay apart, which is what makes "who changed
 * this?" answerable.
 */
static void test_typed_between_device_lines(void)
{
    McpBuffer *b = mcp_buffer_new(0);
    McpReadResult *r;
    char *joined;

    feed(b, "Router>");
    mcp_buffer_note_typed(b, "q", 1);
    feed(b, "q");                     /* echo */
    mcp_buffer_note_typed(b, "\r", 1);
    feed(b, "\r\n");
    feed(b, "Router>");

    r = mcp_buffer_read(b, 0, 0);
    joined = join_lines(r);
    CHECK(!strcmp(joined, "Router>|q|Router>"),
          "device, human, device: \"%s\"", joined);
    CHECK(r->nlines == 3 &&
          r->lines[0].author == MCP_AUTHOR_DEVICE &&
          r->lines[1].author == MCP_AUTHOR_HUMAN &&
          r->lines[2].author == MCP_AUTHOR_DEVICE,
          "with the middle one attributed to the human");
    mcp_read_result_free(r);

    /* The tags a client sees. */
    CHECK(mcp_author_tag(MCP_AUTHOR_HUMAN) == 'h' &&
          mcp_author_tag(MCP_AUTHOR_DEVICE) == 'd' &&
          mcp_author_tag(MCP_AUTHOR_MCP) == 'm',
          "the one-letter tags are h, d and m");

    mcp_buffer_free(b);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("MCP capture buffer tests\n\n");

    printf("  [test] empty\n");             test_empty();
    printf("  [test] plain_lines\n");       test_plain_lines();
    printf("  [test] blank_lines\n");       test_blank_lines();
    printf("  [test] partial_line\n");      test_partial_line();
    printf("  [test] partial_not_truncated\n"); test_partial_not_truncated();
    printf("  [test] escape_stripping\n");  test_escape_stripping();
    printf("  [test] escape_split\n");      test_escape_split();
    printf("  [test] cr_overwrite\n");      test_cr_overwrite();
    printf("  [test] backspace\n");         test_backspace();
    printf("  [test] long_line_chunked\n"); test_long_line_chunked();
    printf("  [test] eviction_and_gap\n");  test_eviction_and_gap();
    printf("  [test] paging\n");            test_paging();
    printf("  [test] truncation\n");        test_truncation();
    printf("  [test] budget_bounded\n");    test_budget_bounded();
    printf("  [test] sent_lines\n");        test_sent_lines();
    printf("  [test] typed_lines\n");       test_typed_lines();
    printf("  [test] typed_not_echoed\n");  test_typed_not_echoed();
    printf("  [test] typed_between\n");     test_typed_between_device_lines();
    printf("  [test] authors\n");           test_authors();

    printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
