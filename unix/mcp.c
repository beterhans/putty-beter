/*
 * PuTTY-beter: the MCP server.
 *
 * See mcp.h for what this is for and PLAN.md section 7 for the design. This
 * file is the socket server, the session registry and the wire protocol; the
 * capture buffer is unix/mcp-buffer.c, which knows nothing about sockets.
 *
 * WHY THE PROTOCOL IS NOT JSON
 *
 * PuTTY is C and has no JSON parser, and should not grow one. So the socket
 * speaks a deliberately dumb, stable format and the `putty-mcp` bridge in mcp/
 * does all of the MCP and JSON translation. That keeps MCP's spec churn out of
 * the fork, makes the socket testable with printf(1) and nc(1), and lets any
 * other client drive PuTTY without MCP at all.
 *
 * THE WIRE FORMAT
 *
 * Both directions are the same, and everything is big-endian:
 *
 *     message := u32 nfields
 *                nfields * field
 *     field   := u32 length, then `length' bytes
 *
 * A field is a byte string: no escaping, no quoting, no NULs needed. Integers
 * travel as decimal text, so the C side never has to agree with the bridge on
 * an integer width. At most MCP_MAX_FIELDS fields, each at most MCP_MAX_FIELD
 * bytes, and at most MCP_MAX_MESSAGE in total.
 *
 * Requests (field 0 is the command):
 *
 *     "hello"    <protocol-version> <token>      must be the first message
 *     "sessions"                                 -> "ok" <count>
 *                                                     then per session:
 *                                                     <id> <name> <ro|rw>
 *                                                     <prompt>
 *     "read"     <id> <since> <max-bytes>        -> "ok" <cursor> <total-lines>
 *                                                     <missing-from> <missing-to>
 *                                                     <omitted> <nlines>
 *                                                     then per line:
 *                                                     <id> <author> <text>
 *     "screen"   <id>                            -> "ok" <text> <cols> <rows>
 *     "send"     <id> <text>                     -> "ok" <bytes-sent>
 *     "key"      <id> <name>                     -> "ok"
 *     "version"                                  -> "ok" <version> <protocol>
 *     "ping"                                     -> "ok"
 *
 * `send` and `key` are refused unless the session allows typing, and while a
 * ZMODEM transfer is running.
 *
 * An empty <since> means "from the start of the buffer", and an empty
 * <max-bytes> means the server's default. An empty <missing-from> and
 * <missing-to> mean nothing was lost. <author> is "d" for the device, "h" for
 * the human at the keyboard and "m" for an MCP client.
 *
 * Failures are "error" <message>. A failed handshake, or a malformed message,
 * drops the connection: there is nothing useful to say to a peer that is not
 * speaking the protocol.
 *
 * WHERE THE SOCKET LIVES
 *
 * The directory is $TMPDIR/putty-mcp-<uid>, which is local (a Unix socket
 * cannot live on a synced or network filesystem, and ~/Documents may be iCloud
 * Drive). Inside it are `sock.<pid>', `token.<pid>' and `endpoint.<pid>' - one
 * set per process, because more than one PuTTY can be offering sessions at
 * once and a single fixed name would let the second one steal the first one's
 * socket. The endpoint file - socket path, token path, pid, protocol version -
 * is also written into the configuration folder as `mcp-endpoint.<pid>', so the
 * bridge can find a server whose $TMPDIR differs from its own.
 *
 * Session ids carry the pid for the same reason: the bridge merges the lists
 * from every server it finds, so an id has to say which one it belongs to.
 *
 * PERMISSIONS
 *
 * The socket is 0660 and owned by the `putty-mcp' group, so that only the users
 * in that group can drive it. macOS defaults files to group `staff', which
 * contains every local account, so 0660 alone would let any local user type
 * into your switches. If the `putty-mcp' group does not exist the socket falls
 * back to 0600 - owner only - rather than to a world-usable mode.
 *
 * DEBUG
 *
 * Set PUTTY_MCP_DEBUG=1 for a trace on stderr of the socket, the handshake,
 * every request and every response.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "putty.h"
#include "misc.h"
#include "mcp.h"

/*
 * Bumped to 2 when `sessions` gained the read-only/read-write field, and `send`
 * and `key` were added. Bumped to 3 by phase 4c, when `sessions` gained the
 * human-prompt field and `read` gained one beside its line count, so a client
 * can tell "the device is waiting for me" from "the window is waiting for the
 * person at the keyboard". Both ends are shipped together, and the handshake
 * checks this, so a mismatched pair says so instead of misreading fields.
 */
