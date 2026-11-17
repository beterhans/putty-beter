/*
 * PuTTY-beter: the MCP capture buffer.
 *
 * The MCP server (unix/mcp.c) keeps its own record of everything a session has
 * printed. It deliberately does not use the terminal's scrollback: scrollback
 * is a *display* setting, so a user can turn it down to nothing, while MCP has
 * to see every byte regardless of how far the screen can scroll.
 *
 * This file is free of GTK, of the terminal and of the socket, so it can be
 * unit-tested on its own (test/test_mcp_buffer.c). It takes bytes, strips the
 * escape sequences a network CLI never means as content, splits them into
 * lines, and keeps a sliding window of the most recent ones under a byte
 * budget.
 *
 * Line ids are absolute and never reused: line 1 is the first line of the
 * session, forever. Eviction drops the oldest lines but renumbers nothing, so a
 * reader whose cursor has fallen out of the window is told exactly what it
 * missed rather than silently losing output.
 *
 * See PLAN.md section 7.4 for the reasoning.
 */

#ifndef PUTTY_MCP_BUFFER_H
#define PUTTY_MCP_BUFFER_H

#include "defs.h"

typedef struct McpBuffer McpBuffer;

/* Who wrote a line. Reported to the client as a one-letter tag. */
typedef enum McpAuthor {
    MCP_AUTHOR_DEVICE = 0,     /* the far end */
    MCP_AUTHOR_HUMAN  = 1,     /* typed in the GUI */
    MCP_AUTHOR_MCP    = 2,     /* sent by an MCP client */
} McpAuthor;

/* The one-letter tag read() reports for each author: "d", "h", "m". */
char mcp_author_tag(McpAuthor author);

/* Default window for one session, ~4 MB (roughly 50k lines of CLI output). */
#define MCP_BUFFER_DEFAULT_BUDGET (4 * 1024 * 1024)

/*
 * A line is cut into chunks of at most this many bytes, so that one
 * unterminated line cannot blow the whole budget.
 */
#define MCP_BUFFER_CHUNK 4096

/* Default read budget for one call, ~8 KB (about 100 lines of CLI output). */
#define MCP_READ_DEFAULT_BUDGET 8192

McpBuffer *mcp_buffer_new(size_t budget);
void mcp_buffer_free(McpBuffer *b);

/* Bytes that arrived from the far end. */
void mcp_buffer_feed(McpBuffer *b, const void *data, size_t len);

/*
 * A line we sent, so that read() can say who typed it. Author tagging is
 * wired into the front end in phase 4b; the buffer supports it now because
 * the field has to exist for the ids to be meaningful.
 */
void mcp_buffer_note_sent(McpBuffer *b, McpAuthor author,
                          const void *data, size_t len);

/* Id of the oldest line still in the window, or 0 when it is empty. */
uint64_t mcp_buffer_first_id(McpBuffer *b);
/* Id of the newest *completed* line, or 0 when there is none. */
uint64_t mcp_buffer_last_id(McpBuffer *b);
/* Lines completed so far. Monotonic. */
uint64_t mcp_buffer_total_lines(McpBuffer *b);
/* Completed lines currently held. */
size_t mcp_buffer_count(McpBuffer *b);
/* Bytes of line text currently held. */
size_t mcp_buffer_bytes(McpBuffer *b);

typedef struct McpReadLine {
    uint64_t id;
    McpAuthor author;
    char *text;                /* owned by the result, NUL-terminated */
    size_t len;
} McpReadLine;

typedef struct McpReadResult {
    /*
     * Where the next read should start. This is the id of the last completed
     * line returned, or `since' if none was.
     */
    uint64_t cursor;
    /* Lines completed so far; monotonic, so a reader can see how far behind. */
    uint64_t total_lines;
    /* Lines after `cursor' that are still in the window but were not returned. */
    uint64_t omitted;
    /*
     * If missing_to is non-zero, lines missing_from .. missing_to were evicted
     * before this read and are gone for good.
     */
    uint64_t missing_from, missing_to;
    size_t nlines;
    McpReadLine *lines;
} McpReadResult;

/*
 * Everything after `since', up to max_bytes of line text.
 *
 * The line currently being written is appended even when its id is not greater
 * than `since': it can still grow, so a reader that waited for the end of the
 * previous read has to be shown it again. Its id is total_lines + 1, and it
 * does not move the cursor - once it is terminated by a newline it becomes a
 * normal line and the cursor advances to it.
 *
 * That line is never truncated: if it does not fit in max_bytes it is simply
 * left out, which loses nothing because it does not move the cursor. A
 * fragment of a growing line would be worse than nothing, since it invites the
 * caller to believe the prompt is shorter than it is. A *completed* line is
 * truncated when it does not fit, because the cursor has to be able to move
 * past it.
 */
McpReadResult *mcp_buffer_read(McpBuffer *b, uint64_t since, size_t max_bytes);
void mcp_read_result_free(McpReadResult *r);

#endif /* PUTTY_MCP_BUFFER_H */
