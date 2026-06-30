#include "Forms.h"
#include "Logger.h"
#include "ESPNetwork.h"
#include "Config.h"
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_task_wdt.h>
#include <ctype.h>

FormManager Forms;

static int clampProgressPercent(int value) {
    if (value < 0) return 0;
    if (value > 100) return 100;
    return value;
}

static int overallProgressPercent(int ready, int total, int currentPercent) {
    if (total <= 0) return 0;
    long value = ((long)ready * 100L + clampProgressPercent(currentPercent)) / total;
    if (value > 99 && ready < total) value = 99;
    return clampProgressPercent((int)value);
}

int FormManager::readyCount() const {
    int count = 0;
    for (bool ready : formReady) {
        if (ready) count++;
    }
    return count;
}

void FormManager::setProgress(const String& phase, const String& message, int form, int forms, int ready, int percent, size_t bytes, size_t totalBytes, int image, int images) {
    progressActive = true;
    progressPhase = phase;
    progressMessage = message;
    progressForm = form;
    progressForms = forms;
    progressReady = ready;
    progressPercent = clampProgressPercent(percent);
    progressBytes = bytes;
    progressTotalBytes = totalBytes;
    progressImage = image;
    progressImages = images;
    progressUpdatedAt = millis();
}

void FormManager::finishProgress(const String& message) {
    progressActive = false;
    progressPhase = "idle";
    progressMessage = message;
    progressReady = readyCount();
    progressForms = formCount;
    progressPercent = formCount > 0 ? clampProgressPercent((progressReady * 100) / formCount) : 0;
    progressBytes = 0;
    progressTotalBytes = 0;
    progressImage = 0;
    progressImages = 0;
    progressUpdatedAt = millis();
}

bool FormManager::cacheFormHtml(int index) {
    if (index < 0 || index >= formCount || index >= (int)formHtmlOffsets.size()) return false;
    if ((int)formHtmlCache.size() < formCount) formHtmlCache.resize(formCount);

    formHtmlCache[index] = "";
    String fileName = "/raw_" + String(index) + ".txt";
    File file = LittleFS.open(fileName, FILE_READ);
    if (!file) {
        Logger.add("Form cache failed: file not found for #" + String(index + 1), "error");
        return false;
    }

    uint32_t offset = formHtmlOffsets[index];
    if (offset >= file.size()) {
        file.close();
        Logger.add("Form cache failed: empty HTML for #" + String(index + 1), "error");
        return false;
    }

    size_t expectedSize = file.size() - offset;
    String html = "";
    html.reserve(expectedSize + 1);
    file.seek(offset);

    char buf[1025];
    while (file.available()) {
        size_t len = file.read((uint8_t*)buf, 1024);
        buf[len] = '\0';
        html += buf;
        if ((html.length() & 0x0FFF) == 0) {
            delay(1);
            esp_task_wdt_reset();
        }
    }
    file.close();

    if (html.length() != expectedSize) {
        Logger.add("Form cache failed: size mismatch for #" + String(index + 1) + " (" + String(html.length()) + "/" + String(expectedSize) + " bytes)", "error");
        return false;
    }

    formHtmlCache[index] = html;
    Logger.add("Form #" + String(index + 1) + " cached in RAM: " + String(formHtmlCache[index].length()) + " bytes");
    return true;
}

void FormManager::cacheAllReadyForms() {
    if (formCount <= 0) return;
    formHtmlCache.resize(formCount);
    size_t totalBytes = 0;
    int cached = 0;

    for (int i = 0; i < formCount; i++) {
        bool ready = i < (int)formReady.size() && formReady[i];
        if (!ready) {
            formHtmlCache[i] = "";
            continue;
        }

        if (cacheFormHtml(i)) {
            cached++;
            totalBytes += formHtmlCache[i].length();
        } else {
            formReady[i] = false;
        }
    }

    Logger.add("RAM form cache ready: " + String(cached) + " of " + String(formCount) + ", total " + String(totalBytes) + " bytes");
}

