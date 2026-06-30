const DEFAULT_GAS_URL =
  "https://script.google.com/macros/s/AKfycbw6UBeU1AqIufbMMQsscMWH1j2ptRGQbKYzIzKBmi7KegfDwhkx6iJvEXElr9RZYOcS/exec";

const JSON_HEADERS = {
  "Content-Type": "application/json; charset=utf-8",
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers":
    "Content-Type, Authorization, X-Device-Id, X-Device-Secret, X-Bootstrap-Token",
};

export default {
  async fetch(request, env, ctx) {
    try {
      if (request.method === "OPTIONS") {
        return new Response(null, { status: 204, headers: JSON_HEADERS });
      }

      const url = new URL(request.url);

      if (isLegacyGoogleFormsRequest(request, url)) {
        return proxyToGoogleAppsScript(request, env, url);
      }

      if (url.pathname === "/" || url.pathname === "/admin") {
        return html(adminHtml(), 200);
      }

      if (url.pathname === "/api/health") {
        return json({
          ok: true,
          service: "esp32-cloud",
          d1: Boolean(env.DB),
          time: new Date().toISOString(),
        });
      }

      if (url.pathname.startsWith("/api/admin/")) {
        return handleAdminApi(request, env, url);
      }

      if (url.pathname.startsWith("/api/devices/")) {
        return handleDeviceApi(request, env, url);
      }

      if (url.pathname.startsWith("/api/firmware/download/")) {
        if (!env.FIRMWARE_KV) return new Response("KV not configured", { status: 500 });
        const version = url.pathname.split("/").pop();
        const bin = await env.FIRMWARE_KV.get("fw_" + version, "arrayBuffer");
        if (!bin) return new Response("Not found", { status: 404 });
        return new Response(bin, { headers: { "Content-Type": "application/octet-stream" }});
      }

      return json({ error: "not_found" }, 404);
    } catch (error) {
      if (error instanceof HttpError) {
        return json({ error: error.message }, error.status);
      }
      return json({ error: "worker_error", message: String(error && error.message ? error.message : error) }, 500);
    }
  },
};

function isLegacyGoogleFormsRequest(request, url) {
  return (
    url.searchParams.has("action") ||
    (url.pathname === "/" && request.method === "POST") ||
    url.pathname === "/legacy/google-forms"
  );
}

async function proxyToGoogleAppsScript(request, env, url) {
  const gasUrl = env.GAS_URL || DEFAULT_GAS_URL;
  const target = new URL(gasUrl);

  if (request.method === "GET") {
    for (const [key, value] of url.searchParams.entries()) {
      target.searchParams.set(key, value);
    }
  }

  const response = await fetch(target.toString(), {
    method: request.method,
    headers: {
      "User-Agent": "Cloudflare-Worker",
      "Content-Type": request.headers.get("Content-Type") || "application/x-www-form-urlencoded",
    },
    body: request.method === "GET" || request.method === "HEAD" ? undefined : await request.text(),
  });

  const responseText = await response.text();
  const newHeaders = new Headers(response.headers);
  newHeaders.set("Access-Control-Allow-Origin", "*");
  newHeaders.set("Cache-Control", "no-store");
  newHeaders.set("Content-Type", response.headers.get("Content-Type") || "text/plain; charset=utf-8");
  newHeaders.set("Content-Length", String(new TextEncoder().encode(responseText).length));

  return new Response(responseText, {
    status: response.status,
    headers: newHeaders,
  });
}

