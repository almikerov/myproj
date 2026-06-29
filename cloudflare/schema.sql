CREATE TABLE IF NOT EXISTS devices (
  device_id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  secret_hash TEXT NOT NULL,
  model TEXT NOT NULL DEFAULT 'esp32',
  firmware_version TEXT NOT NULL DEFAULT 'unknown',
  ip_address TEXT NOT NULL DEFAULT '',
  status TEXT NOT NULL DEFAULT 'active',
  last_seen_at TEXT,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS device_heartbeats (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  received_at TEXT NOT NULL,
  rssi INTEGER,
  uptime_ms INTEGER,
  free_heap INTEGER,
  firmware_version TEXT,
  ip_address TEXT NOT NULL DEFAULT '',
  payload TEXT NOT NULL,
  FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_device_heartbeats_device_time
  ON device_heartbeats(device_id, received_at DESC);

CREATE TABLE IF NOT EXISTS device_logs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  level TEXT NOT NULL DEFAULT 'info',
  message TEXT NOT NULL,
  payload TEXT NOT NULL DEFAULT '{}',
  created_at TEXT NOT NULL,
  FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_device_logs_device_time
  ON device_logs(device_id, created_at DESC);

CREATE TABLE IF NOT EXISTS device_commands (
  command_id TEXT PRIMARY KEY,
  device_id TEXT NOT NULL,
  type TEXT NOT NULL,
  payload TEXT NOT NULL DEFAULT '{}',
  status TEXT NOT NULL DEFAULT 'queued',
  result TEXT,
  created_at TEXT NOT NULL,
  delivered_at TEXT,
  completed_at TEXT,
  FOREIGN KEY (device_id) REFERENCES devices(device_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_device_commands_device_status
  ON device_commands(device_id, status, created_at ASC);

CREATE TABLE IF NOT EXISTS firmware_releases (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  target_version TEXT NOT NULL,
  url TEXT NOT NULL,
  sha256 TEXT NOT NULL DEFAULT '',
  notes TEXT NOT NULL DEFAULT '',
  required INTEGER NOT NULL DEFAULT 0,
  is_active INTEGER NOT NULL DEFAULT 1,
  created_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_firmware_releases_active
  ON firmware_releases(is_active, id DESC);
