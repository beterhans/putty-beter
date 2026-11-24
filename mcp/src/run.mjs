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
    /*
     * Budget for one read; paging is handled, so this only affects chunking.
     */
    maxBytes: 8192,
    /*
     * Total output to keep. This is what bounds the *whole* command, and it is
     * the number that decides how much of the caller's context one `run` can
     * occupy - 8 KB is roughly 2k tokens.
     *
     * It used to be 256 KB (about 65k tokens), then 32 KB (about 8k): the cap
     * was protecting the socket and this process's memory, not the model, and
     * it was not settable from the MCP tool at all. It is ~2k tokens now, and
     * on purpose: the cap is the caller's *context* budget, and a
     * small-context agent - a local LM-Studio model, say - must not be flooded
     * by one listing. A `find` over a media library used to come back with 65k
     * tokens and nothing in the reply to say so; `truncated` and
     * `omitted_lines` now say so, and the caller can raise this or read the
     * rest from the transcript with the returned cursor.
     */
    maxOutputBytes: 8 * 1024,
    /* Answer `--More--` and friends with a space. */
    answerPager: true,
    maxPagerAnswers: 500,
    /* Return is CR: that is what the keyboard sends and what a console wants. */
    terminator: '\r',
    /* Drop a trailing prompt that the device terminated with a newline. */
    keepTrailingPrompt: false,
};

/*
 * The output cap counts the string the caller receives, which is the lines
 * joined with '\n' - so each line costs its text plus one for the separator.
 */
function outputBytes(lines) {
    let n = 0;
    for (const l of lines)
        n += l.text.length + 1;
    return n;
}

/*
 * When a `run` prints more than max_output_bytes, keep the head and the tail
 * and drop the middle, rather than dropping the tail. For a listing the top is
 * where it starts and the bottom is where the prompt and the newest output
 * are, and both are usually what the caller wanted; the marker line stands
 * where the dropped lines were, so a reader can see the output is not
 * contiguous.
 *
 * The split is 30% head / 70% tail by bytes, the same rule the C buffer uses
 * for a single over-long line (unix/mcp-buffer.c, rl_add_truncated()).
 *
 * Returns { head, tail, omitted }, or null when everything fits.
 */
export const TRUNC_HEAD_FRAC = 0.30;
export const TRUNC_MARKER = '... [output truncated] ...';

