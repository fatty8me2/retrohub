// Runs a command against a LOCAL wrangler dev worker (loopback, isolated registry, short --persist-to C:\rhq\<port>) and stops
// only the process it started. Never production.
//   node qa\online_local.mjs <port> <cmd> [args...]     RETROHUB_TEST_SERVER=ws://127.0.0.1:<port> is set for the command.
// Run from anywhere; the worker dir defaults to ..\BattleShip\server\cloudflare next to this repo (override: BS_WORKER_DIR).
import { spawnSync } from "node:child_process";
import { resolve, dirname } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const port = Number(process.argv[2]);
const cmd = process.argv.slice(3);
if (!port || port < 11200 || cmd.length === 0) { console.error("usage: node online_local.mjs <port >= 11200> <cmd> [args]"); process.exit(2); }
const here = dirname(fileURLToPath(import.meta.url));
const worker = process.env.BS_WORKER_DIR || resolve(here, "..", "..", "BattleShip", "server", "cloudflare");
process.chdir(worker);
const { startServer } = await import(pathToFileURL(resolve(worker, "test_v2_lib.mjs")).href);
const srv = await startServer({ port });
let code = 1;
try {
  const r = spawnSync(cmd[0], cmd.slice(1), { stdio: "inherit", env: { ...process.env, RETROHUB_TEST_SERVER: `ws://127.0.0.1:${port}` } });
  code = r.status ?? 1;
} finally {
  srv.stop();
}
process.exit(code);
