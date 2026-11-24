/*
 * The MCP side of putty-mcp: a stdio MCP server that translates tool calls
 * into socket requests to a running PuTTY.
 *
 * All of the MCP and JSON lives here and in this package. The C side speaks a
 * dumb length-prefixed field protocol and knows nothing about any of it, which
 * is what keeps MCP's spec churn out of the fork.
 */

import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { z } from 'zod';

import {
    listSessions, readSession, screenSession, sendToSession, sendKey,
    serverVersion, runCommand,
} from './protocol.mjs';
import { userManual, INSTRUCTIONS } from './manual.mjs';

export const VERSION = '0.1.0';

function text(s) {
    return { content: [{ type: 'text', text: s }] };
}

function json(o) {
    return { content: [{ type: 'text', text: JSON.stringify(o, null, 2) }] };
}

function failed(e) {
    return {
        content: [{ type: 'text', text: `putty-mcp: ${e.message}` }],
        isError: true,
    };
}

export function buildServer(opts = {}) {
    const server = new McpServer(
        { name: 'putty-mcp', version: VERSION },
        { instructions: INSTRUCTIONS },
    );

    /*
     * The manual comes first, on purpose: it is the tool an agent should call
     * before anything else, and its description says so.
     */
    server.registerTool('user_manual', {
        title: 'putty-mcp user manual',
        description:
            'READ THIS FIRST. How to use every putty-mcp tool: what a session ' +
            'is, how absolute line ids and cursors work, what read and screen ' +
            'return, what the errors mean, and what this version cannot do. ' +
            'Returns the whole manual as Markdown. The same text is available ' +
            'as the `putty://manual` resource, which some clients render more ' +
            'readably.',
        inputSchema: {},
    }, async () => text(userManual()));

    server.registerTool('sessions', {
        title: 'List the PuTTY sessions offered to MCP',
        description:
            'The sessions open in PuTTY that are offered to MCP, as ' +
            '[{id, name, writable, needs_human, prompt}]. Every other tool takes ' +
            'one of these ids as `session`. There is no "current session": ' +
            'never assume an id, always take one from here.\n\n' +
            '`needs_human` true means the *window* is asking the person at the ' +
            'keyboard something right now - a password, a key passphrase, a ' +
            'login name - and `prompt` is what it is asking. That is not yours ' +
            'to answer: ask the human to type it in the window and wait.',
        inputSchema: {},
    }, async () => {
        try {
            return json(await listSessions(opts));
        } catch (e) {
            return failed(e);
        }
    });

    server.registerTool('read', {
        title: 'Read a session\'s output',
        description:
            'Read the lines a session has printed whose id is greater than ' +
            '`since`. Returns {cursor, total_lines, omitted, gap?, lines}, ' +
            'where lines are {id, author, text} and author is "d" (device), ' +
            '"h" (human) or "m" (MCP). Pass the returned `cursor` back as the ' +
            'next `since`. The line currently being written - usually the ' +
            'prompt - is re-sent on every read and does not move the cursor. ' +
            'Pages rather than truncates: check `omitted` and read again. ' +
            '`needs_human` is present when the *window* itself is asking the ' +
            'person at the keyboard something; wait for them, do not answer.',
        inputSchema: {
            session: z.number().int().describe('Session id from sessions().'),
            since: z.number().int().optional().describe(
                'Return lines with an id greater than this. Omit for ' +
                'everything the server still holds.'),
            max_bytes: z.number().int().optional().describe(
                'Budget for this call. Default 8192, about 100 lines of ' +
                'typical CLI output.'),
        },
    }, async ({ session, since, max_bytes }) => {
        try {
            return json(await readSession(
                { session, since, maxBytes: max_bytes }, opts));
        } catch (e) {
            return failed(e);
        }
    });

    server.registerTool('run', {
        title: 'Run one command and wait for it to finish',
        description:
            'Type `command` followed by Return, wait for the device to finish, ' +
            'and return just that command\'s output - everything after the ' +
            'device\'s echo of the command, with the trailing prompt removed. ' +
            'This is the tool to prefer over send() for anything you want an ' +
            'answer from.\n\n' +
            'It waits, which send() does not: it answers a `--More--` pager by ' +
            'itself (counted in `paged_answers`), and it stops rather than ' +
            'guessing at a prompt that wants input. Read `completed_by`:\n' +
            '  "prompt"  - quiet, and the last line looks like a prompt: trust it\n' +
            '  "quiet"   - the device stopped talking, but the last line does not\n' +
            '              look like a prompt: probably finished, check the output\n' +
            '  "timeout" - still talking when the timeout expired: the output is\n' +
            '              partial, and the command may still be running\n' +
            '  "needs_input" - see `needs_input`, and do NOT send the next\n' +
            '              command: answer it deliberately, or ask the human to\n' +
            '  "needs_human" - the *window* is asking the person at the keyboard\n' +
            '              (a password, a key passphrase, a login name), which is\n' +
            '              not yours to answer: see `needs_human`, and wait for the\n' +
            '              human to type it in this window before going on\n' +
            '  "cancelled" - a human typed into the window; the run stopped so\n' +
            '              the two of you do not drive the same stream at once\n' +
            '  "pager_limit" - too many pages; raise max_pager_answers or use\n' +
            '              a command that does not page\n\n' +
            'A password is never yours to give: if `needs_input` says a password ' +
            'is wanted, ask the human to type it in the window. If the session ' +
            'reports `needs_human`, that is exactly what is already happening.',
        inputSchema: {
            session: z.number().int().describe('Session id from sessions().'),
            command: z.string().describe(
                'One line, without a newline. Return is added for you.'),
            expect: z.string().optional().describe(
                'Regular expression for the prompt, if the default family ' +
                'does not fit this device. Replaces the default, not adds to it.'),
            timeout_ms: z.number().int().optional().describe(
                'Give up after this long. Default 30000.'),
            quiet_ms: z.number().int().optional().describe(
                'How long the device must be silent before the command counts ' +
                'as finished. Default 400. Raise it for a slow device.'),
            max_bytes: z.number().int().optional().describe(
                'Budget for one read while polling. Default 8192.'),
            max_output_bytes: z.number().int().optional().describe(
                'Cap on the total output kept for this command, in bytes. ' +
                'Default 8192, about 2k tokens - this is the number that ' +
                'decides how much of your context one run can occupy, so ' +
                'raise it deliberately for a big listing. When it is hit, ' +
                '`truncated` is true and `omitted_lines` says how many lines ' +
                'were dropped; the head and the tail are kept and the middle ' +
                'goes, and the cursor has still advanced past everything, so ' +
                'read() can fetch the rest.'),
            answer_pager: z.boolean().optional().describe(
                'Answer `--More--` with a space. Default true. Set false to be ' +
                'told about the pager instead of having it answered.'),
            pager_patterns: z.array(z.string()).optional().describe(
                'Extra pager patterns, e.g. "^:$" for less/more.'),
            keep_trailing_prompt: z.boolean().optional().describe(
                'Keep a prompt the device terminated with a newline. Default ' +
                'false: it is removed, because it is not output.'),
        },
    }, async (args) => {
        try {
            return json(await runCommand({
                session: args.session,
                command: args.command,
                expect: args.expect,
                timeoutMs: args.timeout_ms,
                quietMs: args.quiet_ms,
                maxBytes: args.max_bytes,
                maxOutputBytes: args.max_output_bytes,
                answerPager: args.answer_pager,
                pagerPatterns: args.pager_patterns,
                keepTrailingPrompt: args.keep_trailing_prompt,
            }, opts));
        } catch (e) {
            return failed(e);
        }
    });

    server.registerTool('screen', {
        title: 'Read the visible screen',
        description:
            'The visible screen only, as text, plus the terminal size in ' +
            'cols and rows. This is "what the human can see right now", ' +
            'which is a different question from read(): read() is a ' +
            'transcript, screen() is a photograph.',
        inputSchema: {
            session: z.number().int().describe('Session id from sessions().'),
        },
    }, async ({ session }) => {
        try {
            return json(await screenSession({ session }, opts));
        } catch (e) {
            return failed(e);
        }
    });

    server.registerTool('send', {
        title: 'Type text into a session',
        description:
            'Type `text` into the session, exactly as if it had been typed at ' +
            'the keyboard: it goes through the line discipline, so local echo ' +
            'and line editing behave normally. It does NOT wait for anything ' +
            'and does not add a newline - include "\\n" to press Return. ' +
            'Follow it with read() to see what came back. Only works when the ' +
            'human ticked "Also allow an MCP client to type into this ' +
            'session"; sessions() reports `writable`, and a read-only session ' +
            'returns an error rather than pretending. Refused during a ZMODEM ' +
            'transfer.',
        inputSchema: {
            session: z.number().int().describe('Session id from sessions().'),
            text: z.string().describe(
                'The text to type. Use "\\n" for Return, or the key tool.'),
        },
    }, async ({ session, text }) => {
        try {
            return json(await sendToSession({ session, text }, opts));
        } catch (e) {
            return failed(e);
        }
    });

    server.registerTool('key', {
        title: 'Send one keystroke to a session',
        description:
            'Send a single named keystroke: enter, tab, esc, space, backspace, ' +
            'delete, up, down, left, right, home, end, pageup, pagedown, or ' +
            'ctrl-a .. ctrl-z. Use it for the keys that are not text - ' +
            'answering a pager, or interrupting a command with ctrl-c. Same ' +
            'rules as send(): needs a writable session, and is refused during ' +
            'a ZMODEM transfer.',
        inputSchema: {
            session: z.number().int().describe('Session id from sessions().'),
            key: z.string().describe('e.g. "enter", "ctrl-c", "up".'),
        },
    }, async ({ session, key }) => {
        try {
            return json(await sendKey({ session, key }, opts));
        } catch (e) {
            return failed(e);
        }
    });

    server.registerTool('version', {
        title: 'PuTTY and protocol versions',
        description: 'The PuTTY build the session is running, and the ' +
            'socket protocol version in use.',
        inputSchema: {},
    }, async () => {
        try {
            return json(await serverVersion(opts));
        } catch (e) {
            return failed(e);
        }
    });

    server.registerResource('manual', 'putty://manual', {
        title: 'putty-mcp user manual',
        description: 'How to use the putty-mcp tools. The same text as the ' +
            'user_manual tool.',
        mimeType: 'text/markdown',
    }, async (uri) => ({
        contents: [{
            uri: uri.href,
            mimeType: 'text/markdown',
            text: userManual(),
        }],
    }));

    server.registerResource('sessions', 'putty://sessions', {
        title: 'PuTTY sessions offered to MCP',
        description: 'The same JSON as the sessions tool.',
        mimeType: 'application/json',
    }, async (uri) => {
        let body;
        try {
            body = JSON.stringify(await listSessions(opts), null, 2);
        } catch (e) {
            body = JSON.stringify({ error: e.message }, null, 2);
        }
        return {
            contents: [{ uri: uri.href, mimeType: 'application/json', text: body }],
        };
    });

    return server;
}

export async function runServer(opts = {}) {
    const server = buildServer(opts);
    const transport = new StdioServerTransport();
    await server.connect(transport);
    return server;
}
