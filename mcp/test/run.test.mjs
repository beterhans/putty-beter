/*
 * Tests for prompt/pager/input detection (mcp/src/detect.mjs) and the run loop
 * (mcp/src/run.mjs).
 *
 * No PuTTY, no socket and no display, and no waiting either: the session is a
 * small stand-in with a virtual clock, so a thirty-second timeout is tested in
 * a microsecond and the result is the same every time. What this pins is the
 * *decisions* - when a command counts as finished, what a pager looks like,
 * what must not be answered. That the real bytes look the same coming out of a
 * real device is what scripts/test-mcp.sh is for.
 *
 *   node mcp/test/run.test.mjs
 */

import { classify, compileExpect, promptReason } from '../src/detect.mjs';
import { runOnSession } from '../src/run.mjs';

let passes = 0, failures = 0;

function check(cond, what, detail = '') {
    if (cond) {
        passes++;
        console.log(`  ok    ${what}`);
    } else {
        failures++;
        console.log(`  FAIL  ${what}${detail ? `\n        ${detail}` : ''}`);
    }
}

function checkEqual(actual, expected, what) {
    check(actual === expected, what, `expected ${JSON.stringify(expected)}\n` +
                                     `        actual   ${JSON.stringify(actual)}`);
}

/* ------------------------------------------------------------------ */
/* A session with nothing behind it                                    */
/* ------------------------------------------------------------------ */

/*
 * Lines are laid out on a virtual timeline. `{at, author, text}` completes a
 * line at virtual time `at`; `{at, open: true, ...}` makes a line the one in
 * progress from `at` onwards; `erase: true` throws the line in progress away,
 * which is what a device overwriting its own `--More--` does.
 *
 * Ids, the cursor and the line in progress all behave as unix/mcp-buffer.c
 * does: ids are absolute, and the line in progress is reported but does not
 * move the cursor.
 */
class FakeSession {
    constructor(opts = {}) {
        this.opts = opts;
        this.t = 0;
        this.extra = [];
        this.sent = [];
        this.keys = [];
        this.completed = [];
        this.nextId = 1;
        this.openLine = opts.beforeOpen ?? { author: 'd', text: 'Router>' };
        this.pagerPage = 0;

        for (const l of opts.before ?? [])
            this.completed.push({ id: this.nextId++, author: l.author ?? 'd', text: l.text });
    }

    _apply() {
        this.extra.sort((a, b) => a.at - b.at);
        for (;;) {
            const e = this.extra.find((x) => x.at <= this.t);
            if (!e)
                break;
            this.extra.splice(this.extra.indexOf(e), 1);
            if (e.erase) {
                this.openLine = null;
            } else if (e.open) {
                this.openLine = { author: e.author ?? 'd', text: e.text };
            } else {
                if (this.openLine)
                    this.completed.push({ id: this.nextId++, ...this.openLine });
                this.openLine = null;
                this.completed.push({ id: this.nextId++, author: e.author ?? 'd', text: e.text });
            }
        }
    }

    /* Mirrors mcp_buffer_note_sent(): close what is open, then our own line. */
    _noteSent(author, data) {
        if (this.openLine && this.openLine.text.length)
            this.completed.push({ id: this.nextId++, ...this.openLine });
        this.openLine = null;

        let cur = '';
        for (const ch of data) {
            if (ch === '\r' || ch === '\n') {
                if (cur) {
                    this.completed.push({ id: this.nextId++, author, text: cur });
                    cur = '';
                }
            } else {
                cur += ch;
            }
        }
        if (cur)
            this.openLine = { author, text: cur };
    }

    async read({ since = 0, maxBytes = 8192 } = {}) {
        this._apply();
        const total = this.completed.length;
        const lines = [];
        let cursor = since;
        let used = 0;

        for (const l of this.completed) {
            if (l.id <= since)
                continue;
            if (lines.length && used + l.text.length > maxBytes)
                break;
            lines.push({ id: l.id, author: l.author, text: l.text });
            used += l.text.length;
            cursor = l.id;
        }
        if (this.openLine && used + this.openLine.text.length <= maxBytes)
            lines.push({ id: total + 1, author: this.openLine.author, text: this.openLine.text });

        return {
            cursor,
            total_lines: total,
            omitted: total > cursor ? total - cursor : 0,
            lines,
        };
    }