#define MCP_PROTO_VERSION 3
#define MCP_GROUP "putty-mcp"

#define MCP_MAX_FIELDS 64
#define MCP_MAX_FIELD (1024 * 1024)
#define MCP_MAX_MESSAGE (4 * 1024 * 1024)
#define MCP_MAX_OUTPUT (4 * 1024 * 1024)
#define MCP_READ_CHUNK 8192
#define MCP_LISTEN_BACKLOG 8

static int mcp_debug_flag = -1;
static bool mcp_debug(void)
{
    if (mcp_debug_flag < 0)
        mcp_debug_flag = (getenv("PUTTY_MCP_DEBUG") != NULL);
    return mcp_debug_flag;
}
#define MCPDBG(...) do {                                        \
        if (mcp_debug()) {                                      \
            fprintf(stderr, "[mcp] " __VA_ARGS__);               \
            fputc('\n', stderr);                                \
            fflush(stderr);                                     \
        }                                                       \
    } while (0)

/* ------------------------------------------------------------------ */
/* Sessions                                                            */
/* ------------------------------------------------------------------ */

struct McpSession {
    int id;
    char *name;
    McpBuffer *buf;
    bool writable;              /* may a client type into this session? */
    mcp_view_fn get_view;
    mcp_prompt_fn get_prompt;   /* phase 4c: who is being asked, if anyone */
    mcp_send_fn send;
    void *ctx;
    struct McpSession *next;
};

static McpSession *sessions;
static int session_count;
static int next_session_id = 1;

/* The prompt the human is being asked right now, or NULL (phase 4c). */
static const char *session_prompt(McpSession *s);

/* ------------------------------------------------------------------ */
/* The server                                                          */
/* ------------------------------------------------------------------ */

typedef struct McpConn McpConn;

struct McpConn {
    int fd;
    int rwx;                    /* what uxsel is currently watching for */
    bool hello_done;
    unsigned char *in;
    size_t inlen, inalloc;
    strbuf *out;
    size_t outpos;
    struct McpConn *next;
};

static bool server_up;
static int listen_fd = -1;
static char *dir_path, *sock_path, *token_path, *endpoint_path;
static char *config_endpoint_path;
static char token_hex[65];
static McpConn *conns;

/*
 * Session ids are made unique across *processes*, not just within one, because
 * more than one PuTTY can be offering sessions at once - the plain `putty` and
 * `pterm` binaries run one process per window, and each has its own server.
 * The bridge merges their lists, so an id has to say which server it belongs
 * to; the pid is the obvious thing that is already unique among live
 * processes. The number is machine-facing - the name is what a human reads.
 */
static int id_base;

/* The id a session in this process is built from; see id_base above. */
static int session_id_base(void)
{
    if (!id_base) {
        unsigned long pid = (unsigned long)getpid() % 1000000UL;
        id_base = (int)(pid * 1000UL);
    }
    return id_base;
}

static uint32_t rd32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool write_file(const char *path, const char *data, size_t len,
                       mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);

    if (fd < 0)
        return false;
    /* The mode above is masked by the umask, so set it again explicitly. */
    if (fchmod(fd, mode) != 0) { /* not fatal */ }
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return false;
        }
        data += n;
        len -= (size_t)n;
    }
    return close(fd) == 0;
}

static char *runtime_dir(void)
{
    const char *tmp = getenv("TMPDIR");
    char *trimmed, *dir;
    size_t n;

    if (!tmp || !*tmp)
        tmp = "/tmp";

    trimmed = dupstr(tmp);
    n = strlen(trimmed);
    while (n > 1 && trimmed[n-1] == '/')
        trimmed[--n] = '\0';

    dir = dupprintf("%s/putty-mcp-%ld", trimmed, (long)getuid());
    sfree(trimmed);
    return dir;
}

/*
 * The token every client must present in its handshake. It is defence in
 * depth: the socket is already owner-only (or group-only), so the token is
 * there for the case where a process gets hold of the socket anyway.
 *
 * This reads /dev/urandom rather than using PuTTY's own random_read(), because
 * that lives in the crypto stack, which pterm does not link. If the token
 * cannot be made, the server refuses to start rather than fall back to
 * something guessable.
 */
