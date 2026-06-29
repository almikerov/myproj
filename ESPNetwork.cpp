#include "ESPNetwork.h"
#include "Config.h"
#include "Logger.h"
#include "time.h"
#include <ESPmDNS.h>
#include <esp_netif.h>

ESPNetworkManager ESPNetwork;
const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);

extern const int LED_PIN; 
extern bool ledState;     
extern int ledBlinkInterval; 

void ESPNetworkManager::begin() {
    Logger.add("Попытка подключения к Wi-Fi роутеру: [" + Config.router_ssid + "]...");
    ledBlinkInterval = 500; 
    
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(Config.router_ssid.c_str(), Config.router_pass.c_str());
    int tries = 0; 
    while (WiFi.status() != WL_CONNECTED && tries < 20) { 
        delay(500); 
        tries++; 
        ledState = !ledState; 
        digitalWrite(LED_PIN, ledState); 
        esp_task_wdt_reset(); 
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Logger.add("✅ Wi-Fi подключен! Внутренний IP: " + WiFi.localIP().toString());
        ledBlinkInterval = 0; 
        digitalWrite(LED_PIN, HIGH); 
        syncTime();
    } else {
        Logger.add("❌ Не удалось подключиться к роутеру. Отключаем режим клиента для стабильности точки доступа.");
        ledBlinkInterval = 150; 
        WiFi.disconnect(true); // Останавливаем бесконечные попытки STA подключиться
        WiFi.mode(WIFI_AP);    // Оставляем только AP, чтобы не конфликтовали антенны
    }

    setupAP();
}

void ESPNetworkManager::setupAP() {
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(Config.ap_name.c_str()); 

    // В режиме WIFI_AP_STA DNS от роутера может "протечь" в DHCP-сервер
    // точки доступа. Принудительно ставим 192.168.4.1 как DNS для клиентов AP.
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_netif_dhcps_stop(ap_netif);
        
        esp_netif_dns_info_t dns;
        memset(&dns, 0, sizeof(dns));
        dns.ip.u_addr.ip4.addr = (uint32_t)apIP;
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
        
        uint8_t dns_offer = 1;
        esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, 
            ESP_NETIF_DOMAIN_NAME_SERVER, &dns_offer, sizeof(dns_offer));
        
        esp_netif_dhcps_start(ap_netif);
        Logger.add("DHCP DNS принудительно установлен на 192.168.4.1");
    }

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(DNS_PORT, "*", apIP);
    if (MDNS.begin("arsenal")) {
        MDNS.addService("http", "tcp", 80);
        Logger.add("mDNS запущен: arsenal.local");
    }
    Logger.add("Точка доступа платы [" + Config.ap_name + "] активирована.");
}

void ESPNetworkManager::syncTime() {
    configTime(10800, 0, "pool.ntp.org"); 
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000) && timeinfo.tm_year > 120) { 
        Logger.add("✅ Время синхронизировано успешно.");
        if (Config.lastUpdateDay != -1 && timeinfo.tm_mday != Config.lastUpdateDay) {
            Logger.add("Обнаружены новые сутки! Сброс лимитов.");
            Config.clearAllTokens(); 
        }
        Config.updateDay(timeinfo.tm_mday);
    } else {
        Logger.add("❌ Ошибка синхронизации времени по NTP.");
    }
}

void ESPNetworkManager::loop() {
    dnsServer.processNextRequest(); 
    
    struct tm timeinfo;
    if (WiFi.status() == WL_CONNECTED && getLocalTime(&timeinfo, 0) && timeinfo.tm_year > 120) {
        if (Config.lastUpdateDay != -1 && timeinfo.tm_mday != Config.lastUpdateDay) {
            Logger.add("Наступил новый день (" + String(timeinfo.tm_mday) + "-е число). Авто-сброс лимитов.");
            Config.clearAllTokens(); 
            Config.updateDay(timeinfo.tm_mday);
        }
    }
}

bool ESPNetworkManager::isConnected() {
    return WiFi.status() == WL_CONNECTED;
}

String ESPNetworkManager::getIP() {
    if (isConnected()) return WiFi.localIP().toString();
    return "Disconnected";
}