    _emitPage() {
        const p = this.opts.pager;
        const page = p.pages[this.pagerPage];
        for (const text of page)
            this.extra.push({ at: this.t + 10, author: 'd', text });
        if (this.pagerPage < p.pages.length - 1)
            this.extra.push({ at: this.t + 10, open: true, author: 'd', text: '--More--' });
        else
            this.extra.push({ at: this.t + 10, open: true, author: 'd', text: p.tail ?? 'Router>' });
    }

    async send(text) {
        this.sent.push(text);
        this._noteSent('m', text);
        /*
         * The echo comes back before the command's output does - it is the
         * terminal echoing what was typed, so it happens on the way in. Getting
         * this order wrong is not a small thing: the echo closes the line in
         * progress, so a pager marker that arrived before it would be closed
         * too, and the run would never see the pager.
         */
        if (this.opts.echo !== false)
            this.extra.push({ at: this.t + 2, author: 'd', text: text.replace(/[\r\n]/g, '') });
        if (this.opts.pager) {
            this.pagerPage = 0;
            this._emitPage();
        }
        for (const s of this.opts.steps ?? [])
            this.extra.push({ at: this.t + s.afterMs, ...s });
        return text.length;
    }

    async key(name) {
        this.keys.push(name);
        if (name === 'space' && this.opts.pager && this.pagerPage < this.opts.pager.pages.length) {
            /* The device overwrites its own --More-- before printing the page. */
            this.extra.push({ at: this.t + 1, erase: true });
            this.pagerPage++;
            this._emitPage();
        }
        return name;
    }
}

/* A run against a fake session, on a virtual clock. */
function run(session, command, options = {}) {
    return runOnSession(session, command, {
        now: () => session.t,
        sleep: (ms) => { session.t += ms; return Promise.resolve(); },
        ...options,
    });
}

/* ------------------------------------------------------------------ */
/* Detection                                                           */
/* ------------------------------------------------------------------ */

