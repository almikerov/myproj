#ifndef ESPNETWORK_H
#define ESPNETWORK_H

#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <esp_task_wdt.h>

class ESPNetworkManager {
public:
    void begin();
    void loop();
    
    bool isConnected();
    String getIP();
    
private:
    void setupAP();
    void syncTime();
    
    DNSServer dnsServer;
};

extern ESPNetworkManager ESPNetwork;

#endif