// Безопасный Stream для записи в LittleFS с периодической отдачей процессорного времени (delay 1),
// чтобы не вызывать панику Watchdog Timer и не вешать систему на долгих файловых операциях.
class SafeFileStream : public Stream {
private:
    File* _file;
    size_t _bytesWritten = 0;
    size_t _totalBytes = 0;
    size_t _nextProgressBytes = 65536;
    unsigned long _startedAt = 0;
    String _label = "";
    int _form = 0;
    int _forms = 0;
    int _ready = 0;
public:
    SafeFileStream(File* f, const String& label = "", int form = 0, int forms = 0, int ready = 0, size_t totalBytes = 0) : _file(f), _totalBytes(totalBytes), _label(label), _form(form), _forms(forms), _ready(ready) {
        _startedAt = millis();
    }
    size_t write(uint8_t c) override {
        _bytesWritten++;
        if (_bytesWritten % 4096 == 0) {
            delay(1);
            reportProgress();
        }
        return _file->write(c);
    }
    size_t write(const uint8_t *buffer, size_t size) override {
        _bytesWritten += size;
        if (_bytesWritten % 4096 < size) delay(1);
        esp_task_wdt_reset();
        reportProgress();
        return _file->write(buffer, size);
    }
    int available() override { return _file->available(); }
    int read() override { return _file->read(); }
    int peek() override { return _file->peek(); }
    void flush() override { _file->flush(); }
    size_t bytesWritten() const { return _bytesWritten; }
private:
    void reportProgress() {
        if (_label.length() == 0 || _bytesWritten < _nextProgressBytes) return;
        String sizePart = String(_bytesWritten / 1024) + " KB";
        int formPercent = 10;
        if (_totalBytes > 0) {
            formPercent = 10 + (int)((_bytesWritten * 70ULL) / _totalBytes);
            if (formPercent > 80) formPercent = 80;
            sizePart += " / " + String(_totalBytes / 1024) + " KB";
        }
        Forms.setProgress("download", "Скачивание опроса " + String(_form) + " из " + String(_forms), _form, _forms, _ready, overallProgressPercent(_ready, _forms, formPercent), _bytesWritten, _totalBytes, 0, 0);
        Logger.add(_label + ": " + sizePart + " in " + String((millis() - _startedAt) / 1000) + "s");
        _nextProgressBytes += 65536;
    }
};

static const int FORM_DOWNLOAD_TIMEOUT_MS = 60000;

static String metaPathForForm(int formIndex) {
    return "/meta_" + String(formIndex) + ".txt";
}

static bool readSmallFile(const String& path, String& out) {
    File file = LittleFS.open(path, FILE_READ);
    if (!file) return false;
    out = file.readString();
    file.close();
    out.trim();
    return out.length() > 0;
}

static bool writeSmallFile(const String& path, const String& value) {
    LittleFS.remove(path);
    File file = LittleFS.open(path, FILE_WRITE);
    if (!file) return false;
    file.print(value);
    file.close();
    return true;
}

static bool parseMetaPayload(const String& payload, String& title, int& maxDaily, String& revision) {
    int sep1 = payload.indexOf("|||");
    if (sep1 < 0) return false;
    int sep2 = payload.indexOf("|||", sep1 + 3);
    if (sep2 < 0) return false;

    title = payload.substring(0, sep1);
    String maxDailyStr = payload.substring(sep1 + 3, sep2);
    revision = payload.substring(sep2 + 3);

    title.trim();
    maxDailyStr.trim();
    revision.trim();
    maxDaily = maxDailyStr.toInt();
    if (maxDaily <= 0) maxDaily = 1;
    return true;
}

static int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;
    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') return -3;
    return -1;
}

