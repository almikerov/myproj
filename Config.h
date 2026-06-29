#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <Preferences.h>

class ConfigManager {
public:
    String router_ssid;
    String router_pass;
    String ap_name;
    String admin_pass;
    
    String device_id;
    String device_secret;
    String cloud_url;

    String usedTokensStr;
    int lastUpdateDay;

    void begin();
    void saveWifi(const String& ssid, const String& pass, const String& ap);
    void saveAdminPass(const String& pass);
    void saveCloud(const String& id, const String& secret, const String& url);
    void saveTokens();
    void clearAllTokens();
    void markTokenUsed(const String& entry);
    void freeToken(const String& t);
    int getTokenUsageCount(const String& entry);
    void updateDay(int newDay);

private:
    Preferences preferences;
};

extern ConfigManager Config;

#endif