function testDetection() {
    const prompt = (t, opts) => classify(t, opts).kind === 'prompt';

    for (const p of ['Router>', 'Router#', 'Router(config-if)#', 'switch-01#',
                     'hans@MacMini2018 ~ %', 'user@host:~$', '[root@host ~]#',
                     'C:\\Users\\hans>'])
        check(prompt(p), `prompt: ${JSON.stringify(p)} looks like a prompt`);

    for (const n of ['total 12', 'hello world', 'line one',
                     '  -rw-r--r--  1 hans  staff  0 Nov 18 x',
                     'Reading package lists...'])
        check(!prompt(n), `prompt: ${JSON.stringify(n)} does not look like a prompt`);

    /* A progress bar is the thing that makes a run finish early if it is
     * mistaken for a prompt, so it is excluded on purpose. */
    check(!prompt('100%'), 'prompt: a progress percentage is not a prompt');
    check(!prompt('  42%'), 'prompt: a padded percentage is not a prompt');

    /*
     * The order is the whole point: `--More--(50%)` ends in `%` and would
     * otherwise be read as a prompt, and `Password:` is a word then a colon.
     */
    checkEqual(classify('--More--(50%)').kind, 'pager',
               'pager: --More--(50%) is a pager, not a prompt');
    checkEqual(classify('--More--').kind, 'pager', 'pager: --More--');
    checkEqual(classify('--MORE--').kind, 'pager', 'pager: --MORE-- (case)');
    checkEqual(classify('(END)').kind, 'pager', 'pager: less (END)');
    checkEqual(classify('More?').kind, 'pager', 'pager: More?');

    /*
     * `:` is the prompt `more` and `less` use from the second page onwards, and
     * the first-page prompt when they read from a pipe. Measured, not guessed:
     * see the note on PAGER_FILENAME.
     */
    checkEqual(classify(':').kind, 'pager', 'pager: ":" is a pager prompt');
    checkEqual(classify(': ').kind, 'pager', 'pager: ": " is a pager prompt');

    /*
     * Given a *file argument*, more and less put the file name where the prompt
     * goes on the first page, in reverse video - which the capture buffer
     * strips, leaving just the name. Without this the run gives up, the pager
     * keeps waiting, and the next command is eaten by it.
     */
    for (const f of ['/tmp/config.txt', 'big.txt', 'sub/dir/file', '~/notes.md',
                     'config.gz', 'a.c'])
        checkEqual(classify(f).kind, 'pager',
                   `pager: a pager's file name ${JSON.stringify(f)}`);

    /* ...and the pattern must stay narrow enough not to swallow real output. */
    for (const n of ['15.2', '1.2.3', 'v15.2', 'done', 'ready', 'README',
                     'version 15.2', 'Total: 4.2 GB'])
        checkEqual(classify(n).kind, 'none',
                   `pager: ${JSON.stringify(n)} is not a pager`);

    for (const [t, what] of [
        ['Password: ', 'a password prompt'],
        ['password:', 'a password prompt'],
        ['[sudo] password for hans: ', 'a password prompt'],
        ['Enter passphrase for key: ', 'a passphrase prompt'],
        ['[confirm]', 'a [confirm] prompt'],
        ['Are you sure you want to continue? [yes/no]', 'a [yes/no] prompt'],
        ['(yes/no)', 'a (yes/no) prompt'],
        ['[y/n]', 'a [y/n] prompt'],
        ['Are you sure?', 'an "are you sure?" prompt'],
        ['Overwrite?', 'an overwrite prompt'],
        ['login:', 'a login prompt'],
        ['Username: ', 'a username prompt'],
        ['Press any key to continue', 'a "press a key" prompt'],
    ])
        checkEqual(classify(t).reason, what, `input: ${JSON.stringify(t)}`);

    checkEqual(classify('Router>').reason, 'the default prompt family',
               'detect: the prompt family is named in the reason');

    /* A caller who knows their device replaces the default, not adds to it. */
    checkEqual(classify('myprompt!', { expect: '!\\s*$' }).kind, 'prompt',
               'expect: a caller pattern replaces the default family');
    checkEqual(classify('Router>', { expect: '!\\s*$' }).kind, 'none',
               'expect: and the default family is then not consulted');

    checkEqual(classify('Router>', { pagerPatterns: ['^Router>$'] }).kind, 'pager',
               'patterns: a caller can add pager patterns');
    checkEqual(classify('Proceed?', { inputPatterns: ['\\?$'] }).kind, 'input',
               'patterns: a caller can add input patterns');

    let threw = '';
    try { compileExpect('['); } catch (e) { threw = e.message; }
    check(/not a valid regular expression/.test(threw),
          'expect: a bad pattern is refused with a useful message', threw);

    checkEqual(promptReason('Router>', null) !== null, true,
               'detect: promptReason works on its own');
}

/* ------------------------------------------------------------------ */
/* The run loop                                                        */
/* ------------------------------------------------------------------ */

async function testBasicRun() {
    const s = new FakeSession({
        before: [{ text: 'line one' }, { text: 'line two' }],
        steps: [
            { afterMs: 100, text: 'version 15.2' },
            { afterMs: 150, text: 'uptime is 4 days' },
            { afterMs: 300, open: true, text: 'Router>' },
        ],
    });
    const r = await run(s, 'show version');

    checkEqual(r.completed_by, 'prompt', 'run: quiet plus a prompt line');
    checkEqual(r.output, 'version 15.2\nuptime is 4 days',
               'run: returns the output and nothing else');
    checkEqual(r.echo, true, 'run: reports that the device echoed the command');
    checkEqual(r.paged_answers, 0, 'run: no pages');
    checkEqual(r.cancelled_by_human, false, 'run: nobody typed');
    check(r.needs_input === undefined, 'run: nothing was wanted');

    /*
     * One request, not two. Two - the text, then a keystroke - leave a gap in
     * which the device's echo lands inside our own line, and the transcript
     * reads "show version show version".
     */
    checkEqual(s.sent.length, 1, 'run: the command and its Return go in one request');
    checkEqual(s.sent[0], 'show version\r', 'run: and the Return is a CR');

    /* The prompt we interrupted is not part of the answer. */
    check(!r.output.includes('Router>'),
          'run: the prompt we interrupted is not in the output');
    check(!r.output.includes('line two'),
          'run: earlier transcript is not in the output');

    /* The cursor is past the output, so the next read does not repeat it. */
    const after = await s.read({ since: r.cursor });
    check(!after.lines.some((l) => l.text === 'version 15.2'),
          'run: the cursor is past the output it returned');

    /* Why it decided, for a human debugging a device. */
    const said = [];
    const s2 = new FakeSession({ steps: [{ afterMs: 100, open: true, text: 'Router>' }] });
    await run(s2, 'ls', { log: (m) => said.push(m) });
    check(said.some((m) => /done by prompt/.test(m)),
          'run: --debug says why it decided the command had finished');
}