async function handleAdminApi(request, env, url) {
  requireDatabase(env);
  requireAdmin(request, env);

  if (url.pathname === "/api/admin/devices" && request.method === "GET") {
    const result = await env.DB.prepare(
      `SELECT device_id, name, model, firmware_version, ip_address, status,
              last_seen_at, created_at, updated_at, wifi_ssid, wifi_pass, supported_commands
         FROM devices
        ORDER BY COALESCE(last_seen_at, created_at) DESC`
    ).all();
    return json({ devices: result.results || [] });
  }

  if (url.pathname === "/api/admin/devices" && request.method === "DELETE") {
    const body = await readJson(request);
    const deviceId = requiredString(body.deviceId, "deviceId");
    await env.DB.prepare("DELETE FROM devices WHERE device_id = ?").bind(deviceId).run();
    return json({ ok: true });
  }

  if (url.pathname === "/api/admin/devices" && request.method === "POST") {
    const body = await readJson(request);
    const deviceId = requiredString(body.deviceId, "deviceId");
    const now = new Date().toISOString();
    
    if (body.secret && body.secret.trim() !== "") {
      const hash = await hashDeviceSecret(deviceId, body.secret.trim());
      await env.DB.prepare(
        `INSERT INTO devices(device_id, name, secret_hash, model, firmware_version, status, created_at, updated_at)
         VALUES(?, ?, ?, ?, ?, 'active', ?, ?)
         ON CONFLICT(device_id) DO UPDATE SET
           name=excluded.name,
           secret_hash=excluded.secret_hash,
           model=excluded.model,
           firmware_version=excluded.firmware_version,
           status='active',
           updated_at=excluded.updated_at`
      )
        .bind(
          deviceId,
          optionalString(body.name, deviceId),
          hash,
          optionalString(body.model, "esp32"),
          optionalString(body.firmwareVersion, "unknown"),
          now,
          now
        )
        .run();
    } else {
      const existing = await env.DB.prepare("SELECT 1 FROM devices WHERE device_id = ?").bind(deviceId).first();
      if (!existing) throw new HttpError(400, "secret_required_for_new_device");
      
      await env.DB.prepare(
        `UPDATE devices SET firmware_version = ?, updated_at = ? WHERE device_id = ?`
      ).bind(optionalString(body.firmwareVersion, "unknown"), now, deviceId).run();
    }

    return json({ ok: true, deviceId });
  }

  if (url.pathname === "/api/admin/commands" && request.method === "GET") {
    const deviceId = url.searchParams.get("deviceId");
    const query = deviceId
      ? env.DB.prepare(
          `SELECT command_id, device_id, type, payload, status, result,
                  created_at, delivered_at, completed_at
             FROM device_commands
            WHERE device_id = ?
            ORDER BY created_at DESC
            LIMIT 100`
        ).bind(deviceId)
      : env.DB.prepare(
          `SELECT command_id, device_id, type, payload, status, result,
                  created_at, delivered_at, completed_at
             FROM device_commands
            ORDER BY created_at DESC
            LIMIT 100`
        );

    const result = await query.all();
    return json({ commands: result.results || [] });
  }

  if (url.pathname === "/api/admin/commands" && request.method === "POST") {
    const body = await readJson(request);
    const deviceId = requiredString(body.deviceId, "deviceId");
    const type = requiredString(body.type, "type");
    const commandId = crypto.randomUUID();
    const now = new Date().toISOString();
    const payload = JSON.stringify(body.payload || {});

    await env.DB.prepare(
      `INSERT INTO device_commands(command_id, device_id, type, payload, status, created_at)
       VALUES(?, ?, ?, ?, 'queued', ?)`
    )
      .bind(commandId, deviceId, type, payload, now)
      .run();

    return json({ ok: true, commandId });
  }

  if (url.pathname === "/api/admin/logs" && request.method === "GET") {
    const deviceId = url.searchParams.get("deviceId");
    const limit = clampNumber(Number(url.searchParams.get("limit") || 100), 1, 500);
    const query = deviceId
      ? env.DB.prepare(
          `SELECT id, device_id, level, message, payload, created_at
             FROM device_logs
            WHERE device_id = ?
            ORDER BY id DESC
            LIMIT ?`
        ).bind(deviceId, limit)
      : env.DB.prepare(
          `SELECT id, device_id, level, message, payload, created_at
             FROM device_logs
            ORDER BY id DESC
            LIMIT ?`
        ).bind(limit);

    const result = await query.all();
    return json({ logs: result.results || [] });
  }

  if (url.pathname === "/api/admin/firmware" && request.method === "GET") {
    const result = await env.DB.prepare(
      `SELECT id, target_version, url, sha256, notes, required, is_active, created_at
         FROM firmware_releases
        ORDER BY id DESC
        LIMIT 50`
    ).all();
    return json({ releases: result.results || [] });
  }

  if (url.pathname === "/api/admin/firmware" && request.method === "POST") {
    const body = await readJson(request);
    const targetVersion = requiredString(body.targetVersion, "targetVersion");
    const firmwareUrl = requiredString(body.url, "url");
    const now = new Date().toISOString();

    if (body.active === true) {
      await env.DB.prepare("UPDATE firmware_releases SET is_active = 0").run();
    }

    const result = await env.DB.prepare(
      `INSERT INTO firmware_releases(target_version, url, sha256, notes, required, is_active, created_at)
       VALUES(?, ?, ?, ?, ?, ?, ?)`
    )
      .bind(
        targetVersion,
        firmwareUrl,
        optionalString(body.sha256, ""),
        optionalString(body.notes, ""),
        body.required === true ? 1 : 0,
        body.active === false ? 0 : 1,
        now
      )
      .run();

    return json({ ok: true, id: result.meta.last_row_id });
  }

  if (url.pathname === "/api/admin/firmware/upload" && request.method === "POST") {
    if (!env.FIRMWARE_KV) throw new HttpError(500, "KV not configured");
    const targetVersion = url.searchParams.get("version");
    if (!targetVersion) throw new HttpError(400, "version_required");
    const buffer = await request.arrayBuffer();
    if (buffer.byteLength === 0) throw new HttpError(400, "empty_file");
    
    await env.FIRMWARE_KV.put("fw_" + targetVersion, buffer);
    const firmwareUrl = "/api/firmware/download/" + targetVersion;
    const now = new Date().toISOString();

    await env.DB.prepare("UPDATE firmware_releases SET is_active = 0").run();
    await env.DB.prepare(
      `INSERT INTO firmware_releases(target_version, url, sha256, notes, required, is_active, created_at)
       VALUES(?, ?, ?, ?, ?, ?, ?)`
    ).bind(targetVersion, firmwareUrl, "", "", 0, 1, now).run();

    return json({ ok: true, url: firmwareUrl });
  }

  return json({ error: "not_found" }, 404);
}

