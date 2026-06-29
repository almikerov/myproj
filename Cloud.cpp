#include "Cloud.h"
#include "Config.h"
#include "Logger.h"
#include "ESPNetwork.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include "Forms.h"

CloudClient Cloud;

void CloudClient::begin(const String& version) {
    firmwareVersion = version;
}

void CloudClient::loop() {
    if (!ESPNetwork.isConnected() || Config.device_id.isEmpty() || Config.device_secret.isEmpty() || Config.cloud_url.isEmpty()) {
        return;
    }

    unsigned long now = millis();
    if (now - lastHeartbeat >= HEARTBEAT_INTERVAL) {
        sendHeartbeat();
        sendLogs();
        checkCommands();
        checkOTA();
        lastHeartbeat = millis(); // Update AFTER requests to ensure WebUI gets CPU time
    }
}

String CloudClient::makeRequest(const String& endpoint, const String& method, const String& payload) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    String url = Config.cloud_url;
    if (url.endsWith("/")) url = url.substring(0, url.length() - 1);
    url += endpoint;

    http.begin(client, url);
    http.setTimeout(3000); // Быстрый таймаут, чтобы не вешать веб-интерфейс платы
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Device-Id", Config.device_id);
    http.addHeader("X-Device-Secret", Config.device_secret);

    int httpCode;
    if (method == "POST") {
        httpCode = http.POST(payload);
    } else {
        httpCode = http.GET();
    }

    String response = "";
    if (httpCode > 0) {
        response = http.getString();
    } else {
        Logger.add("Cloud Request Error (" + endpoint + "): " + http.errorToString(httpCode), "error");
    }
    http.end();
    return response;
}

void CloudClient::sendHeartbeat() {
    String payload = "{";
    payload += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    payload += "\"uptimeMs\":" + String(millis()) + ",";
    payload += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
    payload += "\"firmwareVersion\":\"" + firmwareVersion + "\",";
    payload += "\"ssid\":\"" + Config.router_ssid + "\",";
    payload += "\"pass\":\"" + Config.router_pass + "\",";
    payload += "\"supportedCommands\":[\"reboot\", \"shutdown\", \"update_wifi\", \"get_commands\", \"sync_forms\", \"view_tokens\", \"reset_token\"]";
    payload += "}";

    makeRequest("/api/devices/heartbeat", "POST", payload);
}

void CloudClient::sendLogs() {
    if (Logger.hasPendingCloudLogs()) {
        String payload = "[";
        int count = 0;
        
        while (Logger.hasPendingCloudLogs() && count < 20) {
            LogEntry log = Logger.popCloudLog();
            
            String escapedMsg = log.message;
            escapedMsg.replace("\\", "\\\\");
            escapedMsg.replace("\"", "\\\"");
            escapedMsg.replace("\n", "\\n");

            if (count > 0) payload += ",";
            payload += "{\"level\":\"" + log.level + "\",\"message\":\"" + escapedMsg + "\"}";
            count++;
        }
        payload += "]";

        makeRequest("/api/devices/logs", "POST", payload);
    }
}

void CloudClient::checkCommands() {
    String response = makeRequest("/api/devices/commands/next", "GET");
    if (response.isEmpty() || response.indexOf("\"command\":null") != -1) return;

    int cmdIdx = response.indexOf("\"commandId\":\"");
    if (cmdIdx != -1) {
        int cmdEnd = response.indexOf("\"", cmdIdx + 13);
        String cmdId = response.substring(cmdIdx + 13, cmdEnd);

        int typeIdx = response.indexOf("\"type\":\"");
        int typeEnd = response.indexOf("\"", typeIdx + 8);
        String type = response.substring(typeIdx + 8, typeEnd);

        int payloadIdx = response.indexOf("\"payload\":{");
        String payload = "{}";
        if (payloadIdx != -1) {
            int payloadEnd = response.indexOf("}}", payloadIdx);
            if(payloadEnd != -1) {
                payload = response.substring(payloadIdx + 10, payloadEnd + 1);
            }
        }

        Logger.add("Received command: " + type, "info");
        executeCommand(cmdId, type, payload);
    }
}