async function testNoEcho() {
    const s = new FakeSession({
        echo: false,
        steps: [
            { afterMs: 100, text: 'just this' },
            { afterMs: 200, open: true, text: 'Router>' },
        ],
    });
    const r = await run(s, 'ls');
    checkEqual(r.echo, false, 'run: a device that does not echo is reported');
    checkEqual(r.output, 'just this',
               'run: and the output is still exactly the output');
    checkEqual(r.completed_by, 'prompt', 'run: completion still works without echo');
}

async function testQuietWithoutPrompt() {
    /* A device that writes a line without a newline and then stops, with
     * nothing prompt-shaped about it: quiet agrees, the shape does not, and
     * the caller is told which. */
    const s = new FakeSession({
        echo: false,
        steps: [{ afterMs: 100, open: true, text: 'done and that is all' }],
    });
    const r = await run(s, 'go');
    checkEqual(r.completed_by, 'quiet', 'run: quiet alone is reported as "quiet"');
    checkEqual(r.output, '', 'run: and the unfinished line is not output');
}

async function testSlowCommand() {
    /*
     * The weakness of the quiet signal: a slow command is silent in the
     * middle. `sleep 1; echo done` prints nothing for a second, and a bare
     * quiet period would call that finished - with no output at all, and the
     * caller told to trust it.
     */
    const s = new FakeSession({
        echo: false,
        steps: [
            { afterMs: 1000, text: 'done' },
            { afterMs: 1100, open: true, text: 'Router>' },
        ],
    });
    const r = await run(s, 'sleep 1; echo done');
    checkEqual(r.output, 'done', 'run: a slow command is waited for, not cut off');
    checkEqual(r.completed_by, 'prompt', 'run: and completes properly afterwards');

    /* With a prompt that arrives as a complete line, quiet is still enough. */
    const s2 = new FakeSession({
        echo: false,
        steps: [{ afterMs: 100, text: 'the output' },
                { afterMs: 150, text: 'Router>' }],
    });
    const r2 = await run(s2, 'ls');
    checkEqual(r2.completed_by, 'prompt',
               'run: a prompt that arrived complete still counts as a prompt');
}

async function testPager() {
    const s = new FakeSession({
        pager: { pages: [['page one a', 'page one b'], ['page two a'], ['page three a']] },
    });
    const r = await run(s, 'show running-config');
    checkEqual(r.paged_answers, 2, 'run: every --More-- was answered');
    checkEqual(s.keys.length, 2, 'run: and answered with a space each time');
    checkEqual(s.keys[0], 'space', 'run: the answer is a space');
    checkEqual(r.completed_by, 'prompt', 'run: the run finishes after the last page');
    for (const t of ['page one a', 'page two a', 'page three a'])
        check(r.output.includes(t), `run: paged output contains ${JSON.stringify(t)}`);

    /* A pager left unanswered is a run that hangs forever, so a caller who
     * wants to see it is told rather than having it answered. */
    const s2 = new FakeSession({ pager: { pages: [['a'], ['b']] } });
    const r2 = await run(s2, 'show run', { answerPager: false });
    checkEqual(r2.completed_by, 'needs_input',
               'run: answer_pager false reports the pager instead');
    checkEqual(r2.needs_input.reason, 'a --More-- pager',
               'run: and names it a pager');
    checkEqual(s2.keys.length, 0, 'run: and sends nothing');

    /* A device that pages for ever is stopped rather than followed. */
    const s3 = new FakeSession({
        pager: { pages: [['a'], ['b'], ['c'], ['d'], ['e']] },
    });
    const r3 = await run(s3, 'show run', { maxPagerAnswers: 2 });
    checkEqual(r3.completed_by, 'pager_limit',
               'run: too many pages stops with pager_limit');
}