static bool make_token(char *hex)
{
    unsigned char raw[32];
    size_t got = 0;
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd < 0) {
        MCPDBG("open /dev/urandom: %s", strerror(errno));
        return false;
    }
    while (got < sizeof(raw)) {
        ssize_t n = read(fd, raw + got, sizeof(raw) - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            MCPDBG("read /dev/urandom: %s", strerror(errno));
            close(fd);
            return false;
        }
        if (n == 0) {
            MCPDBG("/dev/urandom returned no data");
            close(fd);
            return false;
        }
        got += (size_t)n;
    }
    close(fd);

    for (size_t i = 0; i < sizeof(raw); i++)
        sprintf(hex + 2*i, "%02x", raw[i]);
    hex[64] = '\0';
    return true;
}

static void conn_close(McpConn *c);
static bool conn_flush(McpConn *c);
static void mcp_conn_event(int fd, int event);
static void mcp_listen_event(int fd, int event);

static void conn_want(McpConn *c, int rwx)
{
    if (c->rwx == rwx)
        return;
    c->rwx = rwx;
    uxsel_set(c->fd, rwx, mcp_conn_event);
}

static void server_stop(void)
{
    while (conns)
        conn_close(conns);

    if (listen_fd >= 0) {
        uxsel_del(listen_fd);
        close(listen_fd);
        listen_fd = -1;
    }

    if (sock_path)
        unlink(sock_path);
    if (token_path)
        unlink(token_path);
    if (endpoint_path)
        unlink(endpoint_path);
    if (config_endpoint_path)
        unlink(config_endpoint_path);

    sfree(dir_path);          dir_path = NULL;
    sfree(sock_path);         sock_path = NULL;
    sfree(token_path);        token_path = NULL;
    sfree(endpoint_path);     endpoint_path = NULL;
    sfree(config_endpoint_path); config_endpoint_path = NULL;

    server_up = false;
    MCPDBG("server stopped");
}

static bool server_start(void)
{
    struct sockaddr_un addr;
    struct group *gr;
    char *body;
    int fd;
    mode_t mode = 0600;

    if (server_up)
        return true;

    dir_path = runtime_dir();
    if (mkdir(dir_path, 0700) != 0 && errno != EEXIST) {
        MCPDBG("mkdir %s: %s", dir_path, strerror(errno));
        sfree(dir_path); dir_path = NULL;
        return false;
    }
    if (chmod(dir_path, 0700) != 0)
        MCPDBG("chmod %s: %s", dir_path, strerror(errno));

    /*
     * One socket, token and endpoint file *per process*, named after the pid.
     *
     * A single fixed name would be actively harmful: the plain `putty` and
     * `pterm` binaries run one process per window, so a second PuTTY would
     * unlink the first one's socket and take the path over, leaving the first
     * silently unreachable. The bridge reads every endpoint file it finds and
     * merges the sessions, so per-process names are what make more than one
     * PuTTY usable at a time.
     *
     * A file left behind by a PuTTY that was killed is harmless: the bridge
     * skips a socket that refuses the connection.
     */
    sock_path = dupprintf("%s/sock.%ld", dir_path, (long)getpid());
    token_path = dupprintf("%s/token.%ld", dir_path, (long)getpid());
    endpoint_path = dupprintf("%s/endpoint.%ld", dir_path, (long)getpid());

    if (strlen(sock_path) >= sizeof(addr.sun_path)) {
        MCPDBG("socket path too long: %s", sock_path);
        server_stop();
        return false;
    }

    /* A socket left behind by a crash would make bind() fail. */
    unlink(sock_path);

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        MCPDBG("socket: %s", strerror(errno));
        server_stop();
        return false;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, sock_path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        MCPDBG("bind %s: %s", sock_path, strerror(errno));
        close(fd);
        server_stop();
        return false;
    }
    if (listen(fd, MCP_LISTEN_BACKLOG) < 0) {
        MCPDBG("listen: %s", strerror(errno));
        close(fd);
        server_stop();
        return false;
    }

    /*
     * Owner-only unless the dedicated group exists. 0660 in the wrong group
     * would be worse than useless on macOS, where the default group is
     * `staff' and contains every local account.
     */
    gr = getgrnam(MCP_GROUP);
    if (gr) {
        mode = 0660;
        if (chown(sock_path, (uid_t)-1, gr->gr_gid) != 0)
            MCPDBG("chown %s: %s", sock_path, strerror(errno));
    } else {
        MCPDBG("no %s group: socket is owner-only (0600)", MCP_GROUP);
    }
    if (chmod(sock_path, mode) != 0)
        MCPDBG("chmod %s: %s", sock_path, strerror(errno));

    if (!make_token(token_hex)) {
        close(fd);
        server_stop();
        return false;
    }
    if (!write_file(token_path, token_hex, 64, 0600)) {
        MCPDBG("cannot write %s: %s", token_path, strerror(errno));
        close(fd);
        server_stop();
        return false;
    }

    body = dupprintf("socket %s\ntoken %s\npid %ld\nversion %d\n",
                     sock_path, token_path, (long)getpid(), MCP_PROTO_VERSION);
    if (!write_file(endpoint_path, body, strlen(body), 0600))
        MCPDBG("cannot write %s: %s", endpoint_path, strerror(errno));

    /*
     * The configuration folder copy is what lets the bridge find a server
     * whose $TMPDIR differs from its own - a GUI app and a shell can disagree
     * about that. pterm has no configuration folder, so it is skipped.
     */
    if (saved_sessions) {
        char *dir = config_location_current();
        config_endpoint_path = dupprintf("%s/mcp-endpoint.%ld", dir,
                                         (long)getpid());
        if (write_file(config_endpoint_path, body, strlen(body), 0600))
            MCPDBG("endpoint also written to %s", config_endpoint_path);
        else
            MCPDBG("cannot write %s: %s", config_endpoint_path,
                   strerror(errno));
        sfree(dir);
    }
    sfree(body);

    nonblock(fd);
    listen_fd = fd;
    server_up = true;
    uxsel_set(listen_fd, SELECT_R, mcp_listen_event);

    MCPDBG("listening on %s (mode 0%o)", sock_path, (unsigned)mode);
    return true;
}

