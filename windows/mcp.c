/*
 * PuTTY-beter: the MCP server.
 *
 * See mcp.h for what this is for and PLAN.md section 7 for the design. This
 * file is the pipe server, the session registry and the wire protocol; the
 * capture buffer is unix/mcp-buffer.c, which knows nothing about either.
 *
 * This is the Windows twin of unix/mcp.c. The wire protocol, the session
 * registry, the request handlers and the capture buffer are shared - the
 * buffer is literally the same file - and only the transport differs, so the
 * two files are deliberately kept recognisably parallel: a change to the
 * protocol has to be made in both. WINDOWS TWIN OF unix/mcp.c.
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
 *                                                     <prompt>
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
 * WHERE THE PIPE LIVES
 *
 * A Windows PuTTY has no Unix socket, so the server listens on a named pipe
 * instead: \\.\pipe\putty-mcp-<user>-<pid>. That is the whole of the difference
 * between this file and unix/mcp.c.
 *
 * One pipe per process, named after the pid, for the same reason the Unix side
 * does it: the plain `putty' and `pterm' binaries run one process per window,
 * so a single fixed name would let a second PuTTY steal the first one's pipe.
 * The username is in the name as well, because the pipe namespace is
 * machine-wide while the Unix socket's is per-user (Pageant's precedent,
 * agent_named_pipe_name()). The files need no such treatment: %TEMP% and the
 * configuration folder are already per-user, and the bridge takes the pipe name
 * from the endpoint record rather than reconstructing it.
 *
 * The token and an endpoint file live in %TEMP%\putty-mcp-<pid>\, and the
 * endpoint file - pipe name, token path, pid, protocol version - is also
 * written into the configuration folder as `mcp-endpoint.<pid>'. That folder
 * is a real folder on Windows too, so it is what lets the bridge find a server
 * whose %TEMP% differs from its own; a pterm, which has no configuration
 * folder, gets only the %TEMP% copy.
 *
 * Session ids carry the pid for the same reason: the bridge merges the lists
 * from every server it finds, so an id has to say which one it belongs to.
 *
 * PERMISSIONS
 *
 * There is no socket mode to set. The pipe is created with a security
 * descriptor that grants the current user access and revokes the World and
 * Network SIDs (make_private_security_descriptor, the same call
 * windows/named-pipe-server.c uses), and PIPE_REJECT_REMOTE_CLIENTS keeps it
 * local. That is the Windows equivalent of the Unix side's owner-only 0600
 * socket, and it is what makes the shared token a second line of defence
 * rather than the only one.
 *
 * DEBUG
 *
 * Set PUTTY_MCP_DEBUG=1 for a trace on stderr of the pipe, the handshake,
 * every request and every response.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "putty.h"
#include "misc.h"
#include "mcp.h"

#include "security-api.h"

/*
 * After putty.h: wincrypt.h needs windows.h, which putty.h has already pulled
 * in (and it must not be first, or windows.h would come before winsock2.h).
 */
#include <wincrypt.h>                  /* CryptGenRandom, for the token */

/*
 * Bumped to 2 when `sessions` gained the read-only/read-write field, and `send`
 * and `key` were added. Bumped to 3 by phase 4c, when `sessions` and `read`
 * gained the human-prompt field, so a client can tell "the device is waiting
 * for me" from "the window is waiting for the person at the keyboard". Both
 * ends are shipped together, and the handshake checks this, so a mismatched
 * pair says so instead of misreading fields.
 */
#define MCP_PROTO_VERSION 3

#define MCP_MAX_FIELDS 64
#define MCP_MAX_FIELD (1024 * 1024)
#define MCP_MAX_MESSAGE (4 * 1024 * 1024)
#define MCP_MAX_OUTPUT (4 * 1024 * 1024)

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
    HANDLE pipe;                /* the connected pipe instance */
    struct handle *in_h;        /* handle-io reader, NULL once freed */
    struct handle *out_h;       /* handle-io writer, NULL once freed */
    bool hello_done;
    unsigned char *in;
    size_t inlen, inalloc;
    strbuf *out;
    size_t outpos;
    struct McpConn *next;
};

static bool server_up;
static HANDLE listen_pipe = INVALID_HANDLE_VALUE;
static OVERLAPPED listen_ovl;
static HandleWait *listen_wait;
static PSECURITY_DESCRIPTOR listen_psd;
static PACL listen_acl;
static char *dir_path, *pipe_path, *token_path, *endpoint_path;
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
        unsigned long pid = (unsigned long)GetCurrentProcessId() % 1000000UL;
        id_base = (int)(pid * 1000UL);
    }
    return id_base;
}

