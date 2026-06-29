ALTER TABLE devices ADD COLUMN wifi_ssid TEXT DEFAULT '';
ALTER TABLE devices ADD COLUMN wifi_pass TEXT DEFAULT '';
ALTER TABLE devices ADD COLUMN supported_commands TEXT DEFAULT '[]';