bool mcp_server_running(void)
{
    return server_up;
}

const char *mcp_server_socket_path(void)
{
    return sock_path;
}

/* ------------------------------------------------------------------ */
/* Request and response framing                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    size_t len;
} mcp_field;

static void resp_field(strbuf *sb, const void *data, size_t len)
{
    put_uint32(sb, (unsigned long)len);
    if (len)
        put_data(sb, data, len);
}

static void resp_str(strbuf *sb, const char *s)
{
    resp_field(sb, s, strlen(s));
}

static void resp_num(strbuf *sb, uint64_t v)
{
    char buf[32];

    sprintf(buf, "%llu", (unsigned long long)v);
    resp_str(sb, buf);
}

/*
 * The total size of the message at p, or 0 if it is not all there yet, or -1
 * if it is not a message at all.
 */
static long msg_total(const unsigned char *p, size_t len)
{
    uint32_t nfields;
    size_t pos = 4;

    if (len < 4)
        return 0;
    nfields = rd32(p);
    if (nfields > MCP_MAX_FIELDS)
        return -1;

    for (uint32_t i = 0; i < nfields; i++) {
        uint32_t flen;
        if (len < pos + 4)
            return 0;
        flen = rd32(p + pos);
        pos += 4;
        if (flen > MCP_MAX_FIELD)
            return -1;
        if (len < pos + flen)
            return 0;
        pos += flen;
    }

    return (pos > MCP_MAX_MESSAGE) ? -1 : (long)pos;
}

static bool field_is(const mcp_field *f, const char *s)
{
    size_t len = strlen(s);

    return f->len == len && !memcmp(f->p, s, len);
}

static uint64_t field_uint(const mcp_field *f)
{
    uint64_t v = 0;

    for (size_t i = 0; i < f->len; i++) {
        if (f->p[i] < '0' || f->p[i] > '9')
            break;
        v = v * 10 + (uint64_t)(f->p[i] - '0');
        if (v > 0xFFFFFFFFFFFFULL)
            return v;
    }
    return v;
}

