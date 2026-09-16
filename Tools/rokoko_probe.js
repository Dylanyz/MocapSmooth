#!/usr/bin/env node
/*
 * rokoko_probe.js — talk to Rokoko Studio Preview's motion engine over JSON-RPC.
 *
 * Used to derive and re-verify the smoothing filter spec in
 * ../.claude/refs/rokoko-measurement.md. Requires your own licensed Rokoko Studio
 * Preview install; no Rokoko code is included or redistributed here.
 *
 * Usage:
 *   node rokoko_probe.js groups
 *   node rokoko_probe.js info
 *   node rokoko_probe.js script <steps.json>
 *
 * steps.json is an array of [method, params, opts]; params null means "send no params"
 * (required, because a literal null crashes the engine). opts may set {save:"file.json"}.
 *
 * Environment:
 *   ROKOKO_ENGINE   path to MotionEngineCli-<ver>.exe
 *                   default: %LOCALAPPDATA%\studio_desktop\app-1.2.0\resources\MotionEngineCli-1.2.1.exe
 *   ROKOKO_APPDATA  directory the engine uses for its Scenes folder.
 *                   Point this at a sandbox so your real scenes are untouched.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

const { spawn } = require("child_process");
const fs = require("fs");
const path = require("path");

const DEFAULT_ENGINE = path.join(
  process.env.LOCALAPPDATA || "",
  "studio_desktop", "app-1.2.0", "resources", "MotionEngineCli-1.2.1.exe"
);

const ENGINE = process.env.ROKOKO_ENGINE || DEFAULT_ENGINE;
const APPDATA = process.env.ROKOKO_APPDATA ||
  path.join(process.env.APPDATA || "", "Rokoko Studio Preview");

function connect() {
  if (!fs.existsSync(ENGINE)) {
    console.error(`engine not found: ${ENGINE}\nset ROKOKO_ENGINE to your MotionEngineCli exe`);
    process.exit(2);
  }
  const proc = spawn(ENGINE, ["serve", "stdio"], { stdio: ["pipe", "pipe", "pipe"] });
  let buf = Buffer.alloc(0);
  const pending = new Map();

  proc.stdout.on("data", (d) => {
    buf = Buffer.concat([buf, d]);
    for (;;) {
      const sep = buf.indexOf("\r\n\r\n");
      if (sep < 0) break;
      const m = /Content-Length:\s*(\d+)/i.exec(buf.slice(0, sep).toString());
      if (!m) break;
      const len = +m[1];
      if (buf.length < sep + 4 + len) break;
      const body = buf.slice(sep + 4, sep + 4 + len).toString();
      buf = buf.slice(sep + 4 + len);
      let msg;
      try { msg = JSON.parse(body); } catch { continue; }
      if (msg.id && pending.has(msg.id)) {
        pending.get(msg.id)(msg);
        pending.delete(msg.id);
      }
    }
  });

  let id = 0;
  // NOTE: params must be omitted entirely for no-argument methods.
  // Sending "params": null makes StreamJsonRpc throw and the engine exits.
  const call = (method, params, timeoutMs = 300000) =>
    new Promise((resolve) => {
      const myId = ++id;
      const req = { jsonrpc: "2.0", id: myId, method };
      if (params !== undefined && params !== null) req.params = params;
      const json = JSON.stringify(req);
      const timer = setTimeout(() => resolve({ timeout: true }), timeoutMs);
      pending.set(myId, (r) => { clearTimeout(timer); resolve(r); });
      proc.stdin.write(`Content-Length: ${Buffer.byteLength(json)}\r\n\r\n${json}`);
    });

  return { proc, call };
}

async function main() {
  const [cmd, arg] = process.argv.slice(2);
  const { proc, call } = connect();

  // initialize takes a POSITIONAL array containing one string. Everything else
  // wraps its arguments in a `parameters` object.
  const init = await call("initialize", [APPDATA.replace(/\\/g, "/")]);
  if (init.error) {
    console.error("initialize failed:", JSON.stringify(init.error).slice(0, 400));
    proc.kill();
    process.exit(1);
  }

  if (cmd === "info") {
    console.log(JSON.stringify(init.result, null, 2));
  } else if (cmd === "groups" || !cmd) {
    const r = await call("getSmoothingGroups");
    console.log(JSON.stringify(r.result ?? r.error, null, 2));
  } else if (cmd === "script") {
    if (!arg) { console.error("usage: rokoko_probe.js script <steps.json>"); proc.kill(); process.exit(2); }
    const steps = JSON.parse(fs.readFileSync(arg, "utf8"));
    for (const [method, params, opts] of steps) {
      const r = await call(method, params);
      console.log(`\n>> ${method} ${JSON.stringify(params ?? {}).slice(0, 200)}`);
      if (opts && opts.save && r.result) {
        fs.writeFileSync(opts.save, JSON.stringify(r.result));
        console.log(`<< saved ${opts.save} (${JSON.stringify(r.result).length} bytes)`);
      } else {
        console.log(`<< ${JSON.stringify(r).slice(0, (opts && opts.len) || 1000)}`);
      }
    }
  } else {
    console.error(`unknown command: ${cmd}`);
    proc.kill();
    process.exit(2);
  }

  proc.kill();
  process.exit(0);
}

main();