static bool writeBase64Decoded(File& out, char c, int quartet[4], int& quartetLen) {
    int v = base64Value(c);
    if (v == -3) return true;
    if (v == -1) return false;

    quartet[quartetLen++] = v;
    if (quartetLen < 4) return true;

    if (quartet[0] < 0 || quartet[1] < 0) return false;

    uint8_t b0 = (quartet[0] << 2) | (quartet[1] >> 4);
    out.write(b0);

    if (quartet[2] != -2) {
        if (quartet[2] < 0) return false;
        uint8_t b1 = ((quartet[1] & 0x0F) << 4) | (quartet[2] >> 2);
        out.write(b1);
    }

    if (quartet[3] != -2) {
        if (quartet[2] < 0 || quartet[3] < 0) return false;
        uint8_t b2 = ((quartet[2] & 0x03) << 6) | quartet[3];
        out.write(b2);
    }

    quartetLen = 0;
    return true;
}

static String imageExtensionForMime(const String& mime) {
    if (mime.indexOf("jpeg") != -1 || mime.indexOf("jpg") != -1) return ".jpg";
    if (mime.indexOf("gif") != -1) return ".gif";
    if (mime.indexOf("webp") != -1) return ".webp";
    return ".png";
}

static bool flushCandidateOrRestart(File& out, String& candidate, char c, int& state) {
    out.print(candidate);
    candidate = "";
    state = 0;
    if (c == 's') {
        candidate = "s";
        state = 1;
    } else {
        out.write((uint8_t)c);
    }
    return true;
}

static bool extractDataImage(File& in, File& out, int formIndex, int& imageIndex, char quote, const String& candidate) {
    String meta = "";
    meta.reserve(48);

    while (in.available()) {
        char c = in.read();
        if (c == quote) {
            out.print(candidate);
            out.print(meta);
            out.write((uint8_t)c);
            return false;
        }
        if (c == ',') break;
        if (meta.length() > 80) {
            out.print(candidate);
            out.print(meta);
            out.write((uint8_t)c);
            return false;
        }
        meta += c;
    }

    String metaLower = meta;
    metaLower.toLowerCase();
    int b64Idx = metaLower.indexOf(";base64");
    if (b64Idx == -1) {
        out.print(candidate);
        out.print(meta);
        out.write((uint8_t)',');
        return false;
    }

    String mime = metaLower.substring(0, b64Idx);
    String imgPath = "/img_" + String(formIndex) + "_" + String(imageIndex) + imageExtensionForMime(mime);
    LittleFS.remove(imgPath);

    File img = LittleFS.open(imgPath, FILE_WRITE);
    if (!img) {
        out.print(candidate);
        out.print(meta);
        out.write((uint8_t)',');
        return false;
    }

    int quartet[4] = {0, 0, 0, 0};
    int quartetLen = 0;
    uint32_t b64Chars = 0;
    bool ok = true;

    while (in.available()) {
        char c = in.read();
        if (c == quote) {
            break;
        }
        if (!writeBase64Decoded(img, c, quartet, quartetLen)) {
            ok = false;
        }
        b64Chars++;
        if ((b64Chars & 0x0FFF) == 0) {
            delay(1);
            esp_task_wdt_reset();
        }
    }

    if (ok && quartetLen == 2) {
        if (quartet[0] >= 0 && quartet[1] >= 0) img.write((uint8_t)((quartet[0] << 2) | (quartet[1] >> 4)));
    } else if (ok && quartetLen == 3) {
        if (quartet[0] >= 0 && quartet[1] >= 0 && quartet[2] >= 0) {
            img.write((uint8_t)((quartet[0] << 2) | (quartet[1] >> 4)));
            img.write((uint8_t)(((quartet[1] & 0x0F) << 4) | (quartet[2] >> 2)));
        }
    } else if (quartetLen != 0) {
        ok = false;
    }

    size_t imageSize = img.size();
    img.close();

    if (!ok || imageSize == 0) {
        LittleFS.remove(imgPath);
        out.print("src=");
        out.write((uint8_t)quote);
        out.write((uint8_t)quote);
        return false;
    }

    out.print("src=");
    out.write((uint8_t)quote);
    out.print("data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==");
    out.write((uint8_t)quote);
    out.print(" data-src=");
    out.write((uint8_t)quote);
    out.print(imgPath);
    out.write((uint8_t)quote);
    out.print(" class=");
    out.write((uint8_t)quote);
    out.print("lazy-img");
    out.write((uint8_t)quote);
    imageIndex++;
    return true;
}

