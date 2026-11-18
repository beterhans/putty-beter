/*
 * Tests for the bridge's discovery and framing (mcp/src/protocol.mjs).
 *
 * No PuTTY, no socket and no display: these are the parts that decide *which*
 * server to talk to, and they are the ones that break silently. In particular
 * the configuration-folder redirect, which is how a relocated installation
 * finds its endpoint file at all.
 *
 *   node mcp/test/protocol.test.mjs
 */

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

import { encode, decode, endpointCandidates, serialise } from '../src/protocol.mjs';

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

/* ------------------------------------------------------------------ */
/* A throwaway world, so nothing of the real one is touched.           */
/* ------------------------------------------------------------------ */

const work = fs.mkdtempSync(path.join(os.tmpdir(), 'putty-mcp-test-'));
const saved = { HOME: process.env.HOME, TMPDIR: process.env.TMPDIR, PUTTYDIR: process.env.PUTTYDIR };

function restore() {
    for (const [k, v] of Object.entries(saved)) {
        if (v === undefined) delete process.env[k];
        else process.env[k] = v;
    }
    fs.rmSync(work, { recursive: true, force: true });
}

function endpointFile(file, dir, pid) {
    fs.mkdirSync(dir, { recursive: true });
    const sock = path.join(dir, `fake.sock.${pid}`);
    const token = path.join(dir, `fake.token.${pid}`);
    /* The reader requires the socket path to exist, so make a real file. */
    fs.writeFileSync(sock, '');
    fs.writeFileSync(token, 'x'.repeat(64));
    fs.writeFileSync(file,
        `socket ${sock}\ntoken ${token}\npid ${pid}\nversion 1\n`);
    return { sock, token };
}

/* ------------------------------------------------------------------ */
/* Framing                                                             */
/* ------------------------------------------------------------------ */

function testFraming() {
    const fields = ['read', '1', '42', 'Router#> show version\nsecond line'];
    const decoded = decode(encode(fields));
    check(JSON.stringify(decoded) === JSON.stringify(fields),
          'framing: a message survives a round trip',
          `got ${JSON.stringify(decoded)}`);

    /* Empty fields are meaningful: "no since" and "no gap" travel as "". */
    const withEmpty = ['ok', '', '', '0'];
    check(JSON.stringify(decode(encode(withEmpty))) === JSON.stringify(withEmpty),
          'framing: empty fields survive, they are not dropped');

    /* The first field's length is at offset 4, big-endian. */
    const bytes = encode(['hello', '1', 'tok']);
    check(bytes.readUInt32BE(0) === 3, 'framing: the field count is first');
    check(bytes.readUInt32BE(4) === 5, 'framing: lengths are big-endian');
    check(bytes.subarray(8, 13).toString() === 'hello',
          'framing: the first field follows its length');

    /*
     * A read() reply carries three fields per line, so a reply to a session
     * with a normal amount of output has hundreds of fields. A field-count
     * limit that was fine for requests used to reject these, and reported it
     * as a malformed response - so this is checked at a realistic size.
     */
    const lines = [];
    for (let i = 0; i < 200; i++) {
        lines.push(String(i), 'd', `output line number ${i} with some text`);
    }
    const reply = ['ok', '199', '200', '', '', '0', '200', ...lines];
    const big = decode(encode(reply));
    check(big.length === reply.length && big.length === 607,
          'framing: a 200-line read reply survives (607 fields)',
          `got ${big.length} fields`);
    check(big[big.length - 1] === lines[lines.length - 1],
          'framing: the last line of a large reply is intact');
}

/* ------------------------------------------------------------------ */
/* Discovery                                                           */
/* ------------------------------------------------------------------ */

function testRedirect() {
    /*
     * The shape a relocated installation has: $HOME/.putty holds a
     * config-location file naming the folder actually in use, and the server
     * writes its endpoint into *that* folder. A bridge that only looked at
     * $HOME/.putty would never find it.
     */
    const home = path.join(work, 'home');
    const base = path.join(home, '.putty');
    const conf = path.join(work, 'config');

    fs.mkdirSync(base, { recursive: true });
    fs.writeFileSync(path.join(base, 'config-location'),
        `# a comment, then the real location\n\n${conf}\n`);
    endpointFile(path.join(conf, 'mcp-endpoint.4242'), conf, 4242);

    process.env.HOME = home;
    process.env.TMPDIR = path.join(work, 'tmp');
    delete process.env.PUTTYDIR;

    const sources = endpointCandidates({}).map((c) => c.source);
    check(sources.includes(path.join(conf, 'mcp-endpoint.4242')),
          'discovery: follows config-location to the effective folder',
          `looked at ${JSON.stringify(sources)}`);
}

function testRedirectWithTilde() {
    /* "~" in the redirect is expanded, as config_expand_home() does. */
    const home = path.join(work, 'home2');
    const base = path.join(home, '.putty');
    const conf = path.join(home, 'moved');
    fs.mkdirSync(base, { recursive: true });
    fs.writeFileSync(path.join(base, 'config-location'), '~/moved\n');
    endpointFile(path.join(conf, 'mcp-endpoint.7'), conf, 7);

    process.env.HOME = home;
    process.env.TMPDIR = path.join(work, 'tmp2');

    const sources = endpointCandidates({}).map((c) => c.source);
    check(sources.includes(path.join(conf, 'mcp-endpoint.7')),
          'discovery: expands ~ in config-location',
          `looked at ${JSON.stringify(sources)}`);
}

