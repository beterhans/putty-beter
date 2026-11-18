/*
 * `run`: send one command, wait for it to finish, return its output.
 *
 * This is the part of MCP that makes the feature usable rather than merely
 * possible. Without it a caller has to send, sleep, read, guess, and sleep
 * again - and it guesses wrong at a `--More--` pager, which hangs forever.
 *
 * It lives in the bridge rather than in the C server, and that is a deliberate
 * reading of the architecture in PLAN.md section 7: the socket protocol is
 * deliberately dumb, and everything that is a *decision* - what counts as
 * finished, what counts as waiting - belongs on this side, where it can be
 * tested from a table of strings with no PuTTY, no display and no network.
 * The C side supplies only the two primitives it already had: `read` and
 * `send`.
 *
 * The wait is a poll loop, and the loop is the point:
 *
 *   1. remember where the transcript was, and send the command and its Return
 *      in ONE request (two requests leave a gap for the echo to land in);
 *   2. wait for our own line to appear, which is the fence: everything before
 *      it is the prompt we interrupted, everything after it is this command's
 *      output;
 *   3. answer a pager, or stop and report a prompt that wants input;
 *   4. when the device has been quiet for `quiet_ms`, the command is finished -
 *      `completed_by: "prompt"` if the last line also looks like a prompt,
 *      `"quiet"` if only the quiet agrees.
 *
 * Waiting for quiet *before* the fence is what causes premature completion, so
 * the fence is required first. PLAN.md section 7.5.
 */

import {
    classify, compileExpect, compilePatterns, promptReason,
} from './detect.mjs';

export const DEFAULTS = {
    /* How long the device must be silent before a command counts as finished. */
    quietMs: 400,
    /* How often to look. Twenty reads a second, each a tiny local socket call. */
    pollMs: 50,
    /* Give up after this long and hand back what there is. */
    timeoutMs: 30000,
    /* Budget for one read; paging is handled, so this only affects chunking. */
    maxBytes: 8192,
    /* Total output to keep, so `show running-config` cannot exhaust anything. */
    maxOutputBytes: 256 * 1024,
    /* Answer `--More--` and friends with a space. */
    answerPager: true,
    maxPagerAnswers: 500,
    /* Return is CR: that is what the keyboard sends and what a console wants. */
    terminator: '\r',
    /* Drop a trailing prompt that the device terminated with a newline. */
    keepTrailingPrompt: false,
};

function byteLength(lines) {
    let n = 0;
    for (const l of lines)
        n += l.text.length;
    return n;
}

/*
 * Is this device line the device's echo of the command?
 *
 * An exact match is the normal case. The suffix test is for a command wider
 * than the terminal, and the reason is not guesswork - it was measured:
 *
 *   A shell knows how wide the terminal is and emits a **bare carriage return**
 *   at every visual wrap, so that its own cursor bookkeeping stays right.
 *   Measured with a 305-character command in a 128-column terminal: bare CRs
 *   arrive after 114 and 244 bytes, then CR LF at the end.
 *
 * The capture buffer reads a bare CR as "back to column 0, so this line is
 * being redrawn" - which is exactly what keeps a progress bar to one line
 * instead of one line per redraw, and is pinned by the buffer's own tests.
 * Applied to a wrapped echo it discards every row but the last, so what reaches
 * us is the *tail* of the command rather than the whole of it.
 *
 * Hence a suffix, not a prefix. The guards exist so that real output is not
 * mistaken for an echo: only the first line after our own is considered at
 * all, the fragment must be long enough to be a row of a wrapped line rather
 * than a word, and the command must be long enough to have wrapped.
 */
const WRAP_FRAGMENT_MIN = 24;
const WRAP_COMMAND_MIN = 80;

function isEcho(line, command) {
    if (!line || line.author !== 'd' || !line.text)
        return false;
    if (line.text === command)
        return true;
    return command.length >= WRAP_COMMAND_MIN &&
        line.text.length >= WRAP_FRAGMENT_MIN &&
        command.endsWith(line.text);
}

/*
 * Send `command` to a session and wait for it to finish.
 *
 * `session` is anything with `read({since, maxBytes})`, `send(text)` and
 * `key(name)`; `protocol.mjs` builds one from a single connection so the whole
 * run shares it. `options.now` and `options.sleep` exist so the tests can run
 * a thirty-second timeout in no time at all.
 */