static bool splitEmbeddedImages(const String& fileName, int formIndex, uint32_t htmlOffset, int expectedImages, int readyCount, int totalForms) {
    File in = LittleFS.open(fileName, FILE_READ);
    if (!in) return false;

    String tmpName = "/raw_tmp_" + String(formIndex) + ".txt";
    LittleFS.remove(tmpName);
    File out = LittleFS.open(tmpName, FILE_WRITE);
    if (!out) {
        in.close();
        return false;
    }

    uint8_t copyBuf[256];
    uint32_t left = htmlOffset;
    while (left > 0 && in.available()) {
        size_t n = left > sizeof(copyBuf) ? sizeof(copyBuf) : left;
        n = in.read(copyBuf, n);
        out.write(copyBuf, n);
        left -= n;
    }

    int state = 0;
    int dataIdx = 0;
    char quote = 0;
    int imageIndex = 0;
    int extracted = 0;
    String candidate = "";
    candidate.reserve(16);
    const char* dataWord = "data:";
    size_t totalSize = in.size();
    uint32_t nextProgressPos = htmlOffset + 65536;

    while (in.available()) {
        char c = in.read();

        if (state == 0) {
            if (c == 's') {
                candidate = "s";
                state = 1;
            } else {
                out.write((uint8_t)c);
            }
        } else if (state == 1) {
            if (c == 'r') {
                candidate += c;
                state = 2;
            } else {
                flushCandidateOrRestart(out, candidate, c, state);
            }
        } else if (state == 2) {
            if (c == 'c') {
                candidate += c;
                state = 3;
            } else {
                flushCandidateOrRestart(out, candidate, c, state);
            }
        } else if (state == 3) {
            if (c == '=') {
                candidate += c;
                state = 4;
            } else {
                flushCandidateOrRestart(out, candidate, c, state);
            }
        } else if (state == 4) {
            if (c == '\'' || c == '"') {
                quote = c;
                candidate += c;
                dataIdx = 0;
                state = 5;
            } else {
                flushCandidateOrRestart(out, candidate, c, state);
            }
        } else if (state == 5) {
            if (c == dataWord[dataIdx]) {
                candidate += c;
                dataIdx++;
                if (dataIdx == 5) {
                    Forms.setProgress("images", "Обработка картинок " + String(imageIndex + 1) + " из " + String(expectedImages), formIndex + 1, totalForms, readyCount, overallProgressPercent(readyCount, totalForms, 85), in.position(), totalSize, imageIndex + 1, expectedImages);
                    if (extractDataImage(in, out, formIndex, imageIndex, quote, candidate)) {
                        extracted++;
                    }
                    candidate = "";
                    state = 0;
                }
            } else {
                flushCandidateOrRestart(out, candidate, c, state);
            }
        }

        if ((out.size() & 0x0FFF) == 0) {
            delay(1);
            esp_task_wdt_reset();
        }
        if (in.position() >= nextProgressPos) {
            int scanPercent = totalSize > htmlOffset ? (int)(((in.position() - htmlOffset) * 100ULL) / (totalSize - htmlOffset)) : 100;
            int formPercent = 80 + (scanPercent / 5);
            Forms.setProgress("images", "Обработка картинок " + String(extracted) + " из " + String(expectedImages), formIndex + 1, totalForms, readyCount, overallProgressPercent(readyCount, totalForms, formPercent), in.position(), totalSize, extracted, expectedImages);
            nextProgressPos += 65536;
        }
    }

    if (candidate.length() > 0) out.print(candidate);

    in.close();
    out.close();

    if (extracted == 0) {
        LittleFS.remove(tmpName);
        return false;
    }

    LittleFS.remove(fileName);
    bool renamed = LittleFS.rename(tmpName, fileName);
    if (!renamed) {
        LittleFS.remove(tmpName);
        return false;
    }

    Logger.add("Images extracted from form #" + String(formIndex + 1) + ": " + String(extracted));
    return true;
}

