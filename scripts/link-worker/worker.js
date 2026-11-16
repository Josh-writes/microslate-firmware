// MicroSlate sign-in relay: a Cloudflare Worker that hands a Dropbox one-time code from your phone to your device.
//
// It is only used while a device signs in to Dropbox. Notes and tokens never pass through it afterwards.
//
//   GET /a?s=<session>&c=<challenge>   the QR code opens this; it forwards the phone to Dropbox's approval page
//   GET /cb?code=...&state=<session>   Dropbox sends the phone back here; the one-time code is kept for 10 minutes
//   GET /p?s=<session>                 the device asks for its code (200 {"code":"..."}, or 204 while it is not there yet)
//   GET /s?s=<session>                 the phone page asks whether the device has picked the code up ({"state":...})
//
// Each session lives in its own Durable Object, so the phone and the device always see the same state at once, wherever
// in the world each of them connects from (plain KV storage can take up to a minute to agree between locations).
//
// The one-time code is useless without the PKCE verifier, which never leaves the device. The session id is random and
// kept secret between the device and the phone that scans its QR code. Nothing is logged and nothing is kept longer
// than KEEP_MS; a session that is never approved stores nothing at all.

import { DurableObject } from "cloudflare:workers";

const APP_KEY = "yy6m5ymz3hwdxk7";   // the Dropbox app key (public)
const SESSION = /^[A-Za-z0-9_-]{22,64}$/;
const CHALLENGE = /^[A-Za-z0-9_-]{43}$/;
const CODE = /^[A-Za-z0-9_.~-]{8,256}$/;

const HEADERS = {
  "cache-control": "no-store",
  "referrer-policy": "no-referrer",
  "x-content-type-options": "nosniff",
  "x-robots-tag": "noindex",
};

// One sign-in attempt: the code the phone got, and whether the device has collected it.
export class LinkSession extends DurableObject {
  async approve(code, keepMs) {
    await this.ctx.storage.put({ code, picked: false });
    await this.ctx.storage.setAlarm(Date.now() + keepMs);
  }

  // The device collects the code. It stays readable until it expires, so a lost reply can be asked for again.
  async read() {
    const code = await this.ctx.storage.get("code");
    if (!code) return null;
    if (!(await this.ctx.storage.get("picked"))) await this.ctx.storage.put("picked", true);
    return code;
  }

  async status() {
    if (!(await this.ctx.storage.get("code"))) return "waiting";
    return (await this.ctx.storage.get("picked")) ? "picked" : "waiting";
  }

  async alarm() {
    await this.ctx.storage.deleteAll();
  }
}

function text(status, body = "Not found") {
  return new Response(body, { status, headers: HEADERS });
}

function json(status, value) {
  return new Response(JSON.stringify(value), { status, headers: { ...HEADERS, "content-type": "application/json" } });
}

// A small page. `script` (optional) is the only script allowed to run on it.
function page(title, body, status = 200, script = "") {
  const nonce = btoa(String.fromCharCode(...crypto.getRandomValues(new Uint8Array(16))));
  const html = `<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${title}</title><style nonce="${nonce}">
body{font:18px/1.5 system-ui,sans-serif;max-width:30rem;margin:12vh auto;padding:0 1.2rem;color:#1b1b1b;background:#fafafa}
@media(prefers-color-scheme:dark){body{color:#eee;background:#161616}}
h1{font-size:1.5rem}code{font:1.5rem/1.6 ui-monospace,monospace;letter-spacing:.08em;word-break:break-all}
small{opacity:.7}</style></head><body>${body}${script ? `<script nonce="${nonce}">${script}</script>` : ""}</body></html>`;
  return new Response(html, {
    status,
    headers: {
      ...HEADERS,
      "content-type": "text/html; charset=utf-8",
      "content-security-policy": `default-src 'none'; style-src 'nonce-${nonce}'; script-src 'nonce-${nonce}'; connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'`,
    },
  });
}

const BAD_LINK = () => page("Link expired", "<h1>That link is not valid</h1><p>Go back to your MicroSlate and start again.</p>", 400);

function statusScript(session) {
  return `(function () {
  var s = ${JSON.stringify(session)}, started = Date.now();
  var msg = document.getElementById("msg"), hint = document.getElementById("hint"), manual = document.getElementById("manual");
  function tick() {
    fetch("/s?s=" + s, { cache: "no-store" }).then(function (r) { return r.json(); }).then(function (j) {
      if (j.state === "picked") {
        msg.textContent = "Your MicroSlate has it and is connecting. You can close this page.";
        hint.textContent = "";
        return;
      }
      if (Date.now() - started > 180000) {
        msg.textContent = "Your MicroSlate has not picked it up yet.";
        hint.textContent = "Make sure it is still on the sign-in screen.";
        manual.hidden = false;
        return;
      }
      setTimeout(tick, 2000);
    }).catch(function () { setTimeout(tick, 4000); });
  }
  tick();
})();`;
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method !== "GET") return text(404);
    const keepMs = Number(env.KEEP_MS) > 0 ? Number(env.KEEP_MS) : 10 * 60 * 1000;
    const session = (id) => env.LINK.get(env.LINK.idFromName(id));

    if (url.pathname === "/a") {
      const s = url.searchParams.get("s") || "";
      const c = url.searchParams.get("c") || "";
      if (!SESSION.test(s) || !CHALLENGE.test(c)) return BAD_LINK();
      const to = new URL("https://www.dropbox.com/oauth2/authorize");
      to.searchParams.set("client_id", APP_KEY);
      to.searchParams.set("response_type", "code");
      to.searchParams.set("code_challenge", c);
      to.searchParams.set("code_challenge_method", "S256");
      to.searchParams.set("token_access_type", "offline");
      to.searchParams.set("redirect_uri", `${url.origin}/cb`);
      to.searchParams.set("state", s);
      return new Response(null, { status: 302, headers: { ...HEADERS, location: to.toString() } });
    }

    if (url.pathname === "/cb") {
      const state = url.searchParams.get("state") || "";
      const code = url.searchParams.get("code") || "";
      if (!SESSION.test(state)) return BAD_LINK();
      if (url.searchParams.get("error") || !CODE.test(code)) {
        return page("Not connected", "<h1>Not connected</h1><p>Dropbox was not approved. You can close this page and try again on your MicroSlate.</p>");
      }
      await session(state).approve(code, keepMs);
      const grouped = code.replace(/(.{4})/g, "$1 ").trim();
      return page(
        "Approved",
        `<h1>Approved</h1>
<p id="msg">Waiting for your MicroSlate to pick it up&hellip;</p>
<p><small id="hint">Keep this page open for a moment. This usually takes a few seconds.</small></p>
<details id="manual" hidden><summary>It is taking too long</summary>
<p><small>On your MicroSlate choose &ldquo;Type code&rdquo; and type this code:</small></p><p><code>${grouped}</code></p></details>`,
        200,
        statusScript(state),
      );
    }

    if (url.pathname === "/p") {
      const s = url.searchParams.get("s") || "";
      if (!SESSION.test(s)) return text(404);
      const code = await session(s).read();
      return code ? json(200, { code }) : new Response(null, { status: 204, headers: HEADERS });
    }

    if (url.pathname === "/s") {
      const s = url.searchParams.get("s") || "";
      if (!SESSION.test(s)) return text(404);
      return json(200, { state: await session(s).status() });
    }

    return text(404);
  },
};
