/*
 * PuTTY-beter: the MCP server.
 *
 * An AI agent drives a session *in the window the human is looking at*, so the
 * human can watch, take over, and answer the prompts the agent cannot (a
 * password, a confirmation). This file is the front end's whole interface to
 * that: one session object per terminal window, one tap on the output stream,
 * and a process-wide socket server that the `putty-mcp` bridge talks to.
 *
 * There is deliberately no JSON and no MCP here. The socket protocol is a
 * sequence of length-prefixed fields (see the comment at the top of mcp.c),
 * and the bridge in mcp/ does all of the MCP and JSON translation. That keeps
 * MCP's spec churn out of the fork, and makes the socket testable with
 * printf(1) and nc(1).
 *
 * See PLAN.md section 7 for the design and the reasoning.
 */

#ifndef PUTTY_MCP_H
#define PUTTY_MCP_H

#include "defs.h"
#include "mcp-buffer.h"

typedef struct McpSession McpSession;

/*
 * What the server asks a session for when a client calls screen(): the visible
 * screen and the terminal size. The front end fills this in, because only it
 * has the Terminal; the server never touches it. `screen' is a malloc'd UTF-8
 * string and the server takes ownership of it.
 */
typedef struct McpView {
    char *screen;
    int cols, rows;
} McpView;

typedef void (*mcp_view_fn)(void *ctx, McpView *out);

/*
 * Put bytes into the session as if they had been typed. Returns false if the
 * session cannot take them right now - during a ZMODEM transfer the stream is
 * protocol and the terminal is not seeing it, so typed bytes would corrupt the
 * transfer - and the server turns that into an error for the client.
 */
typedef bool (*mcp_send_fn)(void *ctx, const void *data, size_t len);

/*
 * Register a session with the server. The server is started on the first call
 * and stopped when the last session goes, so a PuTTY with no MCP-enabled
 * session never creates a socket at all.
 *
 * `writable` is whether this session may be typed into at all: it is a
 * separate setting from "offer this session", because letting a session be read
 * is not the same as letting it be driven.
 */
McpSession *mcp_session_new(const char *name, size_t budget, bool writable,
                            mcp_view_fn get_view, mcp_send_fn send, void *ctx);
void mcp_session_free(McpSession *s);

/* The window title changed; keep the name a client sees roughly current. */
void mcp_session_set_name(McpSession *s, const char *name);

/* Tap on the stream from the far end. */
void mcp_session_feed(McpSession *s, const void *data, size_t len);

/* A line we sent, so that read() can say who typed it. Phase 4b. */
void mcp_session_note_sent(McpSession *s, McpAuthor author,
                           const void *data, size_t len);

/* For diagnostics and for the front end's own debug output. */
bool mcp_server_running(void);
const char *mcp_server_socket_path(void);

#endif /* PUTTY_MCP_H */