function testBrokenRedirect() {
    /* A redirect naming nothing is ignored, not fatal. */
    const home = path.join(work, 'home3');
    const base = path.join(home, '.putty');
    fs.mkdirSync(base, { recursive: true });
    fs.writeFileSync(path.join(base, 'config-location'), '/no/such/directory\n');
    process.env.HOME = home;
    process.env.TMPDIR = path.join(work, 'tmp3');

    let ok = true;
    try {
        endpointCandidates({});
    } catch {
        ok = false;
    }
    check(ok, 'discovery: a redirect naming nothing is ignored');
}

function testMultipleServersAndStale() {
    /*
     * One socket per process: two PuTTYs must both be found. And a file left
     * by a PuTTY that was killed - whose socket is gone - must be skipped
     * rather than offered and then fail to connect.
     */
    const home = path.join(work, 'home4');
    const conf = path.join(home, '.putty');
    fs.mkdirSync(conf, { recursive: true });

    endpointFile(path.join(conf, 'mcp-endpoint.11'), conf, 11);
    endpointFile(path.join(conf, 'mcp-endpoint.22'), conf, 22);

    /* A stale one: its socket file is deleted, as a killed PuTTY leaves it. */
    const stale = endpointFile(path.join(conf, 'mcp-endpoint.33'), conf, 33);
    fs.rmSync(stale.sock);

    process.env.HOME = home;
    process.env.TMPDIR = path.join(work, 'tmp4');

    const found = endpointCandidates({}).map((c) => c.pid).sort();
    check(found.includes(11) && found.includes(22),
          'discovery: finds every process offering sessions',
          `found pids ${JSON.stringify(found)}`);
    check(!found.includes(33),
          'discovery: skips an endpoint whose socket is gone',
          `found pids ${JSON.stringify(found)}`);
}

/* ------------------------------------------------------------------ */
/* One run at a time per session                                       */
/* ------------------------------------------------------------------ */

/*
 * Two commands in one stream interleave, and each run's window - "everything
 * after my own line" - catches the other's output. The reply does not say so,
 * which is what makes it dangerous: found by two runs in a Promise.all against
 * a real Synology NAS, where one result came back with the other's command in
 * it and looked entirely plausible.
 */
async function testSerialise() {
    const tick = (ms) => new Promise((r) => setTimeout(r, ms));

    /* Same key: one at a time, in the order asked. */
    const order = [];
    const queued = (name, ms) => serialise('queue-test', async () => {
        order.push(`${name}:start`);
        await tick(ms);
        order.push(`${name}:end`);
        return name;
    });
    const results = await Promise.all([queued('a', 30), queued('b', 5), queued('c', 1)]);

    check(order.join(',') === 'a:start,a:end,b:start,b:end,c:start,c:end',
          'serialise: one at a time, in the order asked', order.join(','));
    check(JSON.stringify(results) === JSON.stringify(['a', 'b', 'c']),
          'serialise: each caller still gets its own result',
          JSON.stringify(results));

    /* Different keys do not wait for each other. */
    const order2 = [];
    const keyed = (key, name, ms) => serialise(key, async () => {
        order2.push(`${name}:start`);
        await tick(ms);
        order2.push(`${name}:end`);
    });
    await Promise.all([keyed('key-x', 'slow', 30), keyed('key-y', 'fast', 5)]);

    check(order2.join(',') === 'slow:start,fast:start,fast:end,slow:end',
          'serialise: different sessions do not block each other', order2.join(','));

    /* A failure must not poison the queue behind it. */
    const bad = serialise('key-z', async () => { throw new Error('boom'); });
    const good = serialise('key-z', async () => 'after');
    let caught = '';
    await bad.catch((e) => { caught = e.message; });
    check(caught === 'boom', 'serialise: the failing caller sees its own error', caught);
    check(await good === 'after', 'serialise: and the next caller still runs');

    /* And the queue does not leak: after everything settles, a fresh call runs. */
    check(await serialise('key-z', async () => 'again') === 'again',
          'serialise: the queue is cleaned up, so it does not grow for ever');
}

/* ------------------------------------------------------------------ */

console.log('putty-mcp bridge tests\n');
try {
    console.log('  [test] framing');
    testFraming();
    console.log('  [test] config-location redirect');
    testRedirect();
    console.log('  [test] ~ in the redirect');
    testRedirectWithTilde();
    console.log('  [test] broken redirect');
    testBrokenRedirect();
    console.log('  [test] several processes, one stale');
    testMultipleServersAndStale();
    console.log('  [test] one run at a time per session');
    await testSerialise();
} finally {
    restore();
}

console.log(`\n${passes} passed, ${failures} failed`);
process.exit(failures ? 1 : 0);