static McpSession *session_by_id(uint64_t id)
{
    for (McpSession *s = sessions; s; s = s->next)
        if ((uint64_t)s->id == id)
            return s;
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static bool resp_error(McpConn *c, const char *message)
{
    put_uint32(c->out, 2);
    resp_str(c->out, "error");
    resp_str(c->out, message);
    MCPDBG("-> error: %s", message);
    return conn_flush(c);
}

static bool cmd_sessions(McpConn *c)
{
    uint32_t n = 0;

    for (McpSession *s = sessions; s; s = s->next)
        n++;

    put_uint32(c->out, 2 + 4 * n);
    resp_str(c->out, "ok");
    resp_num(c->out, n);
    for (McpSession *s = sessions; s; s = s->next) {
        const char *prompt = session_prompt(s);

        resp_num(c->out, (uint64_t)s->id);
        resp_str(c->out, s->name);
        /* So a client knows before it tries whether it may type. */
        resp_str(c->out, s->writable ? "rw" : "ro");
        /*
         * And whether the person at the keyboard is being asked something
         * (phase 4c). Non-empty means a prompt is on the screen *from PuTTY*
         * - a password, a passphrase, a login name - and the client must wait
         * for the human rather than answer it.
         */
        resp_str(c->out, prompt ? prompt : "");
    }

    MCPDBG("-> sessions: %u", (unsigned)n);
    return conn_flush(c);
}

static bool cmd_read(McpConn *c, const mcp_field *f, uint32_t n)
{
    McpSession *s;
    McpReadResult *r;
    uint64_t since = 0;
    size_t max_bytes = 0;

    if (n < 2)
        return resp_error(c, "read needs a session id");
    s = session_by_id(field_uint(&f[1]));
    if (!s)
        return resp_error(c, "no such session");
    if (n >= 3 && f[2].len)
        since = field_uint(&f[2]);
    if (n >= 4 && f[3].len)
        max_bytes = (size_t)field_uint(&f[3]);

    r = mcp_buffer_read(s->buf, since, max_bytes);

    put_uint32(c->out, 8 + 3 * (uint32_t)r->nlines);
    resp_str(c->out, "ok");
    resp_num(c->out, r->cursor);
    resp_num(c->out, r->total_lines);
    if (r->missing_to) {
        resp_num(c->out, r->missing_from);
        resp_num(c->out, r->missing_to);
    } else {
        resp_str(c->out, "");
        resp_str(c->out, "");
    }
    resp_num(c->out, r->omitted);
    resp_num(c->out, (uint64_t)r->nlines);
    /*
     * Phase 4c: sitting between the line count and the lines is whether a
     * human is being prompted (empty when not). `run` polls this very reply,
     * so a command that lands on a PuTTY password prompt can stop and say so
     * instead of reporting "quiet - probably finished" and sending the next
     * command into a password prompt.
     */
    {
        const char *prompt = session_prompt(s);
        resp_str(c->out, prompt ? prompt : "");
    }
    for (size_t i = 0; i < r->nlines; i++) {
        char tag[2];
        tag[0] = mcp_author_tag(r->lines[i].author);
        tag[1] = '\0';
        resp_num(c->out, r->lines[i].id);
        resp_str(c->out, tag);
        resp_field(c->out, r->lines[i].text, r->lines[i].len);
    }

    MCPDBG("-> read session %d since %llu: %lu lines, cursor %llu%s",
           s->id, (unsigned long long)since, (unsigned long)r->nlines,
           (unsigned long long)r->cursor,
           r->missing_to ? " (with gap)" : "");

    mcp_read_result_free(r);
    return conn_flush(c);
}

static bool cmd_screen(McpConn *c, const mcp_field *f, uint32_t n)
{
    McpSession *s;
    McpView v;

    if (n < 2)
        return resp_error(c, "screen needs a session id");
    s = session_by_id(field_uint(&f[1]));
    if (!s)
        return resp_error(c, "no such session");

    memset(&v, 0, sizeof(v));
    if (s->get_view)
        s->get_view(s->ctx, &v);

    put_uint32(c->out, 4);
    resp_str(c->out, "ok");
    resp_str(c->out, v.screen ? v.screen : "");
    resp_num(c->out, (uint64_t)v.cols);
    resp_num(c->out, (uint64_t)v.rows);
    sfree(v.screen);

    MCPDBG("-> screen session %d: %dx%d", s->id, v.cols, v.rows);
    return conn_flush(c);
}

/*
 * Type into a session.
 *
 * The bytes go through the front end, which puts them through the line
 * discipline exactly as typing at the keyboard does, so local echo and line
 * editing behave the same. They are also recorded in the capture buffer with
 * the MCP author tag, so the transcript says who wrote what - which matters
 * when a human and an agent are both at the same session.
 */
static bool cmd_send(McpConn *c, const mcp_field *f, uint32_t n)
{
    McpSession *s;

    if (n < 3)
        return resp_error(c, "send needs a session id and some text");
    s = session_by_id(field_uint(&f[1]));
    if (!s)
        return resp_error(c, "no such session");
    if (!s->writable)
        return resp_error(c, "this session does not allow typing "
                             "(Session -> Automation (MCP) -> "
                             "\"Also allow an MCP client to type\")");
    if (!s->send)
        return resp_error(c, "this session cannot be typed into");
    if (!s->send(s->ctx, f[2].p, f[2].len))
        return resp_error(c, "the session is busy (a ZMODEM transfer is "
                             "running); not sent");

    mcp_buffer_note_sent(s->buf, MCP_AUTHOR_MCP, f[2].p, f[2].len);

    MCPDBG("-> send %lu bytes to session %d", (unsigned long)f[2].len, s->id);
    put_uint32(c->out, 2);
    resp_str(c->out, "ok");
    resp_num(c->out, (uint64_t)f[2].len);
    return conn_flush(c);
}

/* A keystroke by name, so a client does not have to know the byte values. */
static bool key_bytes(const mcp_field *f, char *out, size_t *outlen)
{
    static const struct { const char *name; const char *bytes; } table[] = {
        { "enter",     "\r" },
        { "return",    "\r" },
        { "tab",       "\t" },
        { "esc",       "\033" },
        { "escape",    "\033" },
        { "space",     " " },
        { "backspace", "\177" },
        { "delete",    "\033[3~" },
        { "up",        "\033[A" },
        { "down",      "\033[B" },
        { "right",     "\033[C" },
        { "left",      "\033[D" },
        { "home",      "\033[H" },
        { "end",       "\033[F" },
        { "pageup",    "\033[5~" },
        { "pagedown",  "\033[6~" },
    };

    for (size_t i = 0; i < lenof(table); i++) {
        if (field_is(f, table[i].name)) {
            size_t len = strlen(table[i].bytes);
            memcpy(out, table[i].bytes, len);
            *outlen = len;
            return true;
        }
    }

    /* ctrl-a .. ctrl-z, and ctrl-@ .. ctrl-_ by number. */
    if (f->len >= 6 && !memcmp(f->p, "ctrl-", 5) && f->len == 6) {
        char ch = f->p[5];
        if (ch >= 'a' && ch <= 'z') {
            out[0] = (char)(ch - 'a' + 1);
            *outlen = 1;
            return true;
        }
        if (ch == '@' || ch == '[' || ch == '\\' || ch == ']' ||
            ch == '^' || ch == '_') {
            out[0] = (char)(ch - '@');
            *outlen = 1;
            return true;
        }
        if (ch == '?') {          /* ctrl-? is DEL */
            out[0] = '\177';
            *outlen = 1;
            return true;
        }
    }

    return false;
}

static bool cmd_key(McpConn *c, const mcp_field *f, uint32_t n)
{
    McpSession *s;
    char bytes[8];
    size_t len = 0;

    if (n < 3)
        return resp_error(c, "key needs a session id and a key name");
    s = session_by_id(field_uint(&f[1]));
    if (!s)
        return resp_error(c, "no such session");
    if (!s->writable)
        return resp_error(c, "this session does not allow typing "
                             "(Session -> Automation (MCP) -> "
                             "\"Also allow an MCP client to type\")");
    if (!s->send)
        return resp_error(c, "this session cannot be typed into");
    if (!key_bytes(&f[2], bytes, &len))
        return resp_error(c, "unknown key name (try enter, tab, esc, "
                             "backspace, up, down, left, right, ctrl-c)");
    if (!s->send(s->ctx, bytes, len))
        return resp_error(c, "the session is busy (a ZMODEM transfer is "
                             "running); not sent");

    mcp_buffer_note_sent(s->buf, MCP_AUTHOR_MCP, bytes, len);

    MCPDBG("-> key %.*s to session %d", (int)f[2].len, f[2].p, s->id);
    put_uint32(c->out, 1);
    resp_str(c->out, "ok");
    return conn_flush(c);
}

static bool cmd_version(McpConn *c)
{
    put_uint32(c->out, 3);
    resp_str(c->out, "ok");
    resp_str(c->out, ver);
    resp_num(c->out, MCP_PROTO_VERSION);
    MCPDBG("-> version: %s protocol %d", ver, MCP_PROTO_VERSION);
    return conn_flush(c);
}

static bool cmd_ping(McpConn *c)
{
    put_uint32(c->out, 1);
    resp_str(c->out, "ok");
    return conn_flush(c);
}

/* ------------------------------------------------------------------ */
/* The handshake and the command table                                  */
/* ------------------------------------------------------------------ */

static bool mcp_hello(McpConn *c, const mcp_field *f, uint32_t n)
{
    if (n < 3 || !field_is(&f[0], "hello")) {
        MCPDBG("first message was not the handshake");
        resp_error(c, "handshake required");
        return false;             /* and drop the connection */
    }
    if (field_uint(&f[1]) != MCP_PROTO_VERSION) {
        MCPDBG("client protocol %llu, we speak %d",
               (unsigned long long)field_uint(&f[1]), MCP_PROTO_VERSION);
        resp_error(c, "unsupported protocol version");
        return false;
    }
    if (f[2].len != 64 || !smemeq(f[2].p, token_hex, 64)) {
        MCPDBG("bad token from fd %d", c->fd);
        resp_error(c, "bad token");
        return false;
    }

    c->hello_done = true;
    put_uint32(c->out, 2);
    resp_str(c->out, "ok");
    resp_num(c->out, MCP_PROTO_VERSION);
    MCPDBG("handshake ok on fd %d", c->fd);
    return conn_flush(c);
}

static bool mcp_dispatch(McpConn *c, const unsigned char *msg, size_t total)
{
    mcp_field fields[MCP_MAX_FIELDS];
    uint32_t nfields = rd32(msg);
    size_t pos = 4;

    (void)total;

    for (uint32_t i = 0; i < nfields; i++) {
        uint32_t flen = rd32(msg + pos);
        pos += 4;
        fields[i].p = (const char *)(msg + pos);
        fields[i].len = flen;
        pos += flen;
    }

    if (!nfields) {
        MCPDBG("empty request");
        return false;
    }

    if (!c->hello_done)
        return mcp_hello(c, fields, nfields);

    if (mcp_debug()) {
        char *cmd = dupprintf("%.*s", (int)fields[0].len, fields[0].p);
        MCPDBG("<- %s (%u fields)", cmd, (unsigned)nfields);
        sfree(cmd);
    }

    if (field_is(&fields[0], "sessions"))
        return cmd_sessions(c);
    if (field_is(&fields[0], "read"))
        return cmd_read(c, fields, nfields);
    if (field_is(&fields[0], "screen"))
        return cmd_screen(c, fields, nfields);
    if (field_is(&fields[0], "send"))
        return cmd_send(c, fields, nfields);
    if (field_is(&fields[0], "key"))
        return cmd_key(c, fields, nfields);
    if (field_is(&fields[0], "version"))
        return cmd_version(c);
    if (field_is(&fields[0], "ping"))
        return cmd_ping(c);

    return resp_error(c, "unknown command");
}

/* ------------------------------------------------------------------ */
/* Connections                                                         */
/* ------------------------------------------------------------------ */

static McpConn *conn_find(int fd)
{
    for (McpConn *c = conns; c; c = c->next)
        if (c->fd == fd)
            return c;
    return NULL;
}

static void conn_close(McpConn *c)
{
    McpConn **pp = &conns;

    while (*pp && *pp != c)
        pp = &(*pp)->next;
    if (*pp)
        *pp = c->next;

    uxsel_del(c->fd);
    close(c->fd);
    sfree(c->in);
    strbuf_free(c->out);
    MCPDBG("closed fd %d", c->fd);
    sfree(c);
}

static bool conn_flush(McpConn *c)
{
    while (c->outpos < c->out->len) {
        ssize_t n = write(c->fd, c->out->s + c->outpos,
                          c->out->len - c->outpos);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            MCPDBG("fd %d write: %s", c->fd, strerror(errno));
            return false;
        }
        c->outpos += (size_t)n;
    }

    if (c->outpos >= c->out->len) {
        strbuf_clear(c->out);
        c->outpos = 0;
    }
    if (c->out->len > MCP_MAX_OUTPUT) {
        MCPDBG("fd %d is not reading; dropping it", c->fd);
        return false;
    }

    conn_want(c, c->out->len ? (SELECT_R | SELECT_W) : SELECT_R);
    return true;
}