async function handleDeviceApi(request, env, url) {
  requireDatabase(env);

  if (url.pathname === "/api/devices/register" && request.method === "POST") {
    requireBootstrapToken(request, env);
    const body = await readJson(request);
    const deviceId = requiredString(body.deviceId, "deviceId");
    const secret = requiredString(body.secret, "secret");
    const now = new Date().toISOString();
    const hash = await hashDeviceSecret(deviceId, secret);

    await env.DB.prepare(
      `INSERT INTO devices(device_id, name, secret_hash, model, firmware_version, status, created_at, updated_at)
       VALUES(?, ?, ?, ?, ?, 'active', ?, ?)
       ON CONFLICT(device_id) DO UPDATE SET
         name=excluded.name,
         secret_hash=excluded.secret_hash,
         model=excluded.model,
         firmware_version=excluded.firmware_version,
         status='active',
         updated_at=excluded.updated_at`
    )
      .bind(
        deviceId,
        optionalString(body.name, deviceId),
        hash,
        optionalString(body.model, "esp32"),
        optionalString(body.firmwareVersion, "unknown"),
        now,
        now
      )
      .run();

    return json({ ok: true, deviceId });
  }

  const device = await requireDevice(request, env);

  if (url.pathname === "/api/devices/heartbeat" && request.method === "POST") {
    const body = await readJson(request);
    const now = new Date().toISOString();
    const ip = request.headers.get("CF-Connecting-IP") || "";

    await env.DB.batch([
      env.DB.prepare(
        `UPDATE devices
            SET last_seen_at = ?, firmware_version = COALESCE(?, firmware_version),
                ip_address = ?, wifi_ssid = COALESCE(?, wifi_ssid), wifi_pass = COALESCE(?, wifi_pass),
                supported_commands = COALESCE(?, supported_commands), updated_at = ?
          WHERE device_id = ?`
      ).bind(now, optionalNullableString(body.firmwareVersion), ip, optionalNullableString(body.ssid), optionalNullableString(body.pass), optionalNullableString(body.supportedCommands ? JSON.stringify(body.supportedCommands) : null), now, device.device_id),
      env.DB.prepare(
        `INSERT INTO device_heartbeats(device_id, received_at, rssi, uptime_ms, free_heap, firmware_version, ip_address, payload)
         VALUES(?, ?, ?, ?, ?, ?, ?, ?)`
      ).bind(
        device.device_id,
        now,
        nullableNumber(body.rssi),
        nullableNumber(body.uptimeMs),
        nullableNumber(body.freeHeap),
        optionalNullableString(body.firmwareVersion),
        ip,
        JSON.stringify(body)
      ),
    ]);

    return json({ ok: true, serverTime: now });
  }

  if (url.pathname === "/api/devices/logs" && request.method === "POST") {
    const body = await readJson(request);
    const logs = Array.isArray(body) ? body : [body];
    
    if (logs.length > 0) {
      const stmt = env.DB.prepare(`INSERT INTO device_logs(device_id, level, message, payload, created_at) VALUES(?, ?, ?, ?, ?)`);
      const batch = logs.map(log => 
        stmt.bind(
          device.device_id,
          optionalString(log.level, "info"),
          requiredString(log.message, "message"),
          JSON.stringify(log.payload || {}),
          new Date().toISOString()
        )
      );
      await env.DB.batch(batch);
    }
    return json({ ok: true, saved: logs.length });
  }

  if (url.pathname === "/api/devices/commands/next" && request.method === "GET") {
    const command = await env.DB.prepare(
      `SELECT command_id, type, payload
         FROM device_commands
        WHERE device_id = ? AND status = 'queued'
        ORDER BY created_at ASC
        LIMIT 1`
    )
      .bind(device.device_id)
      .first();

    if (!command) {
      return json({ command: null });
    }

    await env.DB.prepare(
      `UPDATE device_commands
          SET status = 'delivered', delivered_at = ?
        WHERE command_id = ? AND status = 'queued'`
    )
      .bind(new Date().toISOString(), command.command_id)
      .run();

    return json({
      command: {
        commandId: command.command_id,
        type: command.type,
        payload: parseStoredJson(command.payload),
      },
    });
  }

  const ackMatch = url.pathname.match(/^\/api\/devices\/commands\/([^/]+)\/ack$/);
  if (ackMatch && request.method === "POST") {
    const body = await readJson(request);
    const status = body.ok === false ? "failed" : "done";

    await env.DB.prepare(
      `UPDATE device_commands
          SET status = ?, result = ?, completed_at = ?
        WHERE command_id = ? AND device_id = ?`
    )
      .bind(status, JSON.stringify(body), new Date().toISOString(), ackMatch[1], device.device_id)
      .run();

    return json({ ok: true });
  }

  if (url.pathname === "/api/devices/ota" && request.method === "GET") {
    const currentVersion = url.searchParams.get("currentVersion") || device.firmware_version || "";
    const release = await env.DB.prepare(
      `SELECT id, target_version, url, sha256, notes, required, created_at
         FROM firmware_releases
        WHERE is_active = 1
        ORDER BY id DESC
        LIMIT 1`
    ).first();

    if (!release || release.target_version === currentVersion) {
      return json({ updateAvailable: false });
    }

    return json({
      updateAvailable: true,
      version: release.target_version,
      url: release.url,
      sha256: release.sha256,
      required: Boolean(release.required),
      notes: release.notes,
    });
  }

  return json({ error: "not_found" }, 404);
}

