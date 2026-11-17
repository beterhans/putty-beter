/*
 * The socket side of putty-mcp: find a running PuTTY MCP server, handshake
 * with it, and exchange requests.
 *
 * The protocol is deliberately dumb - a list of length-prefixed fields, no
 * JSON, no escaping - because the other end is C and should not grow a JSON
 * parser. The format is documented at the top of unix/mcp.c, and this file is
 * its only other reader.
 *
 *     message := u32 nfields, then nfields * (u32 length, length bytes)
 *
 * Everything is big-endian. Integers travel as decimal text, so the two ends
 * never have to agree on an integer width.
 */

import fs from 'node:fs';
import net from 'node:net';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

/* Bumped to 2 when `sessions` gained the read-only/read-write field and `send`
 * and `key` were added. Both ends ship together, so a mismatch is a mistake
 * rather than a compatibility question, and the handshake reports it. */
export const PROTOCOL_VERSION = 2;

/* ------------------------------------------------------------------ */
/* Framing                                                             */
/* ------------------------------------------------------------------ */

export function encode(fields) {
    const parts = [];
    const head = Buffer.alloc(4);
    head.writeUInt32BE(fields.length, 0);
    parts.push(head);

    for (const f of fields) {
        const body = Buffer.isBuffer(f) ? f : Buffer.from(String(f), 'utf8');
        const len = Buffer.alloc(4);
        len.writeUInt32BE(body.length, 0);
        parts.push(len, body);
    }
    return Buffer.concat(parts);
}

/*
 * Sanity bounds on a message. These have to be generous, because a *response*
 * is not a handful of fields: `read` returns three per line, and the default
 * 8 KB budget is roughly 200 lines, so a perfectly ordinary reply can carry
 * over 600 fields. A limit of 64 - which is ample for the requests, where the
 * most is four - silently rejected every reply from a session with more than
 * about 19 lines in it, and the error it produced was "malformed response from
 * PuTTY" rather than anything that pointed at the cause.
 *
 * The total size is what actually bounds a message; the field count is only
 * there to catch a nonsense length before allocating.
 */
const MAX_FIELDS = 1000000;
const MAX_FIELD = 1024 * 1024;
const MAX_MESSAGE = 4 * 1024 * 1024;

/*
 * The size of the complete message at the front of `buf`: 0 if it is not all
 * there yet, -1 if it is not a message at all.
 */
function messageSize(buf) {
    if (buf.length < 4) return 0;
    const nfields = buf.readUInt32BE(0);
    if (nfields > MAX_FIELDS) return -1;

    let pos = 4;
    for (let i = 0; i < nfields; i++) {
        if (buf.length < pos + 4) return 0;
        const len = buf.readUInt32BE(pos);
        pos += 4;
        if (len > MAX_FIELD) return -1;
        if (buf.length < pos + len) return 0;
        pos += len;
        if (pos > MAX_MESSAGE) return -1;
    }
    return pos;
}