export async function runOnSession(session, command, options = {}) {
    /*
     * An option that is explicitly `undefined` must not override its default.
     *
     * This is not a nicety. A caller that forwards its own optional arguments
     * - `{ timeoutMs: opts.timeoutMs }` - passes `undefined` for everything the
     * user did not mention, and a plain spread copies that `undefined` over the
     * default. The result is not an error but a silence: `quiet >= undefined`
     * is never true, so the loop never decides the command has finished, and
     * `elapsed >= undefined` disables the timeout that would have rescued it.
     * The run then hangs until something outside kills it.
     */
    const o = { ...DEFAULTS };
    for (const [k, v] of Object.entries(options)) {
        if (v !== undefined)
            o[k] = v;
    }

    /*
     * And a number that is not a positive number is the same trap wearing a
     * different hat: `Number("abc")` is NaN, and every comparison against NaN
     * is false. These come from a command line or from an agent, so they are
     * checked rather than trusted.
     */
    for (const k of ['quietMs', 'pollMs', 'timeoutMs', 'maxBytes', 'maxOutputBytes']) {
        if (typeof o[k] !== 'number' || !Number.isFinite(o[k]) || o[k] <= 0) {
            throw new Error(`${k} must be a positive number (got ${JSON.stringify(o[k])})`);
        }
    }

    const now = o.now ?? (() => Date.now());
    const sleep = o.sleep ?? ((ms) => new Promise((r) => setTimeout(r, ms)));
    const log = o.log ?? (() => {});

    if (typeof command !== 'string' || command.trim() === '')
        throw new Error('run needs a command to send');
    if (/[\r\n]/.test(command))
        throw new Error('run sends one line, so the command must not contain ' +
                        'a newline (use send for that)');

    const expect = compileExpect(o.expect);
    const pagerPatterns = compilePatterns(o.pagerPatterns, 'pager_patterns');
    const inputPatterns = compilePatterns(o.inputPatterns, 'input_patterns');

    const started = now();

    /*
     * Where the transcript was before we touched it. `total_lines` is the id
     * of the last completed line, so it doubles as the cursor - ids are
     * absolute and never reused.
     */
    const before = await session.read({ since: 0, maxBytes: 1 });
    let cursor = before.total_lines;
    let totalLines = before.total_lines;
    log(`baseline: total_lines=${cursor}`);

    /*
     * One request, not two. The device echoes what we type, and the echo
     * arrives between two requests - landing *inside* our own line, so the
     * transcript reads "pwd pwd" and the fence is unusable. One request closes
     * the line before any echo can be seen.
     */
    await session.send(command + o.terminator);
    log(`sent: ${JSON.stringify(command)}`);

    const lines = [];
    const seen = new Set();
    let bytes = 0;
    let tail = null;
    let ownLineId = null;
    let lastActivity = now();
    let pagedAnswers = 0;
    let gap = null;
    let truncated = false;
    let trailingPromptRemoved = false;
    let needsInput = null;
    let cancelled = false;
    let completedBy = null;

    /*
     * Take one read result into the running picture. Returns whether anything
     * moved, which is what the quiet period is measured against.
     */
    const absorb = (r) => {
        let grew = false;

        if (r.gap && !gap)
            gap = r.gap;
        if (r.total_lines > totalLines)
            totalLines = r.total_lines;

        for (const l of r.lines) {
            if (l.id > r.total_lines)
                continue;                     /* the line in progress */
            if (seen.has(l.id))
                continue;
            seen.add(l.id);
            if (bytes + l.text.length > o.maxOutputBytes) {
                truncated = true;             /* seen, but not kept */
                continue;
            }
            bytes += l.text.length;
            lines.push(l);
            grew = true;
        }

        /*
         * The line in progress is the prompt, or a `--More--`. It is
         * re-sent by every read, and it always has the id after the last
         * completed line - which is how it is told apart from a completed line
         * that paging has not reached yet.
         */
        const t = r.lines.find((l) => l.id === r.total_lines + 1) ?? null;
        if ((t?.text ?? null) !== (tail?.text ?? null)) {
            tail = t;
            grew = true;
        } else {
            tail = t;
        }

        return grew;
    };

    /* Read until the read says nothing was left out. */
    const drain = async () => {
        let grew = false;
        for (;;) {
            const r = await session.read({ since: cursor, maxBytes: o.maxBytes });
            if (absorb(r))
                grew = true;
            cursor = r.cursor;
            if (r.omitted === 0)
                break;
        }
        return grew;
    };

    const humanSince = () => {
        if (ownLineId === null)
            return false;
        for (const l of lines) {
            if (l.id > ownLineId && l.author === 'h')
                return true;
        }
        return tail != null && tail.author === 'h' && tail.id > ownLineId;
    };

    for (;;) {
        await sleep(o.pollMs);
        if (await drain())
            lastActivity = now();

        if (ownLineId === null) {
            /* The fence: our own command line, which is the last 'm' line. */
            for (let i = lines.length - 1; i >= 0; i--) {
                if (lines[i].author === 'm') {
                    ownLineId = lines[i].id;
                    log(`fence: our command is line ${ownLineId}`);
                    break;
                }
            }
        }

        const t = tail?.text ?? '';
        const cls = classify(t, {
            expect: o.expect,
            pagerPatterns,
            inputPatterns,
        });

        /*
         * A human typing cancels the run. The human and the agent must not
         * drive the same stream at once (PLAN.md section 7.9), and the value
         * of stopping here is that the caller is told, rather than carrying on
         * and sending the next command on top of whatever the human is doing.
         *
         * Only lines *after* our own count. The human's half-typed line, which
         * our own send closes, is finalised before our command and would
         * otherwise look like an interruption at the first poll.
         */
        if (humanSince()) {
            cancelled = true;
            completedBy = 'cancelled';
            log('cancelled: a human typed into the session');
            break;
        }

        if (cls.kind === 'pager') {
            if (!o.answerPager) {
                needsInput = { reason: cls.reason, prompt: t };
                completedBy = 'needs_input';
                break;
            }
            if (pagedAnswers >= o.maxPagerAnswers) {
                completedBy = 'pager_limit';
                break;
            }
            await session.key('space');
            pagedAnswers++;
            lastActivity = now();
            log(`pager ${pagedAnswers}: ${cls.reason}`);
            continue;
        }

        if (cls.kind === 'input') {
            needsInput = { reason: cls.reason, prompt: t };
            completedBy = 'needs_input';
            log(`needs input: ${cls.reason} at ${JSON.stringify(t)}`);
            break;
        }

        const quiet = now() - lastActivity;
        if (ownLineId !== null && quiet >= o.quietMs) {
            /*
             * Quiet on its own is not enough, and the reason is the weakness
             * PLAN.md section 7.5 names for this signal: a slow command is
             * silent in the middle. `sleep 1; echo done` prints nothing for a
             * second, and a bare quiet period would declare it finished with no
             * output at all.
             *
             * So there has to be something to judge. A prompt is the strongest
             * thing, whether it is still growing (the usual case - no newline
             * after it) or arrived complete. An unfinished line that is not
             * prompt-shaped is the weaker answer, and is reported as "quiet".
             * With neither, the command simply has not printed anything yet:
             * keep waiting rather than guessing.
             */
            const lastDone = lines.length ? lines[lines.length - 1] : null;
            const lastIsPrompt = lastDone && lastDone.author === 'd' &&
                promptReason(lastDone.text, expect) !== null;

            if (cls.kind === 'prompt' || (!tail && lastIsPrompt)) {
                completedBy = 'prompt';
                log(`done by prompt: quiet ${quiet}ms, last line ` +
                    `${JSON.stringify(t)}`);
                break;
            }
            if (tail) {
                completedBy = 'quiet';
                log(`done by quiet: quiet ${quiet}ms, last line ` +
                    `${JSON.stringify(t)}`);
                break;
            }
            log(`quiet ${quiet}ms but nothing to judge yet; still waiting`);
        }

        if (now() - started >= o.timeoutMs) {
            completedBy = 'timeout';
            log(`timeout after ${o.timeoutMs}ms`);
            break;
        }
    }

    /*
     * Everything after our own line is this command's output - minus the
     * device's echo of the command itself, which we already know because we
     * sent it, and minus a prompt the device terminated with a newline.
     */
    let body = ownLineId === null ? [] : lines.filter((l) => l.id > ownLineId);
    let echo = false;
    if (body.length && isEcho(body[0], command)) {
        echo = true;
        body = body.slice(1);
    }
    if (!o.keepTrailingPrompt && body.length && tail == null) {
        const last = body[body.length - 1];
        if (last.author === 'd' && promptReason(last.text, expect)) {
            body = body.slice(0, -1);
            trailingPromptRemoved = true;
        }
    }

    const result = {
        command,
        output: body.map((l) => l.text).join('\n'),
        lines: body.map((l) => ({ id: l.id, author: l.author, text: l.text })),
        cursor,
        total_lines: totalLines,
        completed_by: completedBy,
        echo,
        paged_answers: pagedAnswers,
        cancelled_by_human: cancelled,
        elapsed_ms: now() - started,
        truncated,
    };
    if (needsInput)
        result.needs_input = needsInput;
    if (trailingPromptRemoved)
        result.trailing_prompt_removed = true;
    if (gap)
        result.gap = gap;
    if (truncated)
        log(`output capped at ${o.maxOutputBytes} bytes; the cursor has ` +
            `still advanced past everything`);

    return result;
}
