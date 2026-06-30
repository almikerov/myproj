#include "Forms.h"
#include "Logger.h"
#include "ESPNetwork.h"
#include "Config.h"
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>

FormManager Forms;

// Безопасный Stream для записи в LittleFS с периодической отдачей процессорного времени (delay 1),
// чтобы не вызывать панику Watchdog Timer и не вешать систему на долгих файловых операциях.
class SafeFileStream : public Stream {
private:
    File* _file;
    size_t _bytesWritten = 0;
public:
    SafeFileStream(File* f) : _file(f) {}
    size_t write(uint8_t c) override {
        _bytesWritten++;
        if (_bytesWritten % 4096 == 0) delay(1); 
        return _file->write(c);
    }
    size_t write(const uint8_t *buffer, size_t size) override {
        _bytesWritten += size;
        if (_bytesWritten % 4096 < size) delay(1);
        return _file->write(buffer, size);
    }
    int available() override { return _file->available(); }
    int read() override { return _file->read(); }
    int peek() override { return _file->peek(); }
    void flush() override { _file->flush(); }
};

static String readStringUntilToken(File& file, const char* token) {
    String result = "";
    result.reserve(128); // Prevent heap fragmentation
    int tokenLen = strlen(token);
    int matchIdx = 0;
    while(file.available()) {
        char c = file.read();
        if (c == token[matchIdx]) {
            matchIdx++;
            if (matchIdx == tokenLen) {
                return result;
            }
        } else {
            if (matchIdx > 0) {
                for (int i=0; i<matchIdx; i++) result += token[i];
                matchIdx = 0;
            }
            if (c == token[0]) matchIdx = 1;
            else result += c;
        }
        if (result.length() > 512) return result; 
    }
    return result;
}

void FormManager::begin() {
    if(!LittleFS.begin(true)){
        Logger.add("❌ Ошибка монтирования LittleFS!", "error");
    }
}

String FormManager::getLocalToken(const String& formNumber, IPAddress ip) {
    int hash = (ip[3] * 137) ^ (formNumber.toInt() * 89);
    hash = hash + 4096; 
    char hexBuf[10]; 
    sprintf(hexBuf, "%X", hash);
    return "TK-" + String(hexBuf); 
}

bool FormManager::fetchFromServer() {
    if (!ESPNetwork.isConnected()) {
        Logger.add("Нет сети, пропускаем скачивание опросников.");
        return false;
    }
    Logger.add("--- НАЧАЛО: Запрос списка опросников ---");

    // === Запрос количества форм (отдельный клиент) ===
    {
        WiFiClientSecure countClient;
        countClient.setInsecure();
        HTTPClient http;
        
        http.begin(countClient, Config.cloud_url + "?action=getFormCount");
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.setTimeout(45000);
        http.setReuse(false);
        http.useHTTP10(true);
        http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
        
        int countCode = http.GET();
        Logger.add("HTTP Код (Список): " + String(countCode));

        if (countCode == 200) {
            formCount = http.getString().toInt();
            Logger.add("✅ Найдено опросов: " + String(formCount));
        } else {
            Logger.add("❌ Ошибка получения списка: " + String(countCode), "error");
        }
        http.end();
    } // countClient уничтожается здесь, освобождая память
    
    if (formCount <= 0) return false;

    formTitles.assign(formCount, "Загрузка опроса...");
    formMaxDaily.assign(formCount, 1);
    formHtmlOffsets.assign(formCount, 0);

    Logger.add("Очистка памяти перед загрузкой...");
    File root = LittleFS.open("/");
    if (root) {
        std::vector<String> filesToDelete;
        File file = root.openNextFile();
        while (file) {
            String fName = file.name();
            if (!fName.startsWith("/")) fName = "/" + fName;
            if (fName.startsWith("/raw_")) {
                filesToDelete.push_back(fName);
            }
            file.close();
            file = root.openNextFile();
        }
        root.close();
        
        for (const String& fName : filesToDelete) {
            LittleFS.remove(fName);
            delay(1); // небольшая пауза, чтобы не вешать систему
        }
    }

    int loadedCount = 0;
    for (int i = 0; i < formCount; i++) {
        esp_task_wdt_reset();
        Logger.add("Скачивание опроса #" + String(i + 1) + "...");
        
        // КРИТИЧЕСКИ ВАЖНО: Создаем СВЕЖИЙ WiFiClientSecure для КАЖДОЙ формы!
        // Повторное использование одного клиента приводит к тому, что TLS-сессия
        // остается в грязном состоянии после http.end(), и следующий запрос
        // либо не устанавливается, либо получает мусор.
        WiFiClientSecure formClient;
        formClient.setInsecure();
        HTTPClient formHttp;

        formHttp.begin(formClient, Config.cloud_url + "?action=getForm&index=" + String(i));
        formHttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); 
        formHttp.setTimeout(8000); // 8 секунд вместо 45, чтобы не висело вечно, если Cloudflare не закроет сокет
        formHttp.setReuse(false);
        formHttp.useHTTP10(false); // ВАЖНО: HTTP/1.1 необходим для правильного парсинга Chunked Transfer Encoding
        formHttp.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
        formHttp.addHeader("Connection", "close");

        int formCode = formHttp.GET();
        if (formCode == 200) {
            String fileName = "/raw_" + String(i) + ".txt";
            File file = LittleFS.open(fileName, FILE_WRITE);
            if (file) {
                // writeToStream нативно парсит chunked-ответы, а SafeFileStream не дает зависнуть Watchdog'у
                SafeFileStream safeStream(&file);
                formHttp.writeToStream(&safeStream);
                file.close();
                
                Logger.add("Парсинг...");
                file = LittleFS.open(fileName, FILE_READ);
                if (file) {
                    String title = readStringUntilToken(file, "|||");
                    String maxDailyStr = readStringUntilToken(file, "|||");
                    
                    title.trim(); maxDailyStr.trim();
                    if (title.length() == 0) title = "Опросник " + String(i + 1);
                    title.replace("<", "&lt;"); title.replace(">", "&gt;");
                    
                    formTitles[i] = title;
                    formMaxDaily[i] = maxDailyStr.toInt();
                    formHtmlOffsets[i] = file.position(); 
                    file.close();
                    
                    loadedCount++;
                    Logger.add("✅ Опрос [" + title + "] загружен!");
                } else {
                    Logger.add("❌ Ошибка чтения файла формы.", "error");
                }
            } else {
                Logger.add("❌ Ошибка открытия файла на запись.", "error");
            }
        } else {
            Logger.add("❌ Провал скачивания #" + String(i+1) + ": код " + String(formCode), "error");
        }
        formHttp.end();
    }
    Logger.add("Загружено опросников: " + String(loadedCount) + " из " + String(formCount));
    Logger.add("--- КОНЕЦ: Обновление опросников ---");
    return loadedCount > 0;
}

String FormManager::urlEncode(const String& str) {
    String encodedString = ""; char c, code0, code1;
    for (unsigned int i = 0; i < str.length(); i++) {
        c = str.charAt(i);
        if (c == ' ') encodedString += '+'; else if (isalnum(c)) encodedString += c;
        else {
            code1 = (c & 0xf) + '0'; if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
            c = (c >> 4) & 0xf; code0 = c + '0'; if (c > 9) code0 = c - 10 + 'A';
            encodedString += '%'; encodedString += code0; encodedString += code1;
        }
    }
    return encodedString;
}
