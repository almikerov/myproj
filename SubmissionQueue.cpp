#include "SubmissionQueue.h"
#include "Config.h"
#include "ESPNetwork.h"
#include "Logger.h"
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <algorithm>
#include <vector>

SubmissionQueueManager SubmissionQueue;

static const char* QUEUE_PREFIX = "/ansq_";
static const char* QUEUE_TMP_PREFIX = "/ansq_tmp_";
static const char* QUEUE_SUFFIX = ".txt";
static const char* PAYLOAD_SEPARATOR = "\n---PAYLOAD---\n";

void SubmissionQueueManager::begin() {
    removeTempFiles();
    int queued = count();
    if (queued > 0) {
        Logger.add("Очередь ответов восстановлена: " + String(queued) + ", " + String(totalBytes() / 1024) + " KB");
    }
}

String SubmissionQueueManager::makeQueuePath() {
    static uint32_t sequence = 0;
    sequence++;
    return String(QUEUE_PREFIX) + String(millis()) + "_" + String(sequence) + QUEUE_SUFFIX;
}

void SubmissionQueueManager::removeTempFiles() {
    File root = LittleFS.open("/");
    if (!root) return;

    std::vector<String> filesToDelete;
    File file = root.openNextFile();
    while (file) {
        String name = file.name();
        if (!name.startsWith("/")) name = "/" + name;
        if (name.startsWith(QUEUE_TMP_PREFIX)) filesToDelete.push_back(name);
        file.close();
        file = root.openNextFile();
    }
    root.close();

    for (const String& path : filesToDelete) {
        LittleFS.remove(path);
        delay(1);
    }
}

int SubmissionQueueManager::count() {
    File root = LittleFS.open("/");
    if (!root) return 0;

    int result = 0;
    File file = root.openNextFile();
    while (file) {
        String name = file.name();
        if (!name.startsWith("/")) name = "/" + name;
        if (name.startsWith(QUEUE_PREFIX) && name.endsWith(QUEUE_SUFFIX) && !name.startsWith(QUEUE_TMP_PREFIX)) result++;
        file.close();
        file = root.openNextFile();
    }
    root.close();
    return result;
}

