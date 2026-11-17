#!/usr/bin/env node
/*
 * putty-mcp - the bridge between an MCP client and a running PuTTY.
 *
 * An agent launches `putty-mcp mcp` and speaks MCP on stdio; this program
 * translates that into requests over a Unix socket to PuTTY. Humans can run
 * the other subcommands (`list`, `read`, `screen`, `manual`) to see exactly
 * what the agent sees.
 *
 * See USER-MANUAL.md for the agent-facing documentation, and PLAN.md section 7
 * in the putty-beter source for the design.
 */

import { main } from '../src/cli.mjs';

/*
 * stdout belongs to the MCP protocol when we are a server, so nothing else may
 * write there. Anything unexpected goes to stderr, where a client logs it.
 */
process.on('uncaughtException', (e) => {
    process.stderr.write(`putty-mcp: uncaught: ${e && e.stack ? e.stack : e}\n`);
    process.exit(1);
});

main(process.argv.slice(2))
    .then((code) => {
        /*
         * A numeric result is an exit code for a one-shot subcommand. The MCP
         * server returns nothing, because it has to keep running until its
         * stdin closes - exiting here would kill it the instant the client
         * connected, before it could answer a single request.
         */
        if (typeof code === 'number')
            process.exit(code);
    })
    .catch((e) => {
        process.stderr.write(`putty-mcp: ${e && e.stack ? e.stack : e}\n`);
        process.exit(1);
    });