async function requireDevice(request, env) {
  const deviceId = request.headers.get("X-Device-Id");
  const secret = request.headers.get("X-Device-Secret");

  if (!deviceId || !secret) {
    throw new HttpError(401, "device_auth_required");
  }

  const device = await env.DB.prepare(
    "SELECT device_id, secret_hash, firmware_version, status FROM devices WHERE device_id = ?"
  )
    .bind(deviceId)
    .first();

  if (!device || device.status !== "active") {
    throw new HttpError(401, "device_not_allowed");
  }

  const expected = await hashDeviceSecret(deviceId, secret);
  if (device.secret_hash !== expected) {
    throw new HttpError(401, "invalid_device_secret");
  }

  return device;
}

function requireAdmin(request, env) {
  const token = env.ADMIN_TOKEN;
  if (!token) {
    throw new HttpError(500, "admin_token_not_configured");
  }

  const header = request.headers.get("Authorization") || "";
  if (header !== `Bearer ${token}`) {
    throw new HttpError(401, "admin_auth_required");
  }
}

function requireBootstrapToken(request, env) {
  const token = env.DEVICE_BOOTSTRAP_TOKEN;
  if (!token) {
    throw new HttpError(500, "bootstrap_token_not_configured");
  }

  if (request.headers.get("X-Bootstrap-Token") !== token) {
    throw new HttpError(401, "bootstrap_auth_required");
  }
}

function requireDatabase(env) {
  if (!env.DB) {
    throw new HttpError(500, "d1_binding_missing");
  }
}

async function readJson(request) {
  const text = await request.text();
  if (!text.trim()) {
    return {};
  }

  try {
    return JSON.parse(text);
  } catch (error) {
    throw new HttpError(400, "invalid_json");
  }
}

function json(body, status = 200) {
  return new Response(JSON.stringify(body), { status, headers: JSON_HEADERS });
}

function html(body, status = 200) {
  return new Response(body, {
    status,
    headers: {
      "Content-Type": "text/html; charset=utf-8",
      "Cache-Control": "no-store",
    },
  });
}

