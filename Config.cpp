#include "Config.h"

ConfigManager Config;

void ConfigManager::begin() {
    preferences.begin("config", false);
    router_ssid = preferences.getString("ssid", "ASUS");
    router_pass = preferences.getString("pass", "19642005");
    ap_name = preferences.getString("ap", "Base_Opros_V25");
    admin_pass = preferences.getString("admin_pass", "19642005");
    
    device_id = preferences.getString("dev_id", "");
    device_secret = preferences.getString("dev_sec", "");
    cloud_url = preferences.getString("cloud_url", "https://your-worker.workers.dev");

    usedTokensStr = preferences.getString("tokens", "");
    lastUpdateDay = preferences.getInt("last_day", -1);
}

void ConfigManager::saveWifi(const String& ssid, const String& pass, const String& ap) {
    router_ssid = ssid;
    router_pass = pass;
    ap_name = ap;
    preferences.putString("ssid", ssid);
    preferences.putString("pass", pass);
    preferences.putString("ap", ap);
}

void ConfigManager::saveAdminPass(const String& pass) {
    admin_pass = pass;
    preferences.putString("admin_pass", pass);
}

void ConfigManager::saveCloud(const String& id, const String& secret, const String& url) {
    device_id = id;
    device_secret = secret;
    cloud_url = url;
    preferences.putString("dev_id", id);
    preferences.putString("dev_sec", secret);
    preferences.putString("cloud_url", url);
}

void ConfigManager::saveTokens() {
    preferences.putString("tokens", usedTokensStr);
}

void ConfigManager::clearAllTokens() {
    usedTokensStr = "";
    saveTokens();
}

void ConfigManager::markTokenUsed(const String& entry) {
    if (usedTokensStr.length() == 0) usedTokensStr = ",";
    usedTokensStr += entry + ","; 
    saveTokens();
}

void ConfigManager::freeToken(const String& t) {
    for(int i = 0; i < 10; i++) {
        String searchStr = "," + String(i) + "_" + t + ",";
        while(usedTokensStr.indexOf(searchStr) != -1) {
            usedTokensStr.replace(searchStr, ",");
        }
    }
    saveTokens();
}

int ConfigManager::getTokenUsageCount(const String& entry) {
    String searchStr = "," + entry + ",";
    int count = 0; 
    int pos = usedTokensStr.indexOf(searchStr);
    while (pos != -1) { 
        count++; 
        pos = usedTokensStr.indexOf(searchStr, pos + 1); 
    }
    return count;
}

void ConfigManager::updateDay(int newDay) {
    lastUpdateDay = newDay;
    preferences.putInt("last_day", newDay);
}
