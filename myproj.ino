#include <Arduino.h>
#include <esp_task_wdt.h>
#include "Config.h"
#include "Logger.h"
#include "ESPNetwork.h"
#include "Forms.h"
#include "SubmissionQueue.h"
#include "WebUI.h"
#include "Cloud.h"

#define WDT_TIMEOUT 300 // 5 minutes
static const unsigned long FORM_VERSION_FIRST_CHECK_DELAY_MS = 60000UL;
static const unsigned long FORM_VERSION_CHECK_INTERVAL_MS = 15UL * 60UL * 1000UL;
static const unsigned long ANSWER_QUEUE_FLUSH_INTERVAL_MS = 15000UL;

// Hardware pins
const int BOOT_BUTTON_PIN = 0; 
extern const int LED_PIN = 2;         

unsigned long lastLedBlink = 0;
bool ledState = false;
int ledBlinkInterval = 500; 
unsigned long buttonPressTime = 0;
bool buttonPressed = false;

// ============================================================
// Фоновый поток для ВСЕХ блокирующих сетевых операций.
// Работает на ядре 0, чтобы веб-сервер на ядре 1 
// ВСЕГДА оставался отзывчивым.
// ============================================================
void backgroundNetworkTask(void *parameter) {
    // Ждём 8 секунд после старта, чтобы captive portal 
    // успел обработать первые запросы устройств
    vTaskDelay(8000 / portTICK_PERIOD_MS);
    
    if (Forms.formCount > 0 && Forms.readyCount() >= Forms.formCount) {
        Logger.add("Фоновый поток: готовый кэш опросников найден, стартовая загрузка не нужна.");
    } else {
        Logger.add("Фоновый поток: загружаем опросники...");
        bool formsLoaded = Forms.fetchFromServer();
        if (formsLoaded) {
            Logger.add("Фоновый поток: опросники загружены.");
        } else {
            Logger.add("Фоновый поток: опросники не загружены.", "error");
        }
    }

    unsigned long nextFormVersionCheckAt = millis() + FORM_VERSION_FIRST_CHECK_DELAY_MS;
    unsigned long nextAnswerQueueFlushAt = millis() + 5000UL;

    // Бесконечный цикл облачных операций
    while (true) {
        if (Forms.syncRequested && !Forms.syncInProgress) {
            Forms.syncRequested = false;
            Logger.add("Manual form sync requested.");
            bool manualFormsLoaded = Forms.fetchFromServer();
            Logger.add(manualFormsLoaded ? "Manual form sync completed." : "Manual form sync failed.", manualFormsLoaded ? "info" : "error");
        }

        if (!Forms.syncInProgress && (long)(millis() - nextFormVersionCheckAt) >= 0) {
            nextFormVersionCheckAt = millis() + FORM_VERSION_CHECK_INTERVAL_MS;
            Forms.checkForUpdates();
        }

        if ((long)(millis() - nextAnswerQueueFlushAt) >= 0) {
            nextAnswerQueueFlushAt = millis() + ANSWER_QUEUE_FLUSH_INTERVAL_MS;
            if (SubmissionQueue.count() > 0) {
                SubmissionQueue.flush();
            }
        }

        Cloud.loop();
        vTaskDelay(500 / portTICK_PERIOD_MS); // Пауза 0.5с между итерациями
    }
}

void setup() {
    Serial.begin(115200);
    
    pinMode(LED_PIN, OUTPUT);
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    digitalWrite(LED_PIN, LOW);

    esp_task_wdt_deinit();
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT * 1000,
        .idle_core_mask = 0,
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_config);
#else
    esp_task_wdt_init(WDT_TIMEOUT, true);
#endif
    esp_task_wdt_add(NULL); 

    Logger.begin();
    Logger.add("Запуск системы...");

    Config.begin();
    Forms.begin();
    SubmissionQueue.begin();
    ESPNetwork.begin();
    WebUI.begin();             // Веб-сервер стартует СРАЗУ после сети
    Cloud.begin("0.1.0");
    
    // Запускаем фоновый поток на ядре 1 (вместе с основным loop)
    // ВАЖНО: Тяжелые TLS-запросы и запись в память на ядре 0 приводят к сбоям Wi-Fi драйвера
    xTaskCreatePinnedToCore(
        backgroundNetworkTask,  // Функция задачи
        "net_bg",               // Имя (для отладки)
        16384,                  // Размер стека (байт) — TLS требует ~6-8 КБ!
        NULL,                   // Параметр
        1,                      // Приоритет (1 = нормальный)
        NULL,                   // Хэндл задачи (не нужен)
        1                       // Ядро 1 (прикладное)
    );
    
    Logger.add("Веб-сервер запущен. Сетевые операции вынесены в фоновый поток.");
}

void loop() {
    esp_task_wdt_reset(); 
    
    // Только быстрые неблокирующие операции в основном цикле!
    ESPNetwork.loop();   // DNS-сервер (микросекунды)
    WebUI.loop();        // Веб-сервер (микросекунды если нет запросов)
    
    // Cloud.loop() и Forms.fetchFromServer() УБРАНЫ из основного цикла!
    // Они работают в отдельном потоке backgroundNetworkTask на ядре 0.
    
    if (ledBlinkInterval > 0) {
        if (millis() - lastLedBlink >= (unsigned long)ledBlinkInterval) {
            lastLedBlink = millis();
            ledState = !ledState;
            digitalWrite(LED_PIN, ledState);
        }
    }

    if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
        if (!buttonPressed) {
            buttonPressed = true;
            buttonPressTime = millis();
        } else if (millis() - buttonPressTime > 5000) {
            digitalWrite(LED_PIN, LOW); 
            Serial.println("!!! ХАРД РЕСЕТ ЧЕРЕЗ КНОПКУ BOOT !!!");
            Preferences prefs;
            prefs.begin("config", false);
            prefs.clear();
            prefs.end();
            delay(1000);
            ESP.restart();
        }
    } else {
        buttonPressed = false;
    }
}