static uint32_t rd32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/*
 * Write a small file: the token, or the endpoint record. `path' is UTF-8, like
 * every other path the fork keeps, and is converted on the way to the API; the
 * content is ASCII either way.
 *
 * There is no mode to pass, unlike the Unix side. A file in %TEMP% inherits
 * that directory's ACL, which is already per-user, and what actually guards
 * the server is the pipe's security descriptor - not the permissions on these
 * files.
 */
static bool write_file(const char *path, const char *data, size_t len)
{
    Filename *fn = filename_from_utf8(path);
    FILE *fp = f_open(fn, "wb", true);
    bool ok = false;

    if (fp) {
        ok = (fwrite(data, 1, len, fp) == len);
        if (fclose(fp) != 0)
            ok = false;
    }
    filename_free(fn);
    return ok;
}

static void remove_file(const char *path)
{
    Filename *fn;

    if (!path)
        return;
    fn = filename_from_utf8(path);
    DeleteFileW(fn->wpath);
    filename_free(fn);
}

static bool make_dir(const char *path)
{
    Filename *fn = filename_from_utf8(path);
    bool ok = (CreateDirectoryW(fn->wpath, NULL) != 0 ||
               GetLastError() == ERROR_ALREADY_EXISTS);

    filename_free(fn);
    return ok;
}

/*
 * Remove the runtime directory, if it is empty.
 *
 * Unlike the Unix side, this directory is per-*process* rather than per-user,
 * so a clean exit can take it with it - the Unix one is shared between
 * processes and has to stay. RemoveDirectoryW refuses a directory with
 * anything still in it, and that is the outcome we want: the records above have
 * just been unlinked, so anything else in there is not ours to delete.
 */
static void remove_dir(const char *path)
{
    Filename *fn;

    if (!path)
        return;
    fn = filename_from_utf8(path);
    RemoveDirectoryW(fn->wpath);
    filename_free(fn);
}

/*
 * Rewrite a native path's backslashes as forward slashes, in place.
 *
 * The fork keeps paths POSIX-shaped inside the program and converts to native
 * separators only where a path is *shown* to a person (win_native_path() in
 * windows/utils/defaults.c). The two directories this file writes records into
 * both arrive in native form - %TEMP% from the environment, the configuration
 * folder out of config-location.conf - so without this the endpoint record reads
 * "C:\Users\me\AppData\Local\Temp/putty-mcp-1234/token", with two kinds of
 * slash in one path. Windows accepts either, so this is tidiness, not a fix;
 * rewriting in place is safe because the string is one we allocated.
 */
static void posix_path(char *path)
{
    for (; *path; path++)
        if (*path == '\\')
            *path = '/';
}

/*
 * %TEMP%\putty-mcp-<pid>. A named pipe is not a filesystem object, so this
 * directory is not where the server lives - it is where a client that cannot
 * read the configuration folder finds the token and the pipe name. Resolved
 * from the environment at run time, because a profile can live on another
 * drive.
 */
static char *runtime_dir(void)
{
    const char *tmp = getenv("TEMP");
    char *trimmed, *dir;
    size_t n;

    if (!tmp || !*tmp)
        tmp = getenv("TMP");
    if (!tmp || !*tmp)
        tmp = ".";

    trimmed = dupstr(tmp);
    n = strlen(trimmed);
    while (n > 1 && (trimmed[n-1] == '/' || trimmed[n-1] == '\\'))
        trimmed[--n] = '\0';

    posix_path(trimmed);

    dir = dupprintf("%s/putty-mcp-%ld", trimmed,
                    (long)GetCurrentProcessId());
    sfree(trimmed);
    return dir;
}

/*
 * The token every client must present in its handshake. It is defence in
 * depth: the pipe is already owner-only (see the security descriptor in
 * server_start), so the token is there for the case where a process gets hold
 * of the pipe anyway.
 *
 * CryptGenRandom rather than PuTTY's own random_read(), because that lives in
 * the crypto stack, which puttytel and pterm do not both link. If the token
 * cannot be made, the server refuses to start rather than fall back to
 * something guessable.
 */
