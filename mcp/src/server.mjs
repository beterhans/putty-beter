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
    serverVersion,
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
            '[{id, name}]. Every other tool takes one of these ids as ' +
            '`session`. There is no "current session": never assume an id, ' +
            'always take one from here.',
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
            'Pages rather than truncates: check `omitted` and read again.',
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
