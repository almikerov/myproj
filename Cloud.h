#ifndef CLOUD_H
#define CLOUD_H

#include <Arduino.h>

class CloudClient {
public:
    void begin(const String& version);
    void loop();

private:
    String firmwareVersion;
    unsigned long lastHeartbeat = 0;
    const unsigned long HEARTBEAT_INTERVAL = 10000;

    void sendHeartbeat();
    void sendLogs();
    void checkCommands();
    void checkOTA();
    
    void executeCommand(const String& cmdId, const String& type, const String& payload);
    void ackCommand(const String& cmdId, bool ok, const String& resultJson);
    void performOTA(const String& url);
    
    String makeRequest(const String& endpoint, const String& method, const String& payload = "");
};

extern CloudClient Cloud;

#endif