static bool conn_read(McpConn *c)
{
    unsigned char buf[MCP_READ_CHUNK];
    ssize_t n;
    size_t pos = 0;

    n = read(c->fd, buf, sizeof(buf));
    if (n == 0) {
        MCPDBG("fd %d closed by peer", c->fd);
        return false;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return true;
        MCPDBG("fd %d read: %s", c->fd, strerror(errno));
        return false;
    }

    if (c->inlen + (size_t)n > c->inalloc) {
        size_t want = c->inalloc ? c->inalloc : 4096;
        while (want < c->inlen + (size_t)n)
            want *= 2;
        c->in = sresize(c->in, want, unsigned char);
        c->inalloc = want;
    }
    memcpy(c->in + c->inlen, buf, (size_t)n);
    c->inlen += (size_t)n;

    while (pos < c->inlen) {
        long total = msg_total(c->in + pos, c->inlen - pos);

        if (total == 0)
            break;                /* the rest of the message is still coming */
        if (total < 0) {
            MCPDBG("fd %d sent something that is not a message", c->fd);
            return false;
        }
        if (!mcp_dispatch(c, c->in + pos, (size_t)total))
            return false;
        pos += (size_t)total;
    }

    if (pos) {
        memmove(c->in, c->in + pos, c->inlen - pos);
        c->inlen -= pos;
    }
    if (c->inlen > MCP_MAX_MESSAGE) {
        MCPDBG("fd %d sent an oversized partial message", c->fd);
        return false;
    }

    return true;
}

