// Starts a LOCAL wrangler dev from the BattleShip worktree's server\cloudflare folder for qa\invite_e2e.ps1 and keeps it up until a stop file appears.
//   node qa\invite_e2e_server.mjs <server\cloudflare dir> <port> <stop file>
// Vars as test_admin_invites.mjs (approval ON, owner = 26 x 'a'), but real-time invite hours. Prints "READY <pid>" with the wrangler node PID.
import { pathToFileURL } from "node:url";
import { existsSync } from "node:fs";
import { join } from "node:path";
const [dir, portArg, stopFile] = process.argv.slice(2);
process.chdir(dir);
const { startServer } = await import(pathToFileURL(join(dir, "test_v2_lib.mjs")).href);
const port = Number(portArg);
const srv = await startServer({ port, vars: { ADMIN_UID: "a".repeat(26), REG_APPROVAL_DEFAULT: "1", REG_DEFAULT_CUTOFF: "4102444800000", REG_TOUCH_MS: "0",
  REG_PENDING_RECHECK_MS: "300", REG_INVITE_ACTIVE_CAP: "12", REG_INVITE_GLOBAL_CAP: "60" } });
const http = `http://127.0.0.1:${port}`;
await fetch(`${http}/__test/reg?op=t_cutoff`, { method: "POST", body: JSON.stringify({ at: Date.now() }) });   // every account created from now on is new (pending)
console.log("READY", process.pid, "cwd", process.cwd());
while (!existsSync(stopFile)) await new Promise((r) => setTimeout(r, 500));
srv.stop();
await new Promise((r) => setTimeout(r, 1500));
process.exit(0);