export function decode(buf) {
    const nfields = buf.readUInt32BE(0);
    const out = [];
    let pos = 4;
    for (let i = 0; i < nfields; i++) {
        const len = buf.readUInt32BE(pos);
        pos += 4;
        out.push(buf.subarray(pos, pos + len).toString('utf8'));
        pos += len;
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* Are we running stale code?                                          */
/* ------------------------------------------------------------------ */

let stalenessWarned = false;

/*
 * An MCP client spawns the bridge once and keeps that process for the whole
 * life of its session, so editing anything in this package has no effect until
 * the client is restarted - and there is no way for a running process to
 * reload itself. The failure mode is nasty: the bridge keeps working, just
 * wrongly. (A field-count limit that was too small for a `read` reply was
 * exactly that: it looked like a malformed response from PuTTY, and nothing
 * pointed at the real cause.)
 *
 * So, once, on the first request: if any of our own source files is newer than
 * this process, say so on stderr, where a client logs it.
 */
function warnIfStale() {
    if (stalenessWarned)
        return;
    stalenessWarned = true;

    const startedAt = Date.now() - process.uptime() * 1000;
    const files = [];
    for (const dir of ['src', 'bin']) {
        const full = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', dir);
        try {
            for (const name of fs.readdirSync(full))
                if (name.endsWith('.mjs'))
                    files.push(path.join(full, name));
        } catch { /* not installed the way we expect; nothing to check */ }
    }

    const newer = files.filter((f) => {
        try {
            return fs.statSync(f).mtimeMs > startedAt;
        } catch {
            return false;
        }
    });

    if (newer.length) {
        process.stderr.write(
            'putty-mcp: this bridge process started before ' +
            `${newer.length} of its own source file(s) were last changed ` +
            `(${newer.map((f) => path.basename(f)).join(', ')}). ` +
            'It is running stale code, and cannot reload itself: ' +
            'restart the MCP client to pick the change up.\n');
    }
}

/* ------------------------------------------------------------------ */
/* Finding the server                                                  */
/* ------------------------------------------------------------------ */

/*
 * The first non-comment line of <base>/config-location, if it names a
 * directory that is actually there. This mirrors config_redirect_read() in
 * unix/storage.c, including expanding a leading "~" and nothing else.
 *
 * It matters because the server writes `mcp-endpoint` into the *effective*
 * configuration folder - which is the redirected one when the user has moved
 * their configuration - so a bridge that only looked at the base folder would
 * never see the file.
 */
function readRedirect(base) {
    let text;
    try {
        text = fs.readFileSync(path.join(base, 'config-location'), 'utf8');
    } catch {
        return null;
    }

    for (const raw of text.split('\n')) {
        const line = raw.trim();
        if (!line || line.startsWith('#'))
            continue;

        const expanded = line.startsWith('~') &&
            (line.length === 1 || line[1] === '/')
            ? os.homedir() + line.slice(1)
            : line;

        try {
            if (fs.statSync(expanded).isDirectory())
                return expanded;
        } catch { /* names nothing; ignore it, as storage.c does */ }
        return null;
    }
    return null;
}

/*
 * The configuration folder, in the same order unix/storage.c uses: $PUTTYDIR,
 * then ~/.putty, then an XDG directory - and, after each of those, whatever
 * its `config-location` redirect file points at.
 *
 * The server writes `mcp-endpoint` here so that a bridge whose $TMPDIR differs
 * from PuTTY's - a GUI app and a shell can disagree - can still find it.
 */
function configDirs() {
    const home = os.homedir();
    const bases = [];
    if (process.env.PUTTYDIR) bases.push(process.env.PUTTYDIR);
    bases.push(path.join(home, '.putty'));
    if (process.env.XDG_CONFIG_HOME)
        bases.push(path.join(process.env.XDG_CONFIG_HOME, 'putty'));
    bases.push(path.join(home, '.config', 'putty'));

    const out = [];
    for (const base of bases) {
        if (!out.includes(base))
            out.push(base);
        const effective = readRedirect(base);
        if (effective && !out.includes(effective))
            out.push(effective);
    }
    return out;
}

function runtimeDir() {
    let tmp = process.env.TMPDIR || '/tmp';
    tmp = tmp.replace(/\/+$/, '') || '/';
    return path.join(tmp, `putty-mcp-${process.getuid()}`);
}

function readEndpointFile(file) {
    let text;
    try {
        text = fs.readFileSync(file, 'utf8');
    } catch {
        return null;
    }

    const kv = {};
    for (const line of text.split('\n')) {
        const m = /^(\w+)\s+(.+?)\s*$/.exec(line);
        if (m) kv[m[1]] = m[2];
    }
    if (!kv.socket || !kv.token) return null;

    /*
     * A crash leaves the endpoint file behind. If its socket is gone the file
     * is stale, and we would rather say "no server" than fail to connect.
     */
    if (!fs.existsSync(kv.socket)) return null;

    return {
        socket: kv.socket,
        tokenFile: kv.token,
        pid: kv.pid ? Number(kv.pid) : undefined,
        version: kv.version ? Number(kv.version) : undefined,
        source: file,
    };
}

/* The names in a directory that look like endpoint files, newest first. */
function endpointFiles(dir) {
    let names;
    try {
        names = fs.readdirSync(dir);
    } catch {
        return [];
    }

    const files = [];
    for (const name of names) {
        if (!/^(mcp-)?endpoint(\.\d+)?$/.test(name))
            continue;
        const file = path.join(dir, name);
        try {
            files.push({ file, mtime: fs.statSync(file).mtimeMs });
        } catch { /* vanished between readdir and stat; ignore */ }
    }
    /* Newest first, so a freshly started PuTTY is the first thing tried. */
    files.sort((a, b) => b.mtime - a.mtime);
    return files.map((f) => f.file);
}

/*
 * Every place a server might be.
 *
 * There can be more than one: the plain `putty` and `pterm` binaries run one
 * process per window and each has its own socket, so this returns all of them
 * and the caller merges the session lists. A stale file, or a socket that
 * refuses the connection, is simply skipped.
 */
export function endpointCandidates(opts = {}) {
    const out = [];
    const seen = new Set();
    const push = (c) => {
        if (c && c.socket && !seen.has(c.socket)) {
            seen.add(c.socket);
            out.push(c);
        }
    };

    /* Explicitly named, and therefore first. */
    if (opts.endpoint)
        push(readEndpointFile(opts.endpoint));
    if (process.env.PUTTY_MCP_ENDPOINT)
        push(readEndpointFile(process.env.PUTTY_MCP_ENDPOINT));

    for (const dir of configDirs())
        for (const file of endpointFiles(dir))
            push(readEndpointFile(file));

    const rt = runtimeDir();
    for (const file of endpointFiles(rt))
        push(readEndpointFile(file));

    /* An explicit socket and token override everything. */
    const sock = opts.socket || path.join(rt, 'sock');
    const token = opts.tokenFile || path.join(rt, 'token');
    if (fs.existsSync(sock) && fs.existsSync(token))
        push({ socket: sock, tokenFile: token, source: 'derived' });

    return out;
}

/* ------------------------------------------------------------------ */
/* The client                                                          */
/* ------------------------------------------------------------------ */

export class McpError extends Error {}

export class Client {
    constructor(sock, info, opts = {}) {
        this.sock = sock;
        this.info = info;
        this.debug = !!opts.debug;
        this.inbuf = Buffer.alloc(0);
        this.waiting = [];
        this.dead = null;

        sock.on('data', (d) => {
            this.inbuf = Buffer.concat([this.inbuf, d]);
            this.pump();
        });
        sock.on('error', (e) => this.die(e));
        sock.on('close', () => this.die(new McpError('connection closed')));
    }

    static async connect(cand, opts = {}) {
        const sock = await new Promise((resolve, reject) => {
            const s = net.connect(cand.socket);
            const onErr = (e) => {
                s.destroy();
                reject(e);
            };
            s.once('connect', () => {
                s.off('error', onErr);
                resolve(s);
            });
            s.once('error', onErr);
        });

        let token;
        try {
            token = fs.readFileSync(cand.tokenFile, 'utf8').trim();
        } catch (e) {
            sock.destroy();
            throw new McpError(`cannot read token file ${cand.tokenFile}: ${e.message}`);
        }

        const c = new Client(sock, cand, opts);
        if (opts.debug)
            process.stderr.write(`[putty-mcp] connected to ${cand.socket} (from ${cand.source})\n`);

        const reply = await c.exchange(['hello', String(PROTOCOL_VERSION), token]);
        if (reply[0] !== 'ok')
            throw new McpError(`handshake failed: ${reply[1] || 'unknown error'}`);
        return c;
    }

    die(err) {
        if (this.dead) return;
        this.dead = err;
        const waiting = this.waiting;
        this.waiting = [];
        for (const w of waiting) w.reject(err);
    }

    close() {
        this.sock.destroy();
    }

    pump() {
        while (this.waiting.length) {
            const total = messageSize(this.inbuf);
            if (total < 0) {
                this.die(new McpError('malformed response from PuTTY'));
                return;
            }
            if (total === 0) return;

            const msg = this.inbuf.subarray(0, total);
            this.inbuf = this.inbuf.subarray(total);
            this.waiting.shift().resolve(decode(msg));
        }
    }

    exchange(fields) {
        if (this.dead) return Promise.reject(this.dead);
        if (this.debug)
            process.stderr.write(`[putty-mcp] -> ${JSON.stringify(fields)}\n`);

        return new Promise((resolve, reject) => {
            this.waiting.push({ resolve, reject });
            this.sock.write(encode(fields));
        }).then((reply) => {
            if (this.debug)
                process.stderr.write(`[putty-mcp] <- ${JSON.stringify(reply)}\n`);
            return reply;
        });
    }

    /*
     * A request that throws on an "error" reply, so callers do not each have
     * to check. The fields after the status are returned as strings.
     */
    async request(command, ...args) {
        const reply = await this.exchange([command, ...args]);
        if (reply[0] === 'error')
            throw new McpError(reply[1] || 'unknown error');
        if (reply[0] !== 'ok')
            throw new McpError(`unexpected reply ${JSON.stringify(reply[0])}`);
        return reply.slice(1);
    }
}

/*
 * Connect to every server that answers. Connections are made per call and
 * closed again, which keeps the bridge stateless: a PuTTY restart, or a new
 * window in a new process, is invisible to the caller.
 */
async function connectAll(opts = {}) {
    warnIfStale();
    const candidates = endpointCandidates(opts);
    const clients = [];
    const errors = [];

    for (const cand of candidates) {
        try {
            clients.push({ client: await Client.connect(cand, opts), endpoint: cand });
        } catch (e) {
            errors.push(`${cand.socket}: ${e.message}`);
            if (opts.debug)
                process.stderr.write(`[putty-mcp] ${cand.socket}: ${e.message}\n`);
        }
    }
    return { clients, errors, candidates };
}

function closeAll(clients) {
    for (const c of clients)
        c.client.close();
}

/*
 * The two failures look identical to a caller but have completely different
 * causes: either no server is running at all (no endpoint file anywhere), or
 * endpoint files are there and nobody is listening - which is what a PuTTY
 * that was killed rather than closed leaves behind.
 */
function noServer({ candidates, errors }) {
    if (!candidates.length) {
        return new McpError(
            'no PuTTY MCP server found. Open a session in PuTTY with ' +
            'Session -> Automation (MCP) turned on, then try again.');
    }
    return new McpError(`no PuTTY MCP server answered (${errors.join('; ')})`);
}

/*
 * Run `fn` against whichever server owns `session`. Session ids are unique
 * across processes - they carry the pid - but the bridge does not rely on
 * that: it simply asks each server in turn and takes the first that does not
 * say "no such session".
 */
async function withSession(session, opts, fn) {
    const { clients, ...rest } = await connectAll(opts);
    if (!clients.length)
        throw noServer(rest);

    let missing = null;
    try {
        for (const { client } of clients) {
            try {
                return await fn(client);
            } catch (e) {
                if (e instanceof McpError && /no such session/.test(e.message)) {
                    missing = e;
                    continue;
                }
                throw e;
            }
        }
    } finally {
        closeAll(clients);
    }
    throw missing ?? new McpError(`no session with id ${session}`);
}

/* ------------------------------------------------------------------ */
/* The commands, in the shape the tools want                           */
/* ------------------------------------------------------------------ */

/* Every session from every server, merged. */
export async function listSessions(opts = {}) {
    const { clients, ...rest } = await connectAll(opts);
    if (!clients.length)
        throw noServer(rest);

    const sessions = [];
    try {
        for (const { client, endpoint } of clients) {
            const [count, ...fields] = await client.request('sessions');
            for (let i = 0; i < Number(count); i++) {
                sessions.push({
                    id: Number(fields[3 * i]),
                    name: fields[3 * i + 1],
                    writable: fields[3 * i + 2] === 'rw',
                    pid: endpoint.pid,
                });
            }
        }
    } finally {
        closeAll(clients);
    }
    return sessions;
}

export async function readSession({ session, since = 0, maxBytes } = {}, opts = {}) {
    return withSession(session, opts, async (c) => {
        const f = await c.request(
            'read', String(session),
            since ? String(since) : '',
            maxBytes ? String(maxBytes) : '');

        const [cursor, totalLines, missingFrom, missingTo, omitted, nlines] = f;
        const lines = [];
        for (let i = 0; i < Number(nlines); i++) {
            const base = 6 + 3 * i;
            lines.push({
                id: Number(f[base]),
                author: f[base + 1],
                text: f[base + 2],
            });
        }

        const out = {
            cursor: Number(cursor),
            total_lines: Number(totalLines),
            omitted: Number(omitted),
            lines,
        };
        if (missingTo)
            out.gap = { missing_from: Number(missingFrom), missing_to: Number(missingTo) };
        return out;
    });
}

export async function screenSession({ session } = {}, opts = {}) {
    return withSession(session, opts, async (c) => {
        const [text, cols, rows] = await c.request('screen', String(session));
        return { text, cols: Number(cols), rows: Number(rows) };
    });
}

/*
 * Type text into a session. Refused by the server unless the session allows
 * typing, and while a ZMODEM transfer is running.
 */
export async function sendToSession({ session, text } = {}, opts = {}) {
    return withSession(session, opts, async (c) => {
        const [bytes] = await c.request('send', String(session), text ?? '');
        return { sent: Number(bytes) };
    });
}

/* A keystroke by name: enter, tab, esc, backspace, up, down, left, right, ctrl-c... */
export async function sendKey({ session, key } = {}, opts = {}) {
    return withSession(session, opts, async (c) => {
        await c.request('key', String(session), key);
        return { sent: key };
    });
}

/* The first server that answers is as good as any for these. */
export async function serverVersion(opts = {}) {
    const { clients, ...rest } = await connectAll(opts);
    if (!clients.length)
        throw noServer(rest);
    try {
        const [version, protocol] = await clients[0].client.request('version');
        return { version, protocol: Number(protocol) };
    } finally {
        closeAll(clients);
    }
}

export async function ping(opts = {}) {
    const { clients, ...rest } = await connectAll(opts);
    if (!clients.length)
        throw noServer(rest);
    try {
        await clients[0].client.request('ping');
        return true;
    } finally {
        closeAll(clients);
    }
}