async function testNeedsInput() {
    const s = new FakeSession({
        steps: [
            { afterMs: 100, text: 'Password for hans:' },
            { afterMs: 150, open: true, text: 'Password: ' },
        ],
    });
    const r = await run(s, 'sudo ls');
    checkEqual(r.completed_by, 'needs_input', 'run: a password prompt stops the run');
    checkEqual(r.needs_input.reason, 'a password prompt', 'run: and says what it wants');
    checkEqual(r.needs_input.prompt, 'Password: ', 'run: and shows the prompt');
    checkEqual(r.output, 'Password for hans:',
               'run: with the output so far');
    checkEqual(s.keys.length, 0, 'run: and nothing is typed at a password prompt');
    checkEqual(s.sent.length, 1, 'run: and no further command is sent');

    const c = new FakeSession({
        steps: [{ afterMs: 100, open: true, text: 'Are you sure? [confirm]' }],
    });
    const rc = await run(c, 'reload');
    checkEqual(rc.completed_by, 'needs_input',
               'run: a [confirm] stops the run rather than answering it');
    checkEqual(c.sent.length, 1, 'run: nothing is sent at a [confirm]');
}

async function testHumanCancels() {
    /*
     * The important one: a human's half-typed line, which our own send closes,
     * is finalised *before* our command and must not look like an
     * interruption at the first poll.
     */
    const s = new FakeSession({
        before: [{ text: 'line one' }],
        beforeOpen: { author: 'h', text: 'ls -l' },
        steps: [{ afterMs: 100, text: 'output' },
                { afterMs: 200, open: true, text: 'Router>' }],
    });
    const r = await run(s, 'show version');
    checkEqual(r.cancelled_by_human, false,
               'run: the human\'s earlier half-typed line is not an interruption');
    checkEqual(r.completed_by, 'prompt', 'run: so the run completes normally');
    checkEqual(r.output, 'output', 'run: and the human\'s half-typed line is not output');

    /* And the real thing: the human types while the command is running. */
    const s2 = new FakeSession({
        steps: [
            { afterMs: 100, text: 'output' },
            { afterMs: 250, open: true, author: 'h', text: 'q' },
        ],
    });
    const r2 = await run(s2, 'show version');
    checkEqual(r2.cancelled_by_human, true, 'run: a human typing cancels the run');
    checkEqual(r2.completed_by, 'cancelled', 'run: and it is reported as cancelled');

    /* A completed human line counts too, not only the one in progress. */
    const s3 = new FakeSession({
        steps: [
            { afterMs: 100, text: 'output' },
            { afterMs: 250, author: 'h', text: 'ls' },
        ],
    });
    const r3 = await run(s3, 'show version');
    checkEqual(r3.cancelled_by_human, true, 'run: a completed human line cancels too');
}

async function testTimeout() {
    const steps = [];
    for (let i = 0; i < 40; i++)
        steps.push({ afterMs: 100 + i * 100, text: `line ${i}` });
    const s = new FakeSession({ echo: false, steps });
    const r = await run(s, 'ping -c 100', { timeoutMs: 1000 });
    checkEqual(r.completed_by, 'timeout', 'run: a device that never stops times out');
    checkEqual(r.elapsed_ms, 1000, 'run: and the elapsed time is reported');
    check(r.output.includes('line 0'), 'run: with the partial output');
    checkEqual(r.truncated, false, 'run: not truncated, just incomplete');
}