static void mcp_conn_event(int fd, int event)
{
    McpConn *c = conn_find(fd);

    if (!c)
        return;

    if (event & SELECT_R) {
        if (!conn_read(c)) {
            conn_close(c);
            return;
        }
    }
    /* conn_read() may have closed it. */
    c = conn_find(fd);
    if (c && (event & SELECT_W)) {
        if (!conn_flush(c))
            conn_close(c);
    }
}

static void mcp_listen_event(int fd, int event)
{
    if (!(event & SELECT_R))
        return;

    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        McpConn *c;

        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            return;               /* EAGAIN, or something we cannot fix */
        }

        nonblock(cfd);
        c = snew(McpConn);
        memset(c, 0, sizeof(*c));
        c->fd = cfd;
        c->out = strbuf_new();
        c->next = conns;
        conns = c;
        conn_want(c, SELECT_R);
        MCPDBG("connection on fd %d", cfd);
    }
}

/* ------------------------------------------------------------------ */
/* The front end's interface                                           */
/* ------------------------------------------------------------------ */

McpSession *mcp_session_new(const char *name, size_t budget, bool writable,
                            mcp_view_fn get_view, mcp_send_fn send, void *ctx)
{
    McpSession *s = snew(McpSession);

    memset(s, 0, sizeof(*s));
    s->buf = mcp_buffer_new(budget);
    s->name = dupstr(name && *name ? name : "session");
    s->writable = writable;
    s->get_view = get_view;
    s->send = send;
    s->ctx = ctx;
    s->id = session_id_base() + next_session_id++;

    s->next = sessions;
    sessions = s;
    session_count++;

    /*
     * The server only exists while there is a session to drive, which is what
     * makes MCP opt-in: a PuTTY with no MCP-enabled session never creates a
     * socket, a token or a file.
     */
    if (!server_start())
        MCPDBG("session %d registered but the server is not up", s->id);

    MCPDBG("session %d registered as \"%s\" (%s)", s->id, s->name,
           s->writable ? "read-write" : "read-only");
    return s;
}

