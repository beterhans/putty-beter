/*
 * The human-facing half of putty-mcp.
 *
 * The same program is both the MCP bridge (`putty-mcp mcp`, which is what an
 * agent launches) and a plain command-line client. That is deliberate: the
 * whole point of the dumb socket protocol is that it can be driven with no
 * LLM in the loop at all, so `putty-mcp list` and `putty-mcp read 1` are the
 * first things to reach for when something is wrong.
 *
 *   putty-mcp list                 the sessions PuTTY is offering
 *   putty-mcp read 1 [--since N]   read a session's output
 *   putty-mcp screen 1             the visible screen
 *   putty-mcp manual               the user manual the agent reads
 *   putty-mcp mcp                  the MCP server, on stdio
 */

import {
    listSessions, readSession, screenSession, sendToSession, sendKey,
    serverVersion, ping, endpointCandidates, McpError, runCommand,
} from './protocol.mjs';
import { userManual, manualPath } from './manual.mjs';
import { runServer, VERSION } from './server.mjs';

const USAGE = `putty-mcp ${VERSION} - read a live PuTTY session

Usage:
  putty-mcp mcp                     Run the MCP server on stdio (this is what
                                    an agent launches; not for humans)
  putty-mcp list                    List the sessions PuTTY offers to MCP
  putty-mcp read <session>          Print a session's output
  putty-mcp screen <session>        Print the visible screen
  putty-mcp send <session> <text>   Type text into a session ("\n" = Return;
                                    or pass --text with real newlines)
  putty-mcp run <session> <command> Run one command and wait for it to finish,
                                    printing just its output. Answers a
                                    "--More--" pager by itself.
  putty-mcp key <session> <key>     Send one keystroke: enter, tab, esc,
                                    backspace, up, down, left, right, ctrl-c
  putty-mcp manual                  Print the user manual the agent reads
  putty-mcp version                 PuTTY build and protocol version
  putty-mcp ping                    Check that a PuTTY server is reachable

Options:
  --since <id>        (read) return lines with an id greater than this
  --max-bytes <n>     (read, run) budget for one call, default 8192
  --expect <regex>    (run) the prompt pattern, if the default family does
                      not fit this device. Replaces the default.
  --timeout <ms>      (run) give up after this long, default 30000
  --quiet <ms>        (run) how long the device must be silent before the
                      command counts as finished, default 400
  --no-pager          (run) report a --More-- pager instead of answering it
  --keep-prompt       (run) keep a prompt that the device ended with a newline
  --json              print the raw result instead of a human summary
  --endpoint <file>   read the socket and token from this endpoint file
  --socket <path>     the server's Unix socket, if not discovered
  --token-file <path> the file holding the handshake token
  --debug             trace the socket traffic on stderr (and, for run, why
                      it decided the command had finished)
  -h, --help          this text

With no server running, the socket only exists while a PuTTY session has
"Session -> Automation (MCP)" turned on.
`;

function parseArgs(argv) {
    const opts = {};
    const rest = [];
    for (let i = 0; i < argv.length; i++) {
        const a = argv[i];
        const need = () => {
            if (i + 1 >= argv.length)
                throw new McpError(`${a} needs a value`);
            return argv[++i];
        };
        if (a === '--since') opts.since = Number(need());
        else if (a === '--max-bytes') opts.maxBytes = Number(need());
        else if (a === '--text') opts.text = need();
        else if (a === '--expect') opts.expect = need();
        else if (a === '--timeout') opts.timeoutMs = Number(need());
        else if (a === '--quiet') opts.quietMs = Number(need());
        else if (a === '--no-pager') opts.pager = false;
        else if (a === '--keep-prompt') opts.keepPrompt = true;
        else if (a === '--endpoint') opts.endpoint = need();
        else if (a === '--socket') opts.socket = need();
        else if (a === '--token-file') opts.tokenFile = need();
        else if (a === '--json') opts.json = true;
        else if (a === '--debug') opts.debug = true;
        else if (a === '-h' || a === '--help') opts.help = true;
        else if (a.startsWith('-')) throw new McpError(`unknown option ${a}`);
        else rest.push(a);
    }
    return { opts, rest };
}