function truncate(body, maxOutputBytes) {
    if (outputBytes(body) <= maxOutputBytes)
        return null;

    const inner = Math.max(0, maxOutputBytes - (TRUNC_MARKER.length + 1));
    const headBudget = Math.floor(inner * TRUNC_HEAD_FRAC);

    let hb = 0, i = 0;
    for (; i < body.length; i++) {
        const c = body[i].text.length + 1;
        if (hb + c > headBudget)
            break;
        hb += c;
    }
    /*
     * If not even one whole line fits the head's share, show the first line
     * anyway when it fits the budget at all: the first line of a listing is
     * usually the one that says what the listing is.
     */
    if (i === 0 && body.length) {
        const c = body[0].text.length + 1;
        if (c <= inner) {
            hb = c;
            i = 1;
        }
    }
    const head = body.slice(0, i);

    const tailBudget = Math.max(0, inner - hb);
    const tail = [];
    let tb = 0;
    for (let j = body.length - 1; j >= i; j--) {
        const c = body[j].text.length + 1;
        if (tb + c > tailBudget)
            break;
        tb += c;
        tail.unshift(body[j]);
    }

    return { head, tail, omitted: body.length - head.length - tail.length };
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
     * Phase 4c: if the window is *already* asking the human something, nothing
     * is sent at all. This is the case that matters most - sending here is how
     * an agent types a password at a password prompt - and the transcript
     * alone could not have told us, because the words look exactly like device
     * output. Pinned, not live: once it was prompting before we sent, the run
     * must not go on, whatever the human does while we wait.
     */
    const promptBeforeSend = before.needs_human ?? null;

    /*
     * One request, not two. The device echoes what we type, and the echo
     * arrives between two requests - landing *inside* our own line, so the
     * transcript reads "pwd pwd" and the fence is unusable. One request closes
     * the line before any echo can be seen.
     */
    if (promptBeforeSend === null) {
        await session.send(command + o.terminator);
        log(`sent: ${JSON.stringify(command)}`);
    } else {
        log(`not sending: the window is asking the human ` +
            `${JSON.stringify(promptBeforeSend)}`);
    }

    const lines = [];
    const seen = new Set();
    let tail = null;
    let ownLineId = null;
    let lastActivity = now();
    let pagedAnswers = 0;
    let gap = null;
    let truncated = false;
    let omittedLines = 0;
    let trailingPromptRemoved = false;
    let needsInput = null;
    let needsHuman = null;
    let cancelled = false;
    let completedBy = null;
    /*
     * Phase 4c: non-null while the *window* is asking the person at the
     * keyboard something (see readOn() in protocol.mjs). The last read wins,
     * so an answer clears it.
     */
    let humanPrompt = null;

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
            /*
             * Keep every completed line. The output cap is applied once, at
             * the end, where the head and the tail can both be kept
             * (truncate(), above) instead of dropping the tail. This holds a
             * copy of the transcript window, which the server bounds to about
             * 4 MB anyway, and the lines come from there in the first place.
             */
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
            humanPrompt = r.needs_human ?? null;
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
         * Phase 4c: the *window* is asking the person at the keyboard
         * something - a password, a key passphrase, a login name. The words
         * may well be in the transcript, but what is being awaited is a
         * human, and no agent may answer it: sending the next command here is
         * typing a password at a password prompt. `promptBeforeSend` pins the
         * case where it was already asking before this run sent anything.
         * Checked before the human-typing rule, because the password the human
         * types at that prompt must not read as a cancellation of the run.
         */
        const hp = promptBeforeSend ?? humanPrompt;
        if (hp !== null) {
            needsHuman = {
                reason: 'the window is asking the human',
                prompt: hp,
            };
            completedBy = 'needs_human';
            log(`needs human: ${JSON.stringify(hp)}`);
            break;
        }

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

    /*
     * The cap. Keep the head and the tail and drop the middle. It used to be
     * applied line by line as the output arrived, which kept the head and
     * threw the tail away - the wrong end for a listing, where the prompt and
     * the newest output are at the bottom.
     */
    const cut = truncate(body, o.maxOutputBytes);
    if (cut) {
        truncated = true;
        omittedLines = cut.omitted;
    }
    const shown = cut ? cut.head.concat(cut.tail) : body;

    let outLines;
    if (cut) {
        outLines = cut.head.map((l) => l.text);
        outLines.push(TRUNC_MARKER);
        outLines.push(...cut.tail.map((l) => l.text));
    } else {
        outLines = body.map((l) => l.text);
    }

    const result = {
        command,
        output: outLines.join('\n'),
        lines: shown.map((l) => ({ id: l.id, author: l.author, text: l.text })),
        cursor,
        total_lines: totalLines,
        completed_by: completedBy,
        echo,
        paged_answers: pagedAnswers,
        cancelled_by_human: cancelled,
        elapsed_ms: now() - started,
        truncated,
    };
    /*
     * Only when it happened: `truncated` alone says something was dropped, and
     * the caller needs to know how much before deciding whether to raise
     * max_output_bytes or read the rest with the cursor.
     */
    if (truncated)
        result.omitted_lines = omittedLines;
    if (needsInput)
        result.needs_input = needsInput;
    if (needsHuman)
        result.needs_human = needsHuman;
    if (trailingPromptRemoved)
        result.trailing_prompt_removed = true;
    if (gap)
        result.gap = gap;
    if (truncated)
        log(`output capped at ${o.maxOutputBytes} bytes: ${omittedLines} ` +
            `line(s) dropped, and the cursor has still advanced past them`);

    return result;
}