static int countDataImages(const String& fileName, uint32_t htmlOffset) {
    File file = LittleFS.open(fileName, FILE_READ);
    if (!file) return 0;
    file.seek(htmlOffset);

    const char* needle = "data:image/";
    int match = 0;
    int count = 0;
    while (file.available()) {
        char c = file.read();
        if (c == needle[match]) {
            match++;
            if (needle[match] == '\0') {
                count++;
                match = 0;
            }
        } else {
            match = (c == needle[0]) ? 1 : 0;
        }
        if ((file.position() & 0x0FFF) == 0) {
            delay(1);
            esp_task_wdt_reset();
        }
    }

    file.close();
    return count;
}

static void removeImagesForForm(int formIndex) {
    File root = LittleFS.open("/");
    if (!root) return;

    String prefix = "/img_" + String(formIndex) + "_";
    std::vector<String> filesToDelete;
    File file = root.openNextFile();
    while (file) {
        String fName = file.name();
        if (!fName.startsWith("/")) fName = "/" + fName;
        if (fName.startsWith(prefix)) filesToDelete.push_back(fName);
        file.close();
        file = root.openNextFile();
    }
    root.close();

    for (const String& fName : filesToDelete) {
        LittleFS.remove(fName);
        delay(1);
    }
}

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
    } else {
        loadCachedForms();
    }
}

