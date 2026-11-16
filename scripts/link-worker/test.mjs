// Tests for the sign-in relay, against a running Worker: `npm run dev` in one terminal, `npm test` in another.
// BASE=https://... runs the same checks against a deployed Worker (EXPIRY_MS is then unset and the expiry check is skipped).
const BASE = process.env.BASE || "http://127.0.0.1:8787";
const EXPIRY_MS = Number(process.env.EXPIRY_MS || (process.env.BASE ? 0 : 3000));

let checks = 0, failed = 0;
function check(ok, what) {
  checks++;
  if (!ok) { failed++; console.log("FAIL", what); }
}
// USE_CURL=1 sends the requests with curl instead of Node's own HTTPS client (some networks reset Node's connections).
import { execFileSync } from "node:child_process";
function viaCurl(path, method = "GET") {
  let raw = "";
  for (let attempt = 1; ; attempt++) {   // a flaky network may drop the odd connection: try a few times
    try {
      raw = execFileSync("curl", ["-s", "-i", "-m", "30", "-X", method, BASE + path], { maxBuffer: 1 << 24 }).toString("utf8");
      break;
    } catch (e) {
      if (attempt >= 5) throw e;
    }
  }
  const CRLF = String.fromCharCode(13, 10);
  const split = raw.indexOf(CRLF + CRLF);
  const head = raw.slice(0, split).split(CRLF), body = raw.slice(split + 4);
  const headers = new Map(head.slice(1).map((l) => [l.slice(0, l.indexOf(":")).toLowerCase(), l.slice(l.indexOf(":") + 1).trim()]));
  return { status: Number(head[0].split(" ")[1]), headers: { get: (k) => headers.get(k.toLowerCase()) ?? null }, text: async () => body, json: async () => JSON.parse(body) };
}
// PROXY=http://127.0.0.1:8899 PROXY_KEY=... sends them through scripts/dev-proxy.py instead (for a network that blocks the Worker).
const viaProxy = (path, method = "GET") =>
  fetch(`${process.env.PROXY}/${new URL(BASE).host}${path}`, { method, redirect: "manual", headers: { "X-MS-Proxy-Key": process.env.PROXY_KEY || "" } });
const send = (path, method = "GET") =>
  process.env.PROXY ? viaProxy(path, method) : process.env.USE_CURL ? viaCurl(path, method) : fetch(BASE + path, { method, redirect: "manual" });
const get = (path) => send(path);
const rnd = () => Array.from(crypto.getRandomValues(new Uint8Array(24)), (b) => "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"[b % 64]).join("");
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const S = rnd(), OTHER = rnd(), C = "A".repeat(43);

// the QR address forwards to Dropbox with everything the approval needs
let r = await get(`/a?s=${S}&c=${C}`);
const loc = new URL(r.headers.get("location") || "https://none.invalid/");
check(r.status === 302, "/a redirects");
check(loc.origin + loc.pathname === "https://www.dropbox.com/oauth2/authorize", "/a goes to Dropbox's approval page");
check(loc.searchParams.get("client_id") === "yy6m5ymz3hwdxk7", "/a carries the app key");
check(loc.searchParams.get("code_challenge") === C && loc.searchParams.get("code_challenge_method") === "S256", "/a carries the challenge");
check(loc.searchParams.get("state") === S, "/a carries the session as state");
check(loc.searchParams.get("redirect_uri") === `${new URL(BASE).origin}/cb`, "/a names its own /cb as the redirect address");
check(loc.searchParams.get("token_access_type") === "offline", "/a asks for a refresh token");
check((await get(`/a?s=short&c=${C}`)).status === 400, "/a rejects a bad session");
check((await get(`/a?s=${S}&c=tooshort`)).status === 400, "/a rejects a bad challenge");

// nothing there before the phone approves
r = await get(`/p?s=${S}`);
check(r.status === 204, "/p is empty before approval");
check((await (await get(`/s?s=${S}`)).json()).state === "waiting", "/s says waiting before approval");

// approval
r = await get(`/cb?code=GOODCODE1234&state=${S}`);
const html = await r.text();
const csp = r.headers.get("content-security-policy") || "";
const nonce = /script-src 'nonce-([^']+)'/.exec(csp)?.[1] || "";
check(r.status === 200 && html.includes("Approved"), "/cb shows the approval page");
check(nonce && html.includes(`<script nonce="${nonce}">`), "the page's script carries the policy's nonce");
check(!csp.includes("unsafe-inline") && csp.includes("default-src 'none'") && csp.includes("frame-ancestors 'none'"), "the page has a strict content policy");
check(r.headers.get("cache-control") === "no-store", "/cb is not cached");
check(html.includes("GOOD CODE 1234"), "the page shows the code in groups as a fallback");
check((await (await get(`/s?s=${S}`)).json()).state === "waiting", "/s still says waiting until the device collects it");

// the device collects it, and may ask again
r = await get(`/p?s=${S}`);
check(r.status === 200 && (await r.json()).code === "GOODCODE1234", "/p hands the device its code");
check((await (await get(`/s?s=${S}`)).json()).state === "picked", "/s says picked once collected");
r = await get(`/p?s=${S}`);
check(r.status === 200 && (await r.json()).code === "GOODCODE1234", "/p repeats the code if the first reply was lost");

// sessions are separate
check((await get(`/p?s=${OTHER}`)).status === 204, "another session sees nothing");
check((await (await get(`/s?s=${OTHER}`)).json()).state === "waiting", "another session is not picked");

// declined, malformed, and the wrong kind of request
r = await get(`/cb?error=access_denied&state=${OTHER}`);
check(r.status === 200 && (await r.text()).includes("Not connected"), "a declined approval says so");
check((await get(`/p?s=${OTHER}`)).status === 204, "a declined approval stores nothing");
check((await get(`/cb?code=x&state=${OTHER}`)).status === 200 && (await get(`/p?s=${OTHER}`)).status === 204, "a malformed code stores nothing");
check((await get(`/cb?code=GOODCODE1234&state=bad`)).status === 400, "/cb rejects a bad session");
check((await get(`/p?s=bad`)).status === 404 && (await get(`/s?s=bad`)).status === 404, "/p and /s reject a bad session");
check((await get(`/nothing`)).status === 404, "unknown paths are 404");
check((await send(`/p?s=${S}`, "POST")).status === 404, "only GET is served");

// the code does not outlive its time limit
if (EXPIRY_MS) {
  await sleep(EXPIRY_MS + 2500);
  check((await get(`/p?s=${S}`)).status === 204, "the code is gone after it expires");
  check((await (await get(`/s?s=${S}`)).json()).state === "waiting", "an expired session is not picked");
}

console.log(`${checks} checks, ${failed} failed`);
process.exit(failed ? 1 : 0);
