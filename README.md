# ESP32 Cloud Control System

Commercial-grade ESP32 control system with a local emergency admin panel,
Cloudflare Worker API, Cloudflare D1 storage, Google Apps Script compatibility,
device heartbeats, cloud logs, command queue, OTA metadata, and multi-device
authentication.

## Current structure

- `myproj.ino` - current working ESP32 firmware with local WebServer
  admin UI, LittleFS form cache, Preferences storage, captive portal, and
  Google Forms submission flow.
- `googleScript.txt` - current Google Apps Script integration used by the
  existing form workflow.
- `cloudflare/worker.js` - canonical Cloudflare Worker source.
- `worker.txt` - deployable copy of the Worker for the legacy root-file flow.
- `cloudflare/schema.sql` - D1 database schema.
- `cloudflare/wrangler.toml.example` - Cloudflare deployment example.
- `wrangler.toml` - active Cloudflare Worker deployment config.
- `package.json` - local scripts for checking and deploying the Worker.
- `cloudflare/README.md` - Worker deployment and API notes.

## Architecture direction

The existing ESP32 form workflow remains operational through the Worker legacy
proxy. New fleet-management features are implemented in Cloudflare first:

- each device has its own ID and secret;
- the Worker stores only a hash of the device secret;
- D1 keeps device state, heartbeats, logs, queued commands, and firmware
  releases;
- the browser admin panel is plain HTML, CSS, and JavaScript served by the
  Worker;
- OTA is represented as firmware release metadata, ready for firmware-side
  download and validation logic.

The next firmware step is to add a small cloud client module to the ESP32 code
that calls `/api/devices/heartbeat`, `/api/devices/logs`,
`/api/devices/commands/next`, and `/api/devices/ota` while preserving the local
admin panel as an emergency fallback.

## One-command Cloudflare setup

Run this from the project folder:

```powershell
npm.cmd run deploy:cloudflare
```

If browser login does not work, create a Cloudflare API token and set it only in
the current PowerShell window before running the deploy command:

Set `CLOUDFLARE_API_TOKEN` in the current PowerShell window or put it in local
`config.ini`. Do not commit real tokens.

The script installs Wrangler locally, checks authentication, creates the D1
database, writes its `database_id` into `wrangler.toml`, applies the schema,
asks for Worker secrets, and deploys the Worker.

## Local ESP32 build

The project uses a local Arduino CLI installation for repeatable and fast
firmware builds. Everything is stored inside this project folder.

First-time setup:

```powershell
.\compile.bat
```

Compile only:

```powershell
.\compile.bat
```

Upload to ESP32:

```powershell
.\upload.bat
```

Open serial monitor:

```powershell
.\monitor.bat
```

The build cache lives in `.arduino/build-cache`, so repeated compiles are much
faster than a clean global Arduino IDE build.
