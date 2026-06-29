# ESP32 Cloud Worker

This Worker is the cloud control plane for the ESP32 fleet. It keeps the old
Google Apps Script form flow available while adding D1-backed devices,
heartbeats, cloud logs, queued commands, firmware release metadata, and a plain
HTML admin panel.

## Bindings and secrets

Create a D1 database and apply the schema:

```powershell
wrangler d1 create esp32-cloud
wrangler d1 execute esp32-cloud --file cloudflare/schema.sql
```

Copy `cloudflare/wrangler.toml.example` to `wrangler.toml`, set the D1
`database_id`, then configure secrets:

```powershell
wrangler secret put ADMIN_TOKEN
wrangler secret put DEVICE_BOOTSTRAP_TOKEN
```

Deploy:

```powershell
wrangler deploy
```

## Device authentication

Devices authenticate with:

```http
X-Device-Id: esp32-kitchen-01
X-Device-Secret: per-device-secret
```

The Worker stores only `SHA-256(deviceId + ":" + secret)`.

## Main endpoints

- `GET /admin` - browser admin panel.
- `GET /api/health` - service health.
- `POST /api/admin/devices` - create or rotate a device secret.
- `POST /api/devices/heartbeat` - authenticated device heartbeat.
- `POST /api/devices/logs` - authenticated cloud log upload.
- `GET /api/devices/commands/next` - fetch the next queued command.
- `POST /api/devices/commands/{id}/ack` - acknowledge command result.
- `GET /api/devices/ota` - check active firmware release.

Legacy firmware requests using `?action=getFormCount`, `?action=getForm`, and
root `POST` submissions are still proxied to Google Apps Script.
