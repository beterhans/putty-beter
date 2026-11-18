/*
 * What the last line of a session's output means.
 *
 * Pure functions: no I/O, no timers, no PuTTY. That is deliberate, because
 * this is the part that is easy to get wrong and impossible to test by hand -
 * so the whole judgement of "has the command finished, or is it waiting for
 * something?" is exercised from a table of strings in test/run.test.mjs.
 *
 * PLAN.md section 7.5 is the design. The three things worth repeating here:
 *
 *   - The blinking cursor is not a signal. PuTTY draws it; the device does not
 *     send it, and it blinks whenever the window has focus, including in the
 *     middle of a command.
 *   - The prompt format never has to be known. Three signals are combined: our
 *     own echoed command, a quiet period, and the shape of the last line.
 *   - Waiting for input is not idle. `--More--`, `[confirm]`, `[y/n]` and
 *     `Password:` are stalled but expecting something specific. Blindly sending
 *     the next command at a `[confirm]` prompt is how automation does something
 *     destructive, so these are reported rather than answered.
 */

/*
 * The default prompt family: `Router>`, `Router#`, `Router(config-if)#`,
 * `switch-01#`, `hans@MacMini2018 ~ %`, `user@host:~$`.
 *
 * Deliberately loose. A prompt is short and ends in one of `> # $ %` after
 * something that looks like a hostname, a path or a mode. It is not the only
 * signal - a quiet period has to agree with it - so a false positive here
 * costs a slightly over-confident `completed_by`, not a wrong answer.
 *
 * Checked *after* the pager and input patterns, because `--More--(50%)` ends
 * in `%` and `Password:` ends in `:` after a word.
 */
const PROMPT = /^[^\s]{0,60}(?:\s[^\s]{0,40}){0,3}[>#$%]\s*$/;

/*
 * ...but a progress bar is not a prompt. `100%` is the shape that makes a run
 * declare victory while the command is still running, which is the worst
 * failure this code has: the caller is handed a partial answer and told to
 * trust it. A bare percentage is therefore excluded from the family.
 */
const PROGRESS = /^[\d.,\s]{1,12}%$/;

/*
 * Stalled, expecting something specific. Answering any of these on the
 * caller's behalf would be a guess with consequences, so `run` stops and
 * reports instead. The reason is what the agent is shown, so it is written as
 * prose.
 */
const INPUT = [
    [/password[^\n]{0,60}:\s*$/i, 'a password prompt'],
    [/passphrase[^\n]{0,60}:\s*$/i, 'a passphrase prompt'],
    [/\[[^\]\n]{0,40}confirm[^\]\n]{0,40}\]\s*$/i, 'a [confirm] prompt'],
    [/\[(?:yes|no)(?:\/[a-z]+)?\]\s*$/i, 'a [yes/no] prompt'],
    [/\((?:yes|no)(?:\/[a-z]+)?\)\s*$/i, 'a (yes/no) prompt'],
    [/\[[yYnN]\/[yYnN]\]\s*$/i, 'a [y/n] prompt'],
    [/are you sure[^\n]{0,60}\?\s*$/i, 'an "are you sure?" prompt'],
    [/(?:do you want|would you like) to[^\n]{0,60}\?\s*$/i, 'a yes/no question'],
    [/(?:overwrite|replace)[^\n]{0,60}\?\s*$/i, 'an overwrite prompt'],
    [/press (?:any key|enter|return)[^\n]{0,20}\s*$/i, 'a "press a key" prompt'],
    [/login:\s*$/i, 'a login prompt'],
    [/username:\s*$/i, 'a username prompt'],
];

/*
 * BSD `more` and `less`, given a *file argument*, put the file name where the
 * prompt goes on the first page - in reverse video, which the capture buffer
 * strips, so all that is left to see is the name on a line of its own:
 *
 *     ESC[7m/tmp/config.txtESC[27mESC[K   ->   "/tmp/config.txt"
 *
 * It is worth a pattern because of what happens without it. The run gives up,
 * the pager is still waiting, and the *next* command is eaten by it: `more`
 * reads a leading `!` as "run a shell command", which is how the first live
 * test ended up with a shell command being typed into a pager. A wrong space is
 * a far smaller cost than a swallowed command.
 *
 * Deliberately narrow, though, and the character class is the narrowness: a
 * path or file name is letters, digits, dot, underscore, tilde and hyphen, and
 * nothing else. A first attempt allowed "any token containing a slash", which
 * matched `[y/n]` and `(yes/no)` - the two prompts that must never be answered
 * on the caller's behalf. The tests caught it, which is what they are for.
 */
