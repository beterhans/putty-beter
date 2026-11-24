/*
 * The user manual, as the agent reads it.
 *
 * The full text lives in USER-MANUAL.md next to this package, so it can be
 * edited without touching any code and read as a plain document. It is served
 * three ways: the `user_manual` tool, the `putty://manual` resource, and (as a
 * pointer to it) the instructions sent in the MCP handshake.
 */

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const MANUAL_PATH = path.join(here, '..', 'USER-MANUAL.md');

let cached;

export function manualPath() {
    return MANUAL_PATH;
}

export function userManual() {
    if (cached === undefined) {
        try {
            cached = fs.readFileSync(MANUAL_PATH, 'utf8');
        } catch (e) {
            cached = '# putty-mcp user manual\n\n' +
                `The manual file could not be read (${MANUAL_PATH}: ${e.message}).\n` +
                'Reinstall the bridge, or read USER-MANUAL.md from the putty-beter source.\n';
        }
    }
    return cached;
}

/*
 * Sent as MCP's `instructions` at initialize, which clients put in front of
 * the model. Deliberately short: it is a pointer, not the manual, so it costs
 * almost nothing and still makes the agent read the manual before acting.
 */
export const INSTRUCTIONS = [
    'putty-mcp reads a live PuTTY session - a shell, a router, a switch or a',
    'serial console - in the window the human is looking at.',
    '',
    'Before using any tool here, call the `user_manual` tool (or read the',
    '`putty://manual` resource) and follow it. It is short, and it explains',
    'the two things that are easy to get wrong: that every tool takes an',
    'explicit session id from `sessions()`, and that `read` returns a cursor',
    'which must be fed back as the next `since`.',
    '',
    'You can read a session, run a command in it, and type into it. Typing only',
    'works if the human turned it on for that session - `sessions()` reports',
    '`writable` - and it is real: a command you send runs on the far end.',
    '',
    'Prefer `run`: it sends one command, waits for the device to finish, and',
    'returns just that command\'s output. It answers a `--More--` pager by',
    'itself, and it stops rather than guessing at a `[confirm]` or a password',
    'prompt - check `completed_by` before you trust the output, and never send',
    'the next command at a `needs_input`.',
    '',
    'A password is the human\'s to give: ask them to type it in the window.',
    'When PuTTY itself is asking them - a login password, a key passphrase, a',
    'login name - the session reports `needs_human` (`sessions`, `read` and',
    '`run` all say so). That is not yours to answer either: tell the human,',
    'wait, and carry on when it clears.',
].join('\n');