async function testTrailingPrompt() {
    /* Some devices end the prompt with a newline, so it arrives as a completed
     * line. It is not output, and it is removed - but the caller can say no. */
    const s = new FakeSession({
        echo: false,
        steps: [{ afterMs: 100, text: 'the output' },
                { afterMs: 150, text: 'Router>' }],
    });
    const r = await run(s, 'ls');
    checkEqual(r.output, 'the output', 'run: a newline-terminated prompt is removed');
    checkEqual(r.trailing_prompt_removed, true, 'run: and the removal is reported');

    const s2 = new FakeSession({
        echo: false,
        steps: [{ afterMs: 100, text: 'the output' },
                { afterMs: 150, text: 'Router>' }],
    });
    const r2 = await run(s2, 'ls', { keepTrailingPrompt: true });
    checkEqual(r2.output, 'the output\nRouter>', 'run: keep_trailing_prompt keeps it');

    /* A last line that is not prompt-shaped is never removed. */
    const s3 = new FakeSession({
        echo: false,
        steps: [{ afterMs: 100, text: 'last line of output' }],
    });
    const r3 = await run(s3, 'ls');
    checkEqual(r3.output, 'last line of output',
               'run: ordinary output is not mistaken for a prompt');
}

async function testUndefinedOptions() {
    /*
     * The trap a real caller walks into: forwarding optional arguments passes
     * `undefined` for everything the user did not mention, and a plain spread
     * copies that over the default. What follows is not an error but a
     * silence - no completion, and no timeout to rescue it - so the run hangs
     * until something outside kills it. This is exactly what the first real
     * run against a real pty did.
     */
    const s = new FakeSession({
        echo: false,
        steps: [{ afterMs: 100, open: true, text: 'Router>' }],
    });
    const r = await runOnSession(s, 'ls', {
        now: () => s.t,
        sleep: (ms) => { s.t += ms; return Promise.resolve(); },
        expect: undefined,
        timeoutMs: undefined,
        quietMs: undefined,
        maxBytes: undefined,
        answerPager: undefined,
        keepTrailingPrompt: undefined,
        pagerPatterns: undefined,
    });
    checkEqual(r.completed_by, 'prompt',
               'defaults: an explicit undefined does not override a default');

    /* And a number that is not a positive number is the same trap again:
     * every comparison against NaN is false. */
    for (const bad of [NaN, 0, -1, 'abc'])
        check(await run(new FakeSession(), 'ls', { quietMs: bad })
                .then(() => false, (e) => /quietMs must be a positive number/.test(e.message)),
              `defaults: quietMs=${JSON.stringify(bad)} is refused, not silently fatal`);
}

async function testWrappedEcho() {
    /*
     * A command wider than the terminal. The shell emits a bare CR at every
     * visual wrap, the capture buffer reads a bare CR as "back to column 0"
     * (which is what keeps a progress bar to one line), and so only the last
     * row of the echo survives.
     *
     * Measured against a real bash in a 128-column pty with a 305-character
     * command: bare CRs after 114 and 244 bytes. The segments below are those
     * bytes, so this test is the real shape rather than an invented one.
     */
    const command = 'echo ' + '0123456789'.repeat(30);        /* 305 chars */
    const row1 = 'echo ' + '0123456789'.repeat(11);           /* 115 - what the CR discarded */
    const row2 = '0123456789'.repeat(13);                     /* 130 - discarded too */
    const row3 = command.slice(row1.length + row2.length);    /* 60 - the survivor */
    check(row1.length + row2.length + row3.length === command.length,
          'the measured rows add up to the command',
          `${row1.length}+${row2.length}+${row3.length} vs ${command.length}`);

    const s = new FakeSession({
        echo: false,                       /* the rows below *are* the echo */
        before: [{ text: 'hans@S-D220:/$' }],
        steps: [
            { afterMs: 100, author: 'd', text: row3 },        /* the only row that survived */
            { afterMs: 150, author: 'd', text: command.slice(5) },  /* the echo's output */
            { afterMs: 200, open: true, text: 'hans@S-D220:/$' },
        ],
    });
    const r = await run(s, command);

    checkEqual(r.output, command.slice(5),
               'run: a wrapped echo does not leak into the output');
    checkEqual(r.echo, true,
               'run: and the wrapped echo is still recognised as the echo');
    checkEqual(r.completed_by, 'prompt', 'run: and the run completes normally');

    /*
     * The guards. Real output that merely happens to end like the command must
     * not be swallowed - which is what would happen if a bare suffix were
     * enough.
     */
    const s2 = new FakeSession({
        echo: false,
        steps: [
            { afterMs: 100, author: 'd', text: 'some ordinary output' },
            { afterMs: 200, open: true, text: 'Router>' },
        ],
    });
    const r2 = await run(s2, 'echo some ordinary output');
    checkEqual(r2.output, 'some ordinary output',
               'run: short output that happens to end like the command is kept');
    checkEqual(r2.echo, false, 'run: and is not called an echo');

    /* A command long enough to wrap, whose first output line is a long suffix:
     * only the first line after our own is ever considered for the echo, and
     * the echo itself was exact here, so the output survives. */
    const longCmd = 'grep -n ' + 'x'.repeat(90) + ' /var/log/messages';
    const tail = longCmd.slice(9);
    const s3 = new FakeSession({
        steps: [
            { afterMs: 150, author: 'd', text: tail },      /* output that is a suffix */
            { afterMs: 200, open: true, text: 'Router>' },
        ],
    });
    const r3 = await run(s3, longCmd);
    checkEqual(r3.echo, true, 'run: an exact echo is recognised first');
    checkEqual(r3.output, tail,
               'run: so output that looks like a suffix is not eaten as well');
}