static bool make_token(char *hex)
{
    unsigned char raw[32];
    HCRYPTPROV prov = 0;

    if (!CryptAcquireContext(&prov, NULL, NULL, PROV_RSA_FULL,
                             CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        MCPDBG("CryptAcquireContext: %s", win_strerror(GetLastError()));
        return false;
    }
    if (!CryptGenRandom(prov, sizeof(raw), raw)) {
        MCPDBG("CryptGenRandom: %s", win_strerror(GetLastError()));
        CryptReleaseContext(prov, 0);
        return false;
    }
    CryptReleaseContext(prov, 0);

    for (size_t i = 0; i < sizeof(raw); i++)
        sprintf(hex + 2*i, "%02x", raw[i]);
    hex[64] = '\0';
    return true;
}

static void conn_close(McpConn *c);
static bool conn_flush(McpConn *c);
static void conn_new(HANDLE pipe);
static void listen_loop(bool got_one_already);
static void listen_callback(void *ctx);

static void server_stop(void)
{
    while (conns)
        conn_close(conns);

    if (listen_wait) {
        delete_handle_wait(listen_wait);
        listen_wait = NULL;
    }
    if (listen_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(listen_pipe);
        listen_pipe = INVALID_HANDLE_VALUE;
    }
    if (listen_ovl.hEvent) {
        CloseHandle(listen_ovl.hEvent);
        listen_ovl.hEvent = NULL;
    }
    if (listen_acl) {
        LocalFree(listen_acl);
        listen_acl = NULL;
    }
    if (listen_psd) {
        LocalFree(listen_psd);
        listen_psd = NULL;
    }

    remove_file(token_path);
    remove_file(endpoint_path);
    remove_file(config_endpoint_path);
    remove_dir(dir_path);

    sfree(dir_path);             dir_path = NULL;
    sfree(pipe_path);            pipe_path = NULL;
    sfree(token_path);           token_path = NULL;
    sfree(endpoint_path);        endpoint_path = NULL;
    sfree(config_endpoint_path); config_endpoint_path = NULL;

    server_up = false;
    MCPDBG("server stopped");
}

/*
 * One listening instance of the pipe. The name has a per-process instance
 * limit of PIPE_UNLIMITED_INSTANCES, so after a client has been accepted this
 * is called again to put a fresh instance in its place.
 *
 * FILE_FLAG_OVERLAPPED is not optional: handle-io.c drives the accepted
 * connection with overlapped reads and writes, and a pipe created without it
 * cannot be used that way.
 */
static bool create_pipe(bool first_instance)
{
    SECURITY_ATTRIBUTES sa;

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = listen_psd;
    sa.bInheritHandle = false;

    listen_pipe = CreateNamedPipe(
        /* lpName */
        pipe_path,

        /* dwOpenMode */
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
        (first_instance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),

        /* dwPipeMode */
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
#ifdef PIPE_REJECT_REMOTE_CLIENTS
        | PIPE_REJECT_REMOTE_CLIENTS
#endif
        ,

        /* nMaxInstances */
        PIPE_UNLIMITED_INSTANCES,

        /* nOutBufferSize, nInBufferSize */
        4096, 4096,

        /* nDefaultTimeOut */
        0 /* default timeout */,

        /* lpSecurityAttributes */
        &sa);

    return listen_pipe != INVALID_HANDLE_VALUE;
}

/*
 * Wait for a client. This is the same shape as named_pipe_accept_loop() in
 * windows/named-pipe-server.c, minus the Plug: ConnectNamedPipe either reports
 * a connection already waiting, or tells us the overlapped operation is in
 * progress and we come back through listen_callback() when the event fires.
 */
static void listen_loop(bool got_one_already)
{
    while (1) {
        int error;

        if (got_one_already) {
            got_one_already = false;
            error = 0;
        } else {
            if (ConnectNamedPipe(listen_pipe, &listen_ovl))
                error = 0;
            else
                error = GetLastError();

            if (error == ERROR_IO_PENDING)
                return;
        }

        if (error == 0 || error == ERROR_PIPE_CONNECTED) {
            /*
             * listen_pipe now refers to the connection rather than to a
             * listening instance, so hand it to a new connection and create a
             * fresh instance to listen on.
             */
            HANDLE conn = listen_pipe;

            listen_pipe = INVALID_HANDLE_VALUE;
            conn_new(conn);

            if (!create_pipe(false)) {
                MCPDBG("cannot create the next pipe instance: %s",
                       win_strerror(GetLastError()));
                break;
            }
            continue;
        }

        MCPDBG("error while listening on %s: %s", pipe_path,
               win_strerror(error));
        break;
    }
}

static void listen_callback(void *ctx)
{
    listen_loop(true);
}

static bool server_start(void)
{
    char *body, *sd_err = NULL;

    if (server_up)
        return true;

    dir_path = runtime_dir();
    if (!make_dir(dir_path)) {
        MCPDBG("cannot create %s", dir_path);
        sfree(dir_path); dir_path = NULL;
        return false;
    }

    /*
     * One pipe, token and endpoint file *per process*, named after the pid.
     *
     * A single fixed name would be actively harmful: the plain `putty' and
     * `pterm' binaries run one process per window, so a second PuTTY would
     * take the name over and leave the first one silently unreachable.
     *
     * A file left behind by a PuTTY that was killed is harmless: the bridge
     * skips an endpoint whose pipe refuses the connection.
     *
     * The *pipe* name also carries the username. The pipe namespace is
     * machine-wide, so two users whose pids happen to coincide would otherwise
     * collide, and FILE_FLAG_FIRST_PIPE_INSTANCE would then make the second
     * server fail with nothing but an MCPDBG line - Pageant names its pipe the
     * same way (agent_named_pipe_name()). Nothing downstream reconstructs the
     * name: the endpoint record names the pipe, and the bridge connects to
     * what the record says.
     */
    {
        char *user = get_username();
        pipe_path = dupprintf("\\\\.\\pipe\\putty-mcp-%s-%ld",
                              user ? user : "user",
                              (long)GetCurrentProcessId());
        sfree(user);
    }
    token_path = dupprintf("%s/token", dir_path);
    endpoint_path = dupprintf("%s/endpoint", dir_path);

    /*
     * Owner-only: this is the Windows form of the Unix side's 0600 socket, and
     * it is what the token backs up rather than replaces.
     */
    if (!make_private_security_descriptor(GENERIC_READ | GENERIC_WRITE,
                                          &listen_psd, &listen_acl,
                                          &sd_err)) {
        MCPDBG("cannot make the pipe security descriptor: %s",
               sd_err ? sd_err : "(no detail)");
        sfree(sd_err);
        server_stop();
        return false;
    }

    if (!create_pipe(true)) {
        MCPDBG("CreateNamedPipe %s: %s", pipe_path,
               win_strerror(GetLastError()));
        server_stop();
        return false;
    }

    memset(&listen_ovl, 0, sizeof(listen_ovl));
    listen_ovl.hEvent = CreateEvent(NULL, true, false, NULL);
    listen_wait = add_handle_wait(listen_ovl.hEvent, listen_callback, NULL);

    if (!make_token(token_hex)) {
        server_stop();
        return false;
    }
    if (!write_file(token_path, token_hex, 64)) {
        MCPDBG("cannot write %s", token_path);
        server_stop();
        return false;
    }

    body = dupprintf("pipe %s\ntoken %s\npid %ld\nversion %d\n",
                     pipe_path, token_path, (long)GetCurrentProcessId(),
                     MCP_PROTO_VERSION);
    if (!write_file(endpoint_path, body, strlen(body)))
        MCPDBG("cannot write %s", endpoint_path);

    /*
     * The configuration folder copy is what lets the bridge find a server
     * whose %TEMP% differs from its own - a GUI app and a shell can disagree
     * about that.
     *
     * The Unix side asks saved_sessions first, because pterm there has no
     * configuration folder. Every Windows front end has one (windows/window.c
     * calls config_location_init() before conf_new() for all of them), and a
     * front end with no MCP-enabled session never gets this far, so there is
     * nothing to ask.
     */
    {
        char *dir = config_location_current();
        posix_path(dir);
        config_endpoint_path = dupprintf("%s/mcp-endpoint.%ld", dir,
                                         (long)GetCurrentProcessId());
        if (write_file(config_endpoint_path, body, strlen(body)))
            MCPDBG("endpoint also written to %s", config_endpoint_path);
        else
            MCPDBG("cannot write %s", config_endpoint_path);
        sfree(dir);
    }
    sfree(body);

    server_up = true;
    listen_loop(false);

    MCPDBG("listening on %s", pipe_path);
    return true;
}

bool mcp_server_running(void)
{
    return server_up;
}

/*
 * The Unix side returns the socket path; here it is the pipe name. Only
 * diagnostics use it.
 */
const char *mcp_server_socket_path(void)
{
    return pipe_path;
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
        MCPDBG("bad token from a client");
        resp_error(c, "bad token");
        return false;
    }

    c->hello_done = true;
    put_uint32(c->out, 2);
    resp_str(c->out, "ok");
    resp_num(c->out, MCP_PROTO_VERSION);
    MCPDBG("handshake ok");
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

/*
 * Bytes have arrived from a client. handle-io.c runs this on the main thread,
 * from its reader thread's signal, so everything below is ordinary
 * single-threaded code; the return value is the number of bytes we have taken
 * responsibility for, which is all of them.
 *
 * The framing loop is the same as unix/mcp.c's conn_read(), and deliberately
 * so: the protocol is the one part of this file that must not diverge.
 */
static size_t conn_gotdata(struct handle *h, const void *data, size_t len,
                           int err)
{
    McpConn *c = (McpConn *)handle_get_privdata(h);
    size_t pos = 0;

    if (err) {
        MCPDBG("read error from the pipe: %s", win_strerror(err));
        conn_close(c);
        return 0;
    }
    if (len == 0) {
        MCPDBG("client closed the pipe");
        conn_close(c);
        return 0;
    }

    if (c->inlen + len > c->inalloc) {
        size_t want = c->inalloc ? c->inalloc : 4096;
        while (want < c->inlen + len)
            want *= 2;
        c->in = sresize(c->in, want, unsigned char);
        c->inalloc = want;
    }
    memcpy(c->in + c->inlen, data, len);
    c->inlen += len;

    while (pos < c->inlen) {
        long total = msg_total(c->in + pos, c->inlen - pos);

        if (total == 0)
            break;                /* the rest of the message is still coming */
        if (total < 0) {
            MCPDBG("client sent something that is not a message");
            conn_close(c);
            return 0;
        }
        if (!mcp_dispatch(c, c->in + pos, (size_t)total)) {
            conn_close(c);
            return 0;
        }
        pos += (size_t)total;
    }

    if (pos) {
        memmove(c->in, c->in + pos, c->inlen - pos);
        c->inlen -= pos;
    }
    if (c->inlen > MCP_MAX_MESSAGE) {
        MCPDBG("client sent an oversized partial message");
        conn_close(c);
        return 0;
    }

    return len;
}

static void conn_sentdata(struct handle *h, size_t new_backlog, int err,
                          bool close)
{
    McpConn *c = (McpConn *)handle_get_privdata(h);

    if (err) {
        MCPDBG("write error to the pipe: %s", win_strerror(err));
        conn_close(c);
        return;
    }
    /*
     * Otherwise there is nothing to do. conn_flush() hands bytes to
     * handle_write() and forgets them, and handle-io.c's writer thread calls
     * back only to report progress or a failure.
     */
}

static void conn_new(HANDLE pipe)
{
    McpConn *c = snew(McpConn);

    memset(c, 0, sizeof(*c));
    c->pipe = pipe;
    c->out = strbuf_new();
    c->next = conns;
    conns = c;

    /*
     * HANDLE_FLAG_OVERLAPPED is required, because the pipe instance was
     * created with FILE_FLAG_OVERLAPPED: handle-io.c reads and writes it
     * through overlapped operations on its own threads, which is why the
     * server needs no event-loop plumbing of its own beyond the accept.
     */
    c->in_h = handle_input_new(pipe, conn_gotdata, c, HANDLE_FLAG_OVERLAPPED);
    c->out_h = handle_output_new(pipe, conn_sentdata, c,
                                 HANDLE_FLAG_OVERLAPPED);

    MCPDBG("connection accepted");
}

static void conn_close(McpConn *c)
{
    McpConn **pp = &conns;

    while (*pp && *pp != c)
        pp = &(*pp)->next;
    if (*pp)
        *pp = c->next;

    /*
     * Free the handle-io wrappers before the handle itself. handle_free()
     * defers the real teardown while a wrapper's thread is mid-operation, and
     * a moribund handle never calls back into us again - which is what makes
     * it safe to free the McpConn below while those threads finish.
     */
    if (c->in_h) {
        handle_free(c->in_h);
        c->in_h = NULL;
    }
    if (c->out_h) {
        handle_free(c->out_h);
        c->out_h = NULL;
    }
    if (c->pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(c->pipe);
        c->pipe = INVALID_HANDLE_VALUE;
    }
    sfree(c->in);
    strbuf_free(c->out);
    MCPDBG("connection closed");
    sfree(c);
}

/*
 * Push whatever has been framed into the pipe. This is the seam the protocol
 * core is written against - every cmd_* ends with it - and on Windows it is
 * the whole of the output path: handle_write() queues the bytes and
 * handle-io.c's writer thread does the rest.
 *
 * A client that stops reading is dropped rather than allowed to grow the queue
 * without limit, which is what the Unix side's MCP_MAX_OUTPUT check does too.
 */
static bool conn_flush(McpConn *c)
{
    if (c->outpos < c->out->len) {
        handle_write(c->out_h, c->out->s + c->outpos,
                     c->out->len - c->outpos);
        c->outpos = c->out->len;
    }
    if (c->outpos >= c->out->len) {
        strbuf_clear(c->out);
        c->outpos = 0;
    }
    if (handle_backlog(c->out_h) > (size_t)MCP_MAX_OUTPUT) {
        MCPDBG("client is not reading; dropping it");
        return false;
    }

    return true;
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