const PAGER_PATH = /^\/?(?:[A-Za-z0-9._~-]+\/)+[A-Za-z0-9._~-]*$/;
const PAGER_FILE = /^[A-Za-z0-9._~-]{1,100}\.[A-Za-z][A-Za-z0-9]{0,7}$/;

/*
 * Pagers. `show running-config` on network gear stops at `--More--` and hangs
 * a naive client forever; the answer is a space, and the headline feature of
 * this phase is to send it without the caller knowing.
 *
 * `:` is the prompt `more` and `less` use from the second page onwards, and
 * the first-page prompt when they read from a pipe. It is safe to treat as a
 * pager: a shell does not print a bare `:`, and a space typed at a shell prompt
 * does nothing.
 */
const PAGER = [
    [/--\s*more\s*--[^\n]*$/i, 'a --More-- pager'],
    [/\(END\)\s*$/i, 'a less (END) pager'],
    [/^more\?\s*$/i, 'a More? pager'],
    [/\bq\)?uit\b[^\n]*--\s*more/i, 'a pager'],
    [/^:\s*$/, 'a pager prompt (":")'],
    [PAGER_PATH, 'a pager showing its file name'],
    [PAGER_FILE, 'a pager showing its file name'],
];

function lastLine(text) {
    return text == null ? '' : String(text);
}

function firstMatch(text, patterns) {
    const t = lastLine(text);
    for (const [re, reason] of patterns) {
        if (re.test(t))
            return reason;
    }
    return null;
}

/*
 * `expect` is a caller-supplied pattern for a device whose prompt the default
 * family does not cover. It replaces the default rather than adding to it: a
 * caller who knows their prompt is saying the default family is wrong for this
 * device, not that they want a second opinion.
 */
export function compileExpect(expect) {
    if (expect == null || expect === '')
        return null;
    if (expect instanceof RegExp)
        return expect;
    try {
        return new RegExp(String(expect));
    } catch (e) {
        throw new Error(`expect is not a valid regular expression: ${e.message}`);
    }
}

export function promptReason(text, expect) {
    const t = lastLine(text);
    if (expect)
        return expect.test(t) ? 'the expected pattern' : null;
    if (PROGRESS.test(t))
        return null;
    return PROMPT.test(t) ? 'the default prompt family' : null;
}

export function pagerReason(text, extra = []) {
    return firstMatch(text, PAGER) ?? firstMatch(text, extra);
}

export function inputReason(text, extra = []) {
    return firstMatch(text, INPUT) ?? firstMatch(text, extra);
}

export function isPager(text, extra = []) {
    return pagerReason(text, extra) !== null;
}

/*
 * What a single line is.
 *
 * The order is the whole point, and it is a matter of what a wrong answer
 * costs:
 *
 *   1. **Waiting for input, first.** This is the judgement that stops us doing
 *      something, so it wins every tie: when in doubt, do not type into
 *      someone's live session. `[y/n]` and `(yes/no)` are the reason it has to
 *      be first, because a pager pattern can otherwise be talked into matching
 *      them.
 *   2. **A pager**, because we act on it by sending a space.
 *   3. **A prompt**, last, because a pager marker can end in `%` and would
 *      otherwise be read as a prompt.
 */
export function classify(text, { expect, pagerPatterns = [], inputPatterns = [] } = {}) {
    const input = inputReason(text, compilePatterns(inputPatterns, 'input_patterns'));
    if (input)
        return { kind: 'input', reason: input };

    const pager = pagerReason(text, compilePatterns(pagerPatterns, 'pager_patterns'));
    if (pager)
        return { kind: 'pager', reason: pager };

    const prompt = promptReason(text, compileExpect(expect));
    if (prompt)
        return { kind: 'prompt', reason: prompt };

    return { kind: 'none', reason: null };
}

/*
 * Compile the caller's extra patterns once, rather than per poll: a run can
 * poll twenty times a second and a bad pattern should be reported at once, not
 * on the hundredth line. Idempotent, so an already-compiled list - which is
 * what the run loop passes in - is accepted as it is.
 */
export function compilePatterns(list, what) {
    if (list == null)
        return [];
    if (!Array.isArray(list))
        throw new Error(`${what} must be an array of regular expression strings`);
    return list.map((p) => {
        if (Array.isArray(p) && p.length === 2 && p[0] instanceof RegExp)
            return p;
        try {
            return [p instanceof RegExp ? p : new RegExp(String(p)), String(p)];
        } catch (e) {
            throw new Error(`${what}: "${p}" is not a valid regular expression: ${e.message}`);
        }
    });
}