size_t SubmissionQueueManager::totalBytes() {
    File root = LittleFS.open("/");
    if (!root) return 0;

    size_t result = 0;
    File file = root.openNextFile();
    while (file) {
        String name = file.name();
        if (!name.startsWith("/")) name = "/" + name;
        if (name.startsWith(QUEUE_PREFIX) && name.endsWith(QUEUE_SUFFIX) && !name.startsWith(QUEUE_TMP_PREFIX)) {
            result += file.size();
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
    return result;
}

std::vector<String> SubmissionQueueManager::listFiles() {
    std::vector<String> files;
    File root = LittleFS.open("/");
    if (!root) return files;

    File file = root.openNextFile();
    while (file) {
        String name = file.name();
        if (!name.startsWith("/")) name = "/" + name;
        if (name.startsWith(QUEUE_PREFIX) && name.endsWith(QUEUE_SUFFIX) && !name.startsWith(QUEUE_TMP_PREFIX)) {
            files.push_back(name);
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();

    std::sort(files.begin(), files.end());
    return files;
}

bool SubmissionQueueManager::enqueue(const String& payload, const String& tokenEntry) {
    if (payload.length() == 0 || payload.length() > MAX_PAYLOAD_BYTES) {
        Logger.add("Ответ не сохранен в очередь: неподходящий размер " + String(payload.length()) + " bytes", "error");
        return false;
    }

    int queued = count();
    size_t queuedBytes = totalBytes();
    size_t newBytes = tokenEntry.length() + payload.length() + strlen(PAYLOAD_SEPARATOR) + 16;
    if (queued >= MAX_QUEUE_FILES || queuedBytes + newBytes > MAX_QUEUE_BYTES) {
        Logger.add("Очередь ответов заполнена: " + String(queued) + " files, " + String(queuedBytes / 1024) + " KB", "error");
        return false;
    }

    String finalPath = makeQueuePath();
    String tmpPath = String(QUEUE_TMP_PREFIX) + finalPath.substring(strlen(QUEUE_PREFIX));
    LittleFS.remove(tmpPath);

    File file = LittleFS.open(tmpPath, FILE_WRITE);
    if (!file) {
        Logger.add("Не удалось открыть файл очереди ответов.", "error");
        return false;
    }

    file.print(tokenEntry);
    file.print(PAYLOAD_SEPARATOR);
    file.print(payload);
    file.close();

    LittleFS.remove(finalPath);
    if (!LittleFS.rename(tmpPath, finalPath)) {
        LittleFS.remove(tmpPath);
        Logger.add("Не удалось сохранить файл очереди ответов.", "error");
        return false;
    }

    Logger.add("Ответ сохранен в очередь: " + finalPath + ", всего: " + String(queued + 1));
    return true;
}

bool SubmissionQueueManager::readQueueFile(const String& path, String& tokenEntry, String& payload) {
    File file = LittleFS.open(path, FILE_READ);
    if (!file) return false;

    String content = file.readString();
    file.close();

    int sep = content.indexOf(PAYLOAD_SEPARATOR);
    if (sep < 0) return false;

    tokenEntry = content.substring(0, sep);
    payload = content.substring(sep + strlen(PAYLOAD_SEPARATOR));
    tokenEntry.trim();
    payload.trim();
    return tokenEntry.length() > 0 && payload.length() > 0;
}

bool SubmissionQueueManager::sendNow(const String& payload, String& response) {
    response = "";
    if (!ESPNetwork.isConnected()) {
        response = "ERROR_CONNECTION";
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    http.begin(client, Config.cloud_url);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setTimeout(30000);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");

    const char* headerKeys[] = {"Location"};
    http.collectHeaders(headerKeys, 1);

    int postCode = http.POST(payload);
    Logger.add("HTTP POST Код (Отправка): " + String(postCode));

    bool ok = false;
    if (postCode == 302 || postCode == 303) {
        String redirectUrl = http.header("Location");
        http.end();

        if (redirectUrl.length() == 0) {
            ok = true;
        } else {
            WiFiClientSecure redirectClient;
            redirectClient.setInsecure();
            HTTPClient redirectHttp;
            redirectHttp.begin(redirectClient, redirectUrl);
            redirectHttp.setTimeout(30000);
            redirectHttp.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
            int getCode = redirectHttp.GET();
            Logger.add("HTTP GET Код (Редирект): " + String(getCode));
            if (getCode == 200) {
                response = redirectHttp.getString();
                response.trim();
                ok = response.length() == 0 || response == "SUCCESS" || response.indexOf("ERROR") == -1;
            } else {
                ok = false;
            }
            redirectHttp.end();
        }
    } else if (postCode == 200) {
        response = http.getString();
        response.trim();
        ok = response.length() == 0 || response == "SUCCESS" || response.indexOf("ERROR") == -1;
        http.end();
    } else {
        response = "ERROR_CONNECTION";
        Logger.add("Ошибка отправки ответа: " + http.errorToString(postCode), "error");
        http.end();
    }

    if (ok) response = "SUCCESS";
    else if (response.length() == 0) response = "ERROR_CONNECTION";
    return ok;
}

bool SubmissionQueueManager::flush() {
    if (!ESPNetwork.isConnected()) return false;

    std::vector<String> files = listFiles();

    if (files.empty()) return true;
    Logger.add("Отправка очереди ответов: " + String((int)files.size()));

    int sent = 0;
    for (const String& path : files) {
        String tokenEntry = "";
        String payload = "";
        if (!readQueueFile(path, tokenEntry, payload)) {
            Logger.add("Удаляю поврежденный файл очереди: " + path, "warn");
            LittleFS.remove(path);
            continue;
        }

        String response = "";
        if (!sendNow(payload, response)) {
            Logger.add("Очередь ответов остановлена, интернет/сервер недоступен. Осталось: " + String(count()), "warn");
            return false;
        }

        LittleFS.remove(path);
        sent++;
        delay(100);
    }

    Logger.add("Очередь ответов отправлена: " + String(sent));
    return true;
}