function sessionId(word) {
    const id = Number(word);
    if (!Number.isInteger(id) || id <= 0)
        throw new McpError(`"${word}" is not a session id. Run "putty-mcp list".`);
    return id;
}

/*
 * What to say when nothing answered. The two failures look identical to a
 * caller but have completely different causes, so they are worth separating:
 * either no server is running at all (no endpoint file anywhere), or an
 * endpoint is there and nobody is listening - which is what a PuTTY that was
 * killed rather than closed leaves behind, since it never got to unlink its
 * socket.
 */
function explain(opts, message) {
    const cands = endpointCandidates(opts);

    if (!cands.length) {
        return '  No endpoint file was found. A socket only exists while a PuTTY\n' +
               '  session has Session -> Automation (MCP) turned on.\n';
    }
    let out = `  Looked in: ${cands.map((c) => c.socket).join(', ')}\n`;
    if (/ECONNREFUSED|ENOENT/.test(message)) {
        out += '  A socket that refuses the connection is usually a PuTTY that was\n' +
               '  killed rather than closed. Its files stay behind; they are\n' +
               '  skipped, and a new session does not need them removed.\n';
    }
    return out;
}

/* ------------------------------------------------------------------ */

async function cmdList(opts) {
    const sessions = await listSessions(opts);
    if (opts.json) {
        process.stdout.write(JSON.stringify(sessions, null, 2) + '\n');
        return 0;
    }
    if (!sessions.length) {
        process.stdout.write(
            'No sessions are offered to MCP.\n' +
            'Open one in PuTTY, and turn on Session -> Automation (MCP) ->\n' +
            '"Offer this session to an MCP client", then reopen it.\n');
        return 0;
    }
    const width = Math.max(2, ...sessions.map((s) => String(s.id).length));
    process.stdout.write('ID'.padEnd(width) + '  MODE  NAME\n');
    for (const s of sessions) {
        process.stdout.write(String(s.id).padEnd(width) + '  ' +
            (s.writable ? 'rw' : 'ro') + '    ' + s.name + '\n');
    }
    /*
     * Ids are large and look arbitrary, so say where they come from. There can
     * be several PuTTYs offering sessions - the plain `putty` and `pterm`
     * binaries run one process per window - and each numbers its own from a
     * base derived from its pid, so that ids stay unique across all of them.
     */
    const pids = new Set(sessions.map((s) => s.pid).filter(Boolean));
    if (pids.size > 1) {
        process.stderr.write(
            `(${sessions.length} sessions from ${pids.size} PuTTY processes; ` +
            `the id's leading digits are the process id)\n`);
    }
    return 0;
}

async function cmdRead(opts, args) {
    const session = sessionId(args[0]);
    const r = await readSession({ session, since: opts.since, maxBytes: opts.maxBytes }, opts);

    if (opts.json) {
        process.stdout.write(JSON.stringify(r, null, 2) + '\n');
        return 0;
    }

    if (r.gap) {
        process.stderr.write(
            `warning: lines ${r.gap.missing_from}..${r.gap.missing_to} were ` +
            'dropped from the buffer before this read\n');
    }
    for (const l of r.lines) {
        const tag = l.author === 'd' ? '' : `[${l.author}] `;
        process.stdout.write(`${String(l.id).padStart(6)}  ${tag}${l.text}\n`);
    }
    process.stderr.write(
        `cursor ${r.cursor}, ${r.lines.length} line(s)` +
        (r.omitted ? `, ${r.omitted} still to read` : '') +
        `, total ${r.total_lines}\n`);
    return 0;
}

async function cmdScreen(opts, args) {
    const session = sessionId(args[0]);
    const s = await screenSession({ session }, opts);

    if (opts.json) {
        process.stdout.write(JSON.stringify(s, null, 2) + '\n');
        return 0;
    }
    process.stdout.write(s.text);
    if (!s.text.endsWith('\n'))
        process.stdout.write('\n');
    process.stderr.write(`(${s.cols}x${s.rows})\n`);
    return 0;
}

/*
 * Typing, from the command line. Text is taken as one argument so that a
 * multi-word command does not need quoting twice:
 *
 *   putty-mcp send 1 ls -la
 *   putty-mcp send 1 --text $'ls -la\n'
 *   putty-mcp key 1 enter
 */