/*
 * Phase 4c: let the front end say who, if anyone, is being prompted. NULL (the
 * initial value, and the only value on a front end that never calls this) means
 * "nobody is being asked", which is what `sessions` and `read` report.
 */
void mcp_session_set_prompt_fn(McpSession *s, mcp_prompt_fn fn)
{
    if (s)
        s->get_prompt = fn;
}

/* The prompt now being put to the human, or NULL. Never fails. */
static const char *session_prompt(McpSession *s)
{
    const char *p;

    if (!s->get_prompt)
        return NULL;
    p = s->get_prompt(s->ctx);
    return p ? p : NULL;
}

void mcp_session_free(McpSession *s)
{
    McpSession **pp = &sessions;
    int id;

    if (!s)
        return;

    while (*pp && *pp != s)
        pp = &(*pp)->next;
    if (*pp)
        *pp = s->next;
    session_count--;

    id = s->id;
    mcp_buffer_free(s->buf);
    sfree(s->name);
    sfree(s);
    MCPDBG("session %d unregistered", id);

    if (!session_count)
        server_stop();
}

void mcp_session_set_name(McpSession *s, const char *name)
{
    if (!s || !name || !*name)
        return;
    sfree(s->name);
    s->name = dupstr(name);
}

void mcp_session_feed(McpSession *s, const void *data, size_t len)
{
    if (s)
        mcp_buffer_feed(s->buf, data, len);
}

void mcp_session_note_sent(McpSession *s, McpAuthor author,
                           const void *data, size_t len)
{
    if (s)
        mcp_buffer_note_sent(s->buf, author, data, len);
}

void mcp_session_note_typed(McpSession *s, const void *data, size_t len)
{
    if (s)
        mcp_buffer_note_typed(s->buf, data, len);
}
