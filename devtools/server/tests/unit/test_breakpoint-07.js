/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */
/* eslint-disable no-shadow, max-nested-callbacks */

"use strict";

/**
 * Check that setting a breakpoint in a line without code in the second child
 * script will skip forward.
 */

add_task(threadClientTest(({ threadClient, debuggee }) => {
  return new Promise(resolve => {
    threadClient.once("paused", async function(packet) {
      const source = await getSourceById(
        threadClient,
        packet.frame.where.actor
      );
      const location = { line: debuggee.line0 + 6 };

      source.setBreakpoint(location).then(function([response, bpClient]) {
        // Check that the breakpoint has properly skipped forward one line.
        Assert.equal(response.actualLocation.source.actor, source.actor);
        Assert.equal(response.actualLocation.line, location.line + 1);

        threadClient.once("paused", function(packet) {
          // Check the return value.
          Assert.equal(packet.type, "paused");
          Assert.equal(packet.frame.where.actor, source.actor);
          Assert.equal(packet.frame.where.line, location.line + 1);
          Assert.equal(packet.why.type, "breakpoint");
          Assert.equal(packet.why.actors[0], bpClient.actor);
          // Check that the breakpoint worked.
          Assert.equal(debuggee.a, 1);
          Assert.equal(debuggee.b, undefined);

          // Remove the breakpoint.
          bpClient.remove(function(response) {
            threadClient.resume().then(resolve);
          });
        });

        // Continue until the breakpoint is hit.
        threadClient.resume();
      });
    });

    /* eslint-disable */
    Cu.evalInSandbox(
      "var line0 = Error().lineNumber;\n" +
      "function foo() {\n" + // line0 + 1
      "  bar();\n" +         // line0 + 2
      "}\n" +                // line0 + 3
      "function bar() {\n" + // line0 + 4
      "  this.a = 1;\n" +    // line0 + 5
      "  // A comment.\n" +  // line0 + 6
      "  this.b = 2;\n" +    // line0 + 7
      "}\n" +                // line0 + 8
      "debugger;\n" +        // line0 + 9
      "foo();\n",           // line0 + 10
      debuggee
    );
    /* eslint-enable */
  });
}));