bool FormManager::loadCachedForms() {
    int maxIndex = -1;
    File root = LittleFS.open("/");
    if (!root) return false;

    File file = root.openNextFile();
    while (file) {
        String fName = file.name();
        if (!fName.startsWith("/")) fName = "/" + fName;
        if (fName.startsWith("/raw_") && fName.endsWith(".txt") && !fName.startsWith("/raw_tmp_") && !fName.startsWith("/raw_dl_")) {
            String indexPart = fName.substring(5, fName.length() - 4);
            bool numeric = indexPart.length() > 0;
            for (unsigned int j = 0; j < indexPart.length(); j++) {
                if (!isDigit(indexPart[j])) {
                    numeric = false;
                    break;
                }
            }
            if (numeric) {
                int idx = indexPart.toInt();
                if (idx > maxIndex) maxIndex = idx;
            }
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();

    if (maxIndex < 0) return false;

    formCount = maxIndex + 1;
    formTitles.resize(formCount, "Загрузка опроса...");
    formMaxDaily.resize(formCount, 1);
    formHtmlOffsets.resize(formCount, 0);
    formReady.resize(formCount, false);
    formHtmlCache.resize(formCount, "");
    formHtmlCache.resize(formCount, "");
    formRevisions.resize(formCount, "");
    for (int i = 0; i < formCount; i++) {
        if (formTitles[i].length() == 0) formTitles[i] = "Загрузка опроса...";
        if (formMaxDaily[i] <= 0) formMaxDaily[i] = 1;
    }

    int loaded = 0;
    for (int i = 0; i < formCount; i++) {
        String fileName = "/raw_" + String(i) + ".txt";
        File cached = LittleFS.open(fileName, FILE_READ);
        if (!cached) continue;

        String title = readStringUntilToken(cached, "|||");
        String maxDailyStr = readStringUntilToken(cached, "|||");
        title.trim();
        maxDailyStr.trim();
        if (title.length() == 0) title = "Опросник " + String(i + 1);
        title.replace("<", "&lt;");
        title.replace(">", "&gt;");

        bool hasHtml = cached.available() > 0;
        if (hasHtml) {
            formTitles[i] = title;
            formMaxDaily[i] = maxDailyStr.toInt();
            if (formMaxDaily[i] <= 0) formMaxDaily[i] = 1;
            formHtmlOffsets[i] = cached.position();
            formReady[i] = true;
            readSmallFile(metaPathForForm(i), formRevisions[i]);
            loaded++;
        }
        cached.close();
    }

    if (loaded > 0) {
        Logger.add("Cached forms ready: " + String(loaded) + " of " + String(formCount));
        cacheAllReadyForms();
    }
    return loaded > 0;
}

String FormManager::getLocalToken(const String& formNumber, IPAddress ip) {
    int hash = (ip[3] * 137) ^ (formNumber.toInt() * 89);
    hash = hash + 4096; 
    char hexBuf[10]; 
    sprintf(hexBuf, "%X", hash);
    return "TK-" + String(hexBuf); 
}

bool FormManager::fetchFromServer() {
    finishProgress("Проверка сети");
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
    
    if (formCount <= 0) {
        finishProgress("Список опросов не получен");
        return false;
    }

    int oldCount = formTitles.size();
    formTitles.resize(formCount, "Загрузка опроса...");
    formMaxDaily.resize(formCount, 1);
    formHtmlOffsets.resize(formCount, 0);
    formReady.resize(formCount, false);
    for (int i = oldCount; i < formCount; i++) {
        if (formTitles[i].length() == 0) formTitles[i] = "Загрузка опроса...";
        if (formMaxDaily[i] <= 0) formMaxDaily[i] = 1;
    }

    Logger.add("Очистка памяти перед загрузкой...");
    File root = LittleFS.open("/");
    if (root) {
        std::vector<String> filesToDelete;
        File file = root.openNextFile();
        while (file) {
            String fName = file.name();
            if (!fName.startsWith("/")) fName = "/" + fName;
            if (fName.startsWith("/raw_tmp_") || fName.startsWith("/raw_dl_")) {
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

    int loadedCount = readyCount();
    std::vector<int> imageForms;
    std::vector<int> imageCounts(formCount, 0);
    setProgress("start", "Проверка обновлений", 0, formCount, loadedCount, overallProgressPercent(loadedCount, formCount, 0), 0, 0, 0, 0);
    for (int i = 0; i < formCount; i++) {
        esp_task_wdt_reset();
        Logger.add("Скачивание опроса #" + String(i + 1) + "...");
        setProgress("meta", "Проверка опроса " + String(i + 1) + " из " + String(formCount), i + 1, formCount, loadedCount, overallProgressPercent(loadedCount, formCount, 5), 0, 0, 0, 0);
        unsigned long formStartMs = millis();
        String finalName = "/raw_" + String(i) + ".txt";
        bool hasPublishedCache = false;
        File published = LittleFS.open(finalName, FILE_READ);
        if (published) {
            hasPublishedCache = true;
            published.close();
        }

        bool shouldDownload = true;
        WiFiClientSecure metaClient;
        metaClient.setInsecure();
        HTTPClient metaHttp;
        metaHttp.begin(metaClient, Config.cloud_url + "?action=getFormMeta&index=" + String(i));
        metaHttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        metaHttp.setTimeout(15000);
        metaHttp.setReuse(false);
        metaHttp.useHTTP10(true);
        metaHttp.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
        int metaCode = metaHttp.GET();
        if (metaCode == 200) {
            String metaPayload = metaHttp.getString();
            String remoteTitle = "";
            String remoteRevision = "";
            int remoteMaxDaily = 1;
            if (parseMetaPayload(metaPayload, remoteTitle, remoteMaxDaily, remoteRevision)) {
                remoteTitle.replace("<", "&lt;");
                remoteTitle.replace(">", "&gt;");
                if (remoteTitle.length() > 0) formTitles[i] = remoteTitle;
                formMaxDaily[i] = remoteMaxDaily;

                if (hasPublishedCache && formRevisions[i].length() > 0 && remoteRevision.length() > 0 && formRevisions[i] == remoteRevision) {
                    shouldDownload = false;
                    if (!formReady[i]) loadedCount++;
                    formReady[i] = true;
                    setProgress("cache", "Опрос " + String(i + 1) + " из " + String(formCount) + ": без изменений", i + 1, formCount, loadedCount, overallProgressPercent(loadedCount, formCount, 100), 0, 0, 0, 0);
                    Logger.add("Form #" + String(i + 1) + " unchanged, using cache.");
                } else if (remoteRevision.length() > 0) {
                    if (formReady[i] && loadedCount > 0) loadedCount--;
                    formReady[i] = false;
                    formRevisions[i] = remoteRevision;
                }
            }
        }
        metaHttp.end();

        if (!shouldDownload) {
            delay(50);
            continue;
        }
        
        // КРИТИЧЕСКИ ВАЖНО: Создаем СВЕЖИЙ WiFiClientSecure для КАЖДОЙ формы!
        // Повторное использование одного клиента приводит к тому, что TLS-сессия
        // остается в грязном состоянии после http.end(), и следующий запрос
        // либо не устанавливается, либо получает мусор.
        WiFiClientSecure formClient;
        formClient.setInsecure();
        HTTPClient formHttp;

        formHttp.begin(formClient, Config.cloud_url + "?action=getForm&index=" + String(i));
        formHttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); 
        formHttp.setTimeout(FORM_DOWNLOAD_TIMEOUT_MS);
        formHttp.setReuse(false);
        formHttp.useHTTP10(false); // ВАЖНО: HTTP/1.1 необходим для правильного парсинга Chunked Transfer Encoding
        formHttp.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
        formHttp.addHeader("Accept-Encoding", "identity");
        formHttp.addHeader("Connection", "close");

        int formCode = formHttp.GET();
        if (formCode == 200) {
            int declaredSize = formHttp.getSize();
            size_t totalBytes = declaredSize > 0 ? (size_t)declaredSize : 0;
            setProgress("download", "Скачивание опроса " + String(i + 1) + " из " + String(formCount), i + 1, formCount, loadedCount, overallProgressPercent(loadedCount, formCount, 10), 0, totalBytes, 0, 0);
            String fileName = "/raw_dl_" + String(i) + ".txt";
            LittleFS.remove(fileName);
            File file = LittleFS.open(fileName, FILE_WRITE);
            if (file) {
                // writeToStream нативно парсит chunked-ответы, а SafeFileStream не дает зависнуть Watchdog'у
                SafeFileStream safeStream(&file, "Downloading form #" + String(i + 1), i + 1, formCount, loadedCount, totalBytes);
                int writtenResult = formHttp.writeToStream(&safeStream);
                size_t bytesWritten = safeStream.bytesWritten();
                file.close();

                if (writtenResult < 0 || bytesWritten == 0) {
                    Logger.add("Empty or interrupted form file #" + String(i + 1), "error");
                    LittleFS.remove(fileName);
                    formHttp.end();
                    delay(250);
                    continue;
                }
                
                Logger.add("Парсинг...");
                setProgress("parse", "Разбор опроса " + String(i + 1) + " из " + String(formCount), i + 1, formCount, loadedCount, overallProgressPercent(loadedCount, formCount, 82), bytesWritten, bytesWritten, 0, 0);
                file = LittleFS.open(fileName, FILE_READ);
                if (file) {
                    String title = readStringUntilToken(file, "|||");
                    String maxDailyStr = readStringUntilToken(file, "|||");
                    
                    title.trim(); maxDailyStr.trim();
                    if (title.length() == 0) title = "Опросник " + String(i + 1);
                    title.replace("<", "&lt;"); title.replace(">", "&gt;");
                    
                    formTitles[i] = title;
                    formMaxDaily[i] = maxDailyStr.toInt();
                    if (formMaxDaily[i] <= 0) formMaxDaily[i] = 1;
                    formHtmlOffsets[i] = file.position(); 
                    bool hasHtml = file.available() > 0;
                    file.close();

                    if (!hasHtml) {
                        Logger.add("Form arrived without HTML content.", "error");
                        LittleFS.remove(fileName);
                        formHttp.end();
                        delay(250);
                        continue;
                    }

                    int dataImageCount = countDataImages(fileName, formHtmlOffsets[i]);
                    if (dataImageCount > 0) {
                        removeImagesForForm(i);
                        imageForms.push_back(i);
                        imageCounts[i] = dataImageCount;
                        setProgress("images", "Опрос " + String(i + 1) + ": найдено картинок " + String(dataImageCount), i + 1, formCount, loadedCount, overallProgressPercent(loadedCount, formCount, 84), bytesWritten, bytesWritten, 0, dataImageCount);
                        Logger.add("Form #" + String(i + 1) + " downloaded in " + String(millis() - formStartMs) + " ms and queued for images.");
                    } else {
                        LittleFS.remove(finalName);
                        if (LittleFS.rename(fileName, finalName)) {
                            if (!formReady[i]) loadedCount++;
                            formReady[i] = true;
                            writeSmallFile(metaPathForForm(i), formRevisions[i]);
                            setProgress("ready", "Опрос " + String(i + 1) + " из " + String(formCount) + " готов", i + 1, formCount, loadedCount, overallProgressPercent(loadedCount, formCount, 100), bytesWritten, bytesWritten, 0, 0);
                            Logger.add("✅ Опрос [" + title + "] загружен! Bytes: " + String(bytesWritten) + ", ms: " + String(millis() - formStartMs));
                        } else {
                            Logger.add("Failed to publish form #" + String(i + 1), "error");
                            LittleFS.remove(fileName);
                        }
                    }
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

    for (int idx : imageForms) {
        esp_task_wdt_reset();
        String tmpName = "/raw_dl_" + String(idx) + ".txt";
        String finalName = "/raw_" + String(idx) + ".txt";
        Logger.add("Processing images for form #" + String(idx + 1) + "...");
        unsigned long processStartMs = millis();

        if (splitEmbeddedImages(tmpName, idx, formHtmlOffsets[idx], imageCounts[idx], loadedCount, formCount)) {
            LittleFS.remove(finalName);
            if (LittleFS.rename(tmpName, finalName)) {
                if (!formReady[idx]) loadedCount++;
                formReady[idx] = true;
                writeSmallFile(metaPathForForm(idx), formRevisions[idx]);
                setProgress("ready", "Опрос " + String(idx + 1) + " из " + String(formCount) + " готов", idx + 1, formCount, loadedCount, overallProgressPercent(loadedCount, formCount, 100), 0, 0, imageCounts[idx], imageCounts[idx]);
                Logger.add("Image form #" + String(idx + 1) + " is ready in " + String(millis() - processStartMs) + " ms.");
            } else {
                Logger.add("Failed to publish image form #" + String(idx + 1), "error");
                LittleFS.remove(tmpName);
            }
        } else {
            Logger.add("Failed to process images for form #" + String(idx + 1), "error");
            LittleFS.remove(tmpName);
        }
        delay(250);
    }
    Logger.add("Загружено опросников: " + String(loadedCount) + " из " + String(formCount));
    Logger.add("--- КОНЕЦ: Обновление опросников ---");
    finishProgress("Готово: " + String(loadedCount) + " из " + String(formCount));
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