async function testBadInput() {
    for (const [cmd, what] of [['', 'an empty command'],
                               ['ls\nls', 'a command containing a newline'],
                               ['ls\r', 'a command containing a carriage return']])
        check(await run(new FakeSession(), cmd).then(() => false, (e) => /one line|needs a command/.test(e.message)),
              `run: ${what} is refused`);

    /* The refusal must happen before anything is typed into the session. */
    const s = new FakeSession();
    await run(s, 'a\nb').catch(() => {});
    checkEqual(s.sent.length, 0, 'run: a refused command is not sent at all');
}

async function testGapAndPaging() {
    /* Output larger than one read's budget is paged, not truncated, and the
     * whole of it comes back. */
    const s = new FakeSession({
        echo: false,
        steps: [
            ...Array.from({ length: 30 }, (_, i) => ({ afterMs: 50, text: `line ${i} `.repeat(20) })),
            { afterMs: 200, open: true, text: 'Router>' },
        ],
    });
    const r = await run(s, 'show run', { maxBytes: 256 });
    checkEqual(r.lines.length, 30, 'run: output larger than one read is paged, not lost');
    checkEqual(r.truncated, false, 'run: and is not reported as truncated');
    checkEqual(r.completed_by, 'prompt', 'run: and the run still completes properly');

    /* A cap on total output is honoured, and said so. */
    const s2 = new FakeSession({
        echo: false,
        steps: [
            ...Array.from({ length: 30 }, (_, i) => ({ afterMs: 50, text: `line ${i} `.repeat(20) })),
            { afterMs: 200, open: true, text: 'Router>' },
        ],
    });
    const r2 = await run(s2, 'show run', { maxBytes: 256, maxOutputBytes: 200 });
    checkEqual(r2.truncated, true, 'run: too much output is truncated and reported');
    check(r2.output.length <= 200, 'run: and the cap is respected',
          `got ${r2.output.length} bytes`);
    checkEqual(r2.completed_by, 'prompt',
               'run: truncation does not stop the run finishing');
}

/* ------------------------------------------------------------------ */

console.log('putty-mcp run tests\n');
try {
    console.log('  [test] detection');
    testDetection();
    console.log('  [test] a basic run');
    await testBasicRun();
    console.log('  [test] a device that does not echo');
    await testNoEcho();
    console.log('  [test] quiet without a prompt');
    await testQuietWithoutPrompt();
    console.log('  [test] a slow command');
    await testSlowCommand();
    console.log('  [test] pagers');
    await testPager();
    console.log('  [test] waiting for input');
    await testNeedsInput();
    console.log('  [test] a human typing');
    await testHumanCancels();
    console.log('  [test] timeout');
    await testTimeout();
    console.log('  [test] a newline-terminated prompt');
    await testTrailingPrompt();
    console.log('  [test] a command wider than the terminal');
    await testWrappedEcho();
    console.log('  [test] bad input');
    await testBadInput();
    console.log('  [test] options that are explicitly undefined');
    await testUndefinedOptions();
    console.log('  [test] paging and truncation');
    await testGapAndPaging();
} finally {
    /* Nothing outside this process was touched. */
}

console.log(`\n${passes} passed, ${failures} failed`);
process.exit(failures ? 1 : 0);