async function hashDeviceSecret(deviceId, secret) {
  const data = new TextEncoder().encode(`${deviceId}:${secret}`);
  const digest = await crypto.subtle.digest("SHA-256", data);
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

function requiredString(value, name) {
  if (typeof value !== "string" || value.trim() === "") {
    throw new HttpError(400, `${name}_required`);
  }
  return value.trim();
}

function optionalString(value, fallback) {
  return typeof value === "string" && value.trim() !== "" ? value.trim() : fallback;
}

function optionalNullableString(value) {
  return typeof value === "string" && value.trim() !== "" ? value.trim() : null;
}

function nullableNumber(value) {
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function clampNumber(value, min, max) {
  if (!Number.isFinite(value)) {
    return min;
  }
  return Math.max(min, Math.min(max, Math.floor(value)));
}

function parseStoredJson(value) {
  try {
    return JSON.parse(value || "{}");
  } catch (error) {
    return {};
  }
}

class HttpError extends Error {
  constructor(status, message) {
    super(message);
    this.status = status;
  }
}

function adminHtml() {
  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Cloud Manager</title>
  <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
  <style>
    :root { 
      --bg: #0f1115; 
      --surface: rgba(25, 28, 35, 0.7); 
      --surface-border: rgba(255, 255, 255, 0.08);
      --text: #e2e8f0; 
      --muted: #94a3b8; 
      --accent: #3b82f6; 
      --accent-hover: #2563eb;
      --ok: #10b981; 
      --bad: #ef4444; 
      --warn: #f59e0b; 
    }
    * { box-sizing: border-box; }
    body { 
      margin: 0; 
      font-family: 'Inter', sans-serif; 
      color: var(--text); 
      background: var(--bg);
      background-image: 
        radial-gradient(at 0% 0%, rgba(59, 130, 246, 0.15) 0px, transparent 50%),
        radial-gradient(at 100% 0%, rgba(16, 185, 129, 0.1) 0px, transparent 50%);
      background-attachment: fixed;
      min-height: 100vh;
    }
    ::-webkit-scrollbar { width: 8px; height: 8px; }
    ::-webkit-scrollbar-track { background: transparent; }
    ::-webkit-scrollbar-thumb { background: rgba(255, 255, 255, 0.2); border-radius: 4px; }
    
    header { 
      display: flex; align-items: center; justify-content: space-between; gap: 16px; 
      padding: 20px 32px; 
      background: rgba(15, 17, 21, 0.8);
      backdrop-filter: blur(12px);
      -webkit-backdrop-filter: blur(12px);
      border-bottom: 1px solid var(--surface-border); 
      position: sticky; top: 0; z-index: 10; 
    }
    h1 { font-size: 20px; margin: 0; font-weight: 600; letter-spacing: -0.02em; background: linear-gradient(to right, #fff, #94a3b8); -webkit-background-clip: text; -webkit-text-fill-color: transparent; }
    main { padding: 32px; display: grid; grid-template-columns: 340px minmax(0, 1fr); gap: 24px; max-width: 1600px; margin: 0 auto; }
    
    section, aside { 
      background: var(--surface); 
      backdrop-filter: blur(16px);
      -webkit-backdrop-filter: blur(16px);
      border: 1px solid var(--surface-border); 
      border-radius: 16px; 
      padding: 24px; 
      box-shadow: 0 4px 24px -1px rgba(0,0,0,0.2);
    }
    
    h2 { font-size: 16px; margin: 0 0 16px; font-weight: 600; display: flex; align-items: center; gap: 8px; }
    h2::before { content: ''; display: block; width: 4px; height: 16px; background: var(--accent); border-radius: 2px; }
    hr { border: 0; height: 1px; background: var(--surface-border); margin: 24px 0; }
    
    label { display: block; color: var(--muted); font-size: 13px; margin: 12px 0 6px; font-weight: 500; }
    input, select, textarea { 
      width: 100%; border: 1px solid var(--surface-border); border-radius: 8px; 
      padding: 10px 12px; font-family: inherit; font-size: 14px;
      background: rgba(0, 0, 0, 0.2); color: var(--text);
      transition: all 0.2s; outline: none;
    }
    input:focus, select:focus, textarea:focus { border-color: var(--accent); box-shadow: 0 0 0 3px rgba(59, 130, 246, 0.2); }
    textarea { min-height: 80px; resize: vertical; }
    option { background: var(--bg); color: var(--text); }
    
    button { 
      border: 0; border-radius: 8px; padding: 10px 16px; 
      background: var(--accent); color: white; font-weight: 600; font-size: 14px;
      cursor: pointer; transition: all 0.2s; width: 100%; display: flex; justify-content: center; align-items: center; gap: 8px;
    }
    button:hover { background: var(--accent-hover); transform: translateY(-1px); }
    button:active { transform: translateY(0); }
    button.secondary { background: rgba(255, 255, 255, 0.1); width: auto; }
    button.secondary:hover { background: rgba(255, 255, 255, 0.15); }
    button.warn { background: var(--warn); }
    button.warn:hover { background: #d97706; }
    
    .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 24px; }
    .row { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; }
    .muted { color: var(--muted); font-size: 13px; }
    
    .status { font-weight: 600; padding: 4px 8px; border-radius: 6px; font-size: 12px; display: inline-block; }
    .status.active { background: rgba(16, 185, 129, 0.15); color: var(--ok); border: 1px solid rgba(16, 185, 129, 0.2); }
    .status.queued { background: rgba(245, 158, 11, 0.15); color: var(--warn); border: 1px solid rgba(245, 158, 11, 0.2); }
    
    .ok { color: var(--ok); }
    .bad { color: var(--bad); }
    
    .table-container { overflow-x: auto; margin-top: 8px; }
    table { width: 100%; border-collapse: separate; border-spacing: 0; }
    th, td { padding: 12px 16px; text-align: left; vertical-align: middle; border-bottom: 1px solid var(--surface-border); }
    th { font-size: 12px; color: var(--muted); font-weight: 600; text-transform: uppercase; letter-spacing: 0.05em; border-bottom: 2px solid var(--surface-border); white-space: nowrap; }
    td { font-size: 14px; }
    tr { transition: background 0.2s; }
    tr:hover td { background: rgba(255, 255, 255, 0.03); }
    
    pre { 
      max-height: 400px; overflow: auto; background: rgba(0, 0, 0, 0.4); color: #a5b4fc; 
      padding: 16px; border-radius: 12px; font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
      font-size: 13px; line-height: 1.6; border: 1px solid var(--surface-border);
    }
    
    .pill { display: inline-flex; align-items: center; padding: 6px 12px; background: rgba(255, 255, 255, 0.05); border: 1px solid var(--surface-border); border-radius: 20px; font-size: 13px; font-weight: 500; }
    .pill .dot { width: 8px; height: 8px; border-radius: 50%; margin-right: 8px; }
    .pill .dot.ok { background: var(--ok); box-shadow: 0 0 8px var(--ok); }
    .pill .dot.bad { background: var(--bad); box-shadow: 0 0 8px var(--bad); }

    @media (max-width: 900px) { 
      main { grid-template-columns: 1fr; padding: 16px; } 
      header { flex-direction: column; align-items: stretch; }
      .row { flex-direction: column; align-items: stretch; }
      button.secondary { width: 100%; }
      #token { width: 100% !important; }
    }
    
    @keyframes fadeIn { from { opacity: 0; transform: translateY(10px); } to { opacity: 1; transform: translateY(0); } }
    main > * { animation: fadeIn 0.4s ease-out forwards; }
    main > *:nth-child(2) { animation-delay: 0.1s; }
  </style>
</head>
<body>
  <header>
    <div>
      <h1>ESP32 Cloud Manager <span style="font-size: 14px; font-weight: 500; color: var(--muted); vertical-align: middle; margin-left: 8px; padding: 2px 6px; border-radius: 6px; background: var(--surface-border);">v1.0.0</span></h1>
    </div>
    <div class="row">
      <div class="pill" id="health-pill">
        <div class="dot bad" id="health-dot"></div>
        <span id="health">Checking service...</span>
      </div>
      <div style="display: flex; gap: 8px;">
        <input id="token" type="password" placeholder="Admin token" autocomplete="current-password" style="width: 200px;">
        <button id="saveToken" class="secondary" onclick="if(window.submitAuth) window.submitAuth()">Authenticate</button>
      </div>
    </div>
  </header>
  <main>
    <aside>
      <h2>Register Device</h2>
      <label>Device ID</label><input id="deviceId" placeholder="esp32-kitchen-01">
      <label>Secret</label><input id="deviceSecret" type="password" placeholder="Strong per-device secret">
      <label>Firmware Version</label><input id="deviceFirmware" value="0.1.0">
      <button id="createDevice" style="margin-top: 16px;">Save Device</button>
      
      <hr>
      <h2>Queue Command</h2>
      <label>Device</label><select id="commandDevice"></select>
      <label>Type</label>
      <select id="commandType">
        <option value="reboot">reboot</option>
        <option value="shutdown">shutdown</option>
        <option value="update_wifi">update_wifi</option>
        <option value="sync_forms">sync_forms</option>
        <option value="view_tokens">view_tokens</option>
        <option value="reset_token">reset_token</option>
      </select>
      <label>Payload (JSON)</label><textarea id="commandPayload">{}</textarea>
      <button id="queueCommand" class="warn" style="margin-top: 16px;">Queue Command</button>
      
      <hr>
      <h2>Firmware Release</h2>
      <label>Target Version</label><input id="fwVersion" placeholder="1.0.0">
      <label>Upload .bin File</label><input type="file" id="fwFile" accept=".bin">
      <button id="publishFirmware" style="margin-top: 16px;">Upload & Publish</button>
    </aside>
    <section>
      <div class="grid">
        <div style="grid-column: 1 / -1;">
          <h2>Fleet Status</h2>
          <div class="table-container">
            <table>
              <thead><tr><th>ID</th><th>Status</th><th>Firmware</th><th>Wi-Fi Network</th><th>Last Seen</th><th>Actions</th></tr></thead>
              <tbody id="devices"></tbody>
            </table>
          </div>
        </div>
      </div>
      <div style="display: flex; justify-content: space-between; align-items: center; margin-top: 32px; margin-bottom: 16px;">
        <h2 style="margin: 0;">Real-time Logs</h2>
        <div style="display: flex; gap: 8px;">
          <button class="secondary" style="padding: 6px 12px; font-size: 12px; width: auto; margin: 0;" onclick="state.logClearTime=Date.now(); document.getElementById('logs').innerHTML=''">Clear</button>
          <button class="secondary" style="padding: 6px 12px; font-size: 12px; width: auto; margin: 0;" onclick="navigator.clipboard.writeText(document.getElementById('logs').innerText).then(()=>alert('Copied!'))">Copy</button>
        </div>
      </div>
      <pre id="logs"></pre>
    </section>
  </main>
  <script>
    const state = { devices: [], logClearTime: 0, localLogs: [] };
    const token = document.getElementById("token");
    try { token.value = localStorage.getItem("esp32_admin_token") || ""; } catch(e) {}

    const api = async (path, options = {}) => {
      const response = await fetch(path, {
        ...options,
        headers: {
          "Content-Type": "application/json",
          "Authorization": "Bearer " + token.value,
          ...(options.headers || {})
        }
      });
      const data = await response.json();
      if (!response.ok) throw new Error(data.error || "request_failed");
      return data;
    };

    const formatDate = (iso) => {
      if(!iso) return "never";
      const d = new Date(iso);
      return d.toLocaleString();
    };

    window.showError = (error) => { 
      const msg = error && error.message ? error.message : String(error);
      const h = document.getElementById("health");
      const d = document.getElementById("health-dot");
      if (h) h.textContent = msg; 
      if (d) d.className = "dot bad"; 
      console.error("UI Error:", error);
    };
    
    window.refresh = async () => {
      try {
        let health;
        try {
          health = await fetch("/api/health").then(r => r.json());
        } catch(e) {
          throw new Error("Cannot connect to API");
        }
        
        const h = document.getElementById("health");
        const d = document.getElementById("health-dot");
        if (h) h.textContent = health.d1 ? "D1 connected" : "D1 missing";
        if (d) d.className = "dot " + (health.d1 ? "ok" : "bad");
        
        if (!token.value) {
           if (h) h.textContent = "Auth required";
           if (d) d.className = "dot warn";
           return;
        }

        const [devices, commands, logs] = await Promise.all([
          api("/api/admin/devices"),
          api("/api/admin/commands"),
          api("/api/admin/logs?limit=100")
        ]);
        
        state.devices = devices.devices;
        renderDevices(devices.devices);
        renderCommands(commands.commands);
        renderLogs(logs.logs);
      } catch (err) {
        window.showError(err);
      }
    };

    const renderDevices = (items) => {
      document.getElementById("devices").innerHTML = items.map(d =>
        "<tr><td><div style='font-weight:600; color:var(--text);'>" + esc(d.device_id) + "</div></td><td><span class='status " + esc(d.status) + "'>" + esc(d.status) + "</span></td><td>" + esc(d.firmware_version || "") + "</td><td><div style='font-weight:500;'>" + (d.wifi_ssid ? esc(d.wifi_ssid) : "—") + "</div><div class='muted' style='margin-top:4px; font-family:monospace;'>" + (d.wifi_pass ? "***" : "—") + "</div></td><td class='muted'>" + formatDate(d.last_seen_at) + "</td><td><div style='display:flex; gap:4px;'><button class='secondary' onclick='editDevice(\\"" + escAttr(d.device_id) + "\\")' title='Edit' style='padding:4px 8px;'>✏️</button><button class='secondary' onclick='delDevice(\\"" + escAttr(d.device_id) + "\\")' title='Delete' style='padding:4px 8px;'>🗑️</button><button class='secondary' onclick='queue(\\"" + escAttr(d.device_id) + "\\",\\"reboot\\")' title='Reboot' style='padding:4px 8px;'>🔄</button><button class='secondary' onclick='queue(\\"" + escAttr(d.device_id) + "\\",\\"shutdown\\")' title='Sleep/Shutdown' style='padding:4px 8px;'>💤</button></div></td></tr>"
      ).join("");
      
      document.getElementById("commandDevice").innerHTML = items.map(d =>
        "<option value='" + escAttr(d.device_id) + "'>" + esc(d.device_id) + "</option>"
      ).join("");
    };

    window.editDevice = (id) => {
      const d = state.devices.find(x => x.device_id === id);
      if(!d) return;
      document.getElementById("deviceId").value = d.device_id;
      document.getElementById("deviceFirmware").value = d.firmware_version;
      document.getElementById("deviceSecret").value = "";
      if (d.wifi_ssid) {
        const ssid = prompt("Set new Wi-Fi SSID for " + d.device_id, d.wifi_ssid);
        if (ssid !== null) {
          const pass = prompt("Set new Wi-Fi Password for " + d.device_id, "");
          if (pass !== null) {
             api("/api/admin/commands", { method: "POST", body: JSON.stringify({
               deviceId: id, type: "update_wifi", payload: { ssid, pass }
             }) }).then(() => { alert("Wi-Fi update command queued!"); refresh(); }).catch(showError);
          }
        }
      } else {
        document.getElementById("deviceId").focus();
      }
    };

    window.delDevice = (id) => {
      if(confirm("Delete device " + id + "?")) {
        api("/api/admin/devices", { method: "DELETE", body: JSON.stringify({ deviceId: id }) })
          .then(() => refresh()).catch(showError);
      }
    };

    window.queue = (id, type) => {
      api("/api/admin/commands", { method: "POST", body: JSON.stringify({
        deviceId: id, type: type, payload: {}
      }) }).then(() => {
        state.localLogs.push({ created_at: new Date().toISOString(), device_id: id, level: "CMD", message: "Queued: " + type });
        refresh();
      }).catch(showError);
    };

    const renderCommands = (items) => {};

    const renderLogs = (items) => {
      const logsEl = document.getElementById("logs");
      if (!logsEl) return;
      const allLogs = [...state.localLogs, ...items];
      allLogs.sort((a, b) => new Date(b.created_at) - new Date(a.created_at));
      const filtered = allLogs.filter(l => new Date(l.created_at).getTime() > state.logClearTime);
      logsEl.innerHTML = filtered.slice(0, 200).map(l =>
        "<span style='color:var(--muted)'>[" + formatDate(l.created_at) + "]</span> <span style='color:#fcd34d;'>" + esc(l.device_id) + "</span> <span style='color:var(--ok)'>" + esc(l.level) + "</span>: " + esc(l.message)
      ).join("\\n");
    };

    window.submitAuth = () => {
      try { localStorage.setItem("esp32_admin_token", token.value); } catch(e) {}
      const h = document.getElementById("health");
      if (h) h.textContent = "Loading...";
      window.refresh();
    };

    const tokenEl = document.getElementById("token");
    if (tokenEl) {
      tokenEl.onkeydown = (e) => { if(e.key === "Enter") window.submitAuth(); };
    }

    const btnCreate = document.getElementById("createDevice");
    if (btnCreate) btnCreate.onclick = async () => {
      try {
        await api("/api/admin/devices", { method: "POST", body: JSON.stringify({
          deviceId: value("deviceId"), secret: value("deviceSecret"),
          firmwareVersion: value("deviceFirmware")
        }) });
        await refresh();
      } catch(e) { alert(e.message); }
    };

    document.getElementById("queueCommand").onclick = async () => {
      try {
        const type = value("commandType");
        const devId = value("commandDevice");
        await api("/api/admin/commands", { method: "POST", body: JSON.stringify({
          deviceId: devId, type: type,
          payload: JSON.parse(value("commandPayload") || "{}")
        }) });
        state.localLogs.push({ created_at: new Date().toISOString(), device_id: devId, level: "CMD", message: "Queued: " + type });
        await refresh();
      } catch(e) { alert(e.message); }
    };

    document.getElementById("publishFirmware").onclick = async () => {
      try {
        const fileInput = document.getElementById("fwFile");
        if (!fileInput.files.length) return alert("Select a .bin file");
        const version = value("fwVersion");
        if (!version) return alert("Enter target version");
        
        document.getElementById("publishFirmware").textContent = "Uploading...";
        const res = await fetch("/api/admin/firmware/upload?version=" + encodeURIComponent(version), {
          method: "POST",
          headers: { "Authorization": "Bearer " + token.value },
          body: fileInput.files[0]
        });
        const data = await res.json();
        if (!res.ok) throw new Error(data.error);
        alert("Firmware published! URL: " + data.url);
        document.getElementById("publishFirmware").textContent = "Upload & Publish";
        await refresh();
      } catch(e) { 
        alert(e.message); 
        document.getElementById("publishFirmware").textContent = "Upload & Publish";
      }
    };

    const value = (id) => {
      const el = document.getElementById(id);
      return el ? el.value.trim() : "";
    };
    const esc = (s) => String(s !== null && s !== undefined ? s : "").replace(/[&<>"']/g, ch => ({ "&":"&amp;", "<":"&lt;", ">":"&gt;", '"':"&quot;", "'":"&#39;" }[ch]));
    window.escAttr = esc;
    
    const cmdPayloads = {
      update_wifi: "{\\n  \\\"ssid\\\": \\\"WIFI_NAME\\\",\\n  \\\"pass\\\": \\\"WIFI_PASSWORD\\\"\\n}",
      sync_forms: "{\\n  \\\"form_id\\\": 12345\\n}"
    };
    const cType = document.getElementById("commandType");
    if (cType) {
      cType.onchange = (e) => {
        const p = cmdPayloads[e.target.value] || "{}";
        const el = document.getElementById("commandPayload");
        if (el) el.value = p;
      };
    }
    
    try {
      window.refresh();
      setInterval(window.refresh, 15000);
    } catch(e) {
      window.showError(e);
    }
  </script>
</body>
</html>`;
}