async function cmdSend(opts, args) {
    const session = sessionId(args[0]);
    const text = opts.text !== undefined ? opts.text : args.slice(1).join(' ');
    if (!text)
        throw new McpError('send needs some text (or --text)');
    const r = await sendToSession({ session, text }, opts);
    process.stdout.write(`sent ${r.sent} byte(s)\n`);
    return 0;
}

async function cmdKey(opts, args) {
    const session = sessionId(args[0]);
    if (!args[1])
        throw new McpError('key needs a key name (enter, tab, esc, ' +
                           'backspace, up, down, left, right, ctrl-c, ...)');
    await sendKey({ session, key: args[1] }, opts);
    process.stdout.write(`sent ${args[1]}\n`);
    return 0;
}

/*
 * `run`: send one command, wait for it to finish, print its output. This is
 * the command to use when a human wants to know whether prompt detection is
 * working on a particular device, and `--debug` says why it decided what it
 * decided.
 */
async function cmdRun(opts, args) {
    const session = sessionId(args[0]);
    const command = opts.text !== undefined ? opts.text : args.slice(1).join(' ');
    if (!command)
        throw new McpError('run needs a command');

    const r = await runCommand({
        session,
        command,
        expect: opts.expect,
        timeoutMs: opts.timeoutMs,
        quietMs: opts.quietMs,
        maxBytes: opts.maxBytes,
        answerPager: opts.pager,
        keepTrailingPrompt: opts.keepPrompt,
    }, opts);

    if (opts.json) {
        process.stdout.write(JSON.stringify(r, null, 2) + '\n');
        return 0;
    }

    if (r.output)
        process.stdout.write(r.output + '\n');

    process.stderr.write(
        `(${r.completed_by}` +
        (r.echo ? ', echo seen' : ', no echo') +
        (r.paged_answers ? `, ${r.paged_answers} page(s) answered` : '') +
        (r.needs_input ? `, wants ${r.needs_input.reason}` : '') +
        (r.cancelled_by_human ? ', a human typed' : '') +
        (r.truncated ? ', output truncated' : '') +
        `, ${r.elapsed_ms}ms, cursor ${r.cursor})\n`);

    if (r.needs_input) {
        process.stderr.write(
            `        the session is waiting: ${r.needs_input.reason}\n` +
            `        ${JSON.stringify(r.needs_input.prompt)}\n`);
    }
    return 0;
}

async function cmdVersion(opts) {
    const v = await serverVersion(opts);
    process.stdout.write(
        `putty-mcp  ${VERSION}\n` +
        `putty      ${v.version}\n` +
        `protocol   ${v.protocol}\n`);
    return 0;
}

/* ------------------------------------------------------------------ */

export async function main(argv) {
    const { opts, rest } = parseArgs(argv);
    const cmd = rest.shift() ?? 'help';

    if (opts.help || cmd === 'help')
        { process.stdout.write(USAGE); return 0; }

    if (cmd === 'mcp') {
        /*
         * Returns undefined on purpose: the caller must not exit, because the
         * server lives as long as its stdin does.
         */
        await runServer(opts);
        return undefined;
    }

    if (cmd === 'manual') {
        process.stdout.write(userManual());
        if (!userManual().endsWith('\n'))
            process.stdout.write('\n');
        process.stderr.write(`(${manualPath()})\n`);
        return 0;
    }

    try {
        switch (cmd) {
          case 'list':
          case 'sessions':
            return await cmdList(opts);
          case 'read':
            return await cmdRead(opts, rest);
          case 'screen':
            return await cmdScreen(opts, rest);
          case 'send':
            return await cmdSend(opts, rest);
          case 'run':
            return await cmdRun(opts, rest);
          case 'key':
            return await cmdKey(opts, rest);
          case 'version':
            return await cmdVersion(opts);
          case 'ping':
            await ping(opts);
            process.stdout.write('ok\n');
            return 0;
          default:
            process.stderr.write(`putty-mcp: unknown command "${cmd}"\n\n` + USAGE);
            return 2;
        }
    } catch (e) {
        if (e instanceof McpError) {
            process.stderr.write(`putty-mcp: ${e.message}\n`);
            process.stderr.write(explain(opts, e.message));
            return 1;
        }
        throw e;
    }
}