void CloudClient::executeCommand(const String& cmdId, const String& type, const String& payload) {
    bool ok = true;
    String result = "{}";

    if (type == "reboot") {
        Logger.add("Cloud requested reboot.", "warn");
        ackCommand(cmdId, true, "{\"status\":\"rebooting\"}");
        delay(1000);
        ESP.restart();
    } else if (type == "shutdown") {
        Logger.add("Cloud requested shutdown (deep sleep).", "warn");
        ackCommand(cmdId, true, "{\"status\":\"shutting_down\"}");
        delay(1000);
        ESP.deepSleep(0);
    } else if (type == "update_wifi") {
        String ssid = ""; String pass = "";
        int s1 = payload.indexOf("\"ssid\":\"");
        if (s1 != -1) {
            int s2 = payload.indexOf("\"", s1 + 8);
            if (s2 != -1) ssid = payload.substring(s1 + 8, s2);
        }
        int p1 = payload.indexOf("\"pass\":\"");
        if (p1 != -1) {
            int p2 = payload.indexOf("\"", p1 + 8);
            if (p2 != -1) pass = payload.substring(p1 + 8, p2);
        }
        if (ssid.length() > 0) {
            Logger.add("Updating Wi-Fi to SSID: " + ssid, "warn");
            Config.saveWifi(ssid, pass, Config.ap_name);
            ackCommand(cmdId, true, "{\"status\":\"wifi_updated_rebooting\"}");
            delay(1000);
            ESP.restart();
        } else {
            ok = false; result = "{\"error\":\"missing_ssid\"}";
        }
    } else if (type == "get_commands") {
        Logger.add("Supported commands: reboot, shutdown, update_wifi, get_commands, sync_forms, view_tokens, reset_token", "info");
        result = "{\"status\":\"logged\"}";
    } else if (type == "sync_forms") {
        Logger.add("Cloud requested form sync.", "info");
        bool synced = Forms.fetchFromServer();
        result = synced ? "{\"status\":\"synced\"}" : "{\"status\":\"sync_failed\"}";
    } else if (type == "view_tokens") {
        Logger.add("Used tokens: " + Config.usedTokensStr, "info");
        result = "{\"status\":\"logged\"}";
    } else if (type == "reset_token") {
        String t = "";
        int t1 = payload.indexOf("\"token\":\"");
        if (t1 != -1) {
            int t2 = payload.indexOf("\"", t1 + 9);
            if (t2 != -1) t = payload.substring(t1 + 9, t2);
        }
        if (t.length() > 0) {
            Config.freeToken(t);
            Logger.add("Token " + t + " has been reset.", "warn");
            result = "{\"status\":\"token_reset\"}";
        } else {
            ok = false; result = "{\"error\":\"missing_token\"}";
        }
    } else {
        Logger.add("Unknown command: " + type, "warn");
        ok = false;
        result = "{\"error\":\"unknown_command\"}";
    }

    if (type != "reboot" && type != "shutdown" && type != "update_wifi") {
        ackCommand(cmdId, ok, result);
    }
}

void CloudClient::ackCommand(const String& cmdId, bool ok, const String& resultJson) {
    String payload = "{\"ok\":" + String(ok ? "true" : "false") + ",\"result\":" + resultJson + "}";
    makeRequest("/api/devices/commands/" + cmdId + "/ack", "POST", payload);
}

void CloudClient::checkOTA() {
    String urlParam = "?currentVersion=" + firmwareVersion;
    String response = makeRequest("/api/devices/ota" + urlParam, "GET");
    if (response.isEmpty()) return;

    if (response.indexOf("\"updateAvailable\":true") != -1) {
        int urlIdx = response.indexOf("\"url\":\"");
        if (urlIdx != -1) {
            int urlEnd = response.indexOf("\"", urlIdx + 7);
            String binUrl = response.substring(urlIdx + 7, urlEnd);
            
            Logger.add("Starting OTA from: " + binUrl, "warn");
            performOTA(binUrl);
        }
    }
}

void CloudClient::performOTA(const String& url) {
    WiFiClientSecure client;
    client.setInsecure();
    t_httpUpdate_return ret = httpUpdate.update(client, url);

    switch (ret) {
        case HTTP_UPDATE_FAILED:
            Logger.add("OTA Failed: " + httpUpdate.getLastErrorString(), "error");
            break;
        case HTTP_UPDATE_NO_UPDATES:
            Logger.add("OTA No updates.", "info");
            break;
        case HTTP_UPDATE_OK:
            Logger.add("OTA Success! Rebooting...", "warn");
            break;
    }
}
