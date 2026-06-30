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

static const size_t MAX_FORM_HTML_CACHE_BYTES = 220 * 1024;

static int countDataImages(const String& fileName, uint32_t htmlOffset);
static bool splitEmbeddedImages(const String& fileName, int formIndex, uint32_t htmlOffset, int expectedImages, int readyCount, int totalForms);
static void removeImagesForForm(int formIndex);
static bool prepareCachedFormForRam(int formIndex, uint32_t htmlOffset, int readyCount, int totalForms);

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

void FormManager::requestSync() {
    syncRequested = true;
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
    if (expectedSize > MAX_FORM_HTML_CACHE_BYTES) {
        file.close();
        if (countDataImages(fileName, offset) > 0 && prepareCachedFormForRam(index, offset, readyCount(), formCount)) {
            return cacheFormHtml(index);
        }
        Logger.add("Form cache failed: HTML for #" + String(index + 1) + " is too large for RAM (" + String(expectedSize) + " bytes)", "error");
        return false;
    }

    String html = "";
    if (!html.reserve(expectedSize + 1)) {
        file.close();
        Logger.add("Form cache failed: not enough RAM for #" + String(index + 1) + " (" + String(expectedSize) + " bytes)", "error");
        return false;
    }
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

        if (prepareCachedFormForRam(i, formHtmlOffsets[i], cached, formCount) && cacheFormHtml(i)) {
            cached++;
            totalBytes += formHtmlCache[i].length();
        } else {
            formReady[i] = false;
        }
    }

    Logger.add("RAM form cache ready: " + String(cached) + " of " + String(formCount) + ", total " + String(totalBytes) + " bytes");
}

static const int FORM_DOWNLOAD_TIMEOUT_MS = 60000;
static const int FORM_DOWNLOAD_IDLE_TIMEOUT_MS = 20000;
static const size_t FORM_DOWNLOAD_BUFFER_SIZE = 1024;
static const size_t FORM_DOWNLOAD_PROGRESS_STEP = 32768;

static String bytesToKbString(size_t bytes) {
    return String(bytes / 1024) + " KB";
}

static void reportFormDownloadProgress(const String& label, int form, int forms, int ready, size_t bytesWritten, size_t totalBytes, unsigned long startedAt) {
    String sizePart = bytesToKbString(bytesWritten);
    int formPercent = 10;
    if (totalBytes > 0) {
        sizePart += " / " + bytesToKbString(totalBytes);
        formPercent = 10 + (int)((bytesWritten * 70ULL) / totalBytes);
        if (formPercent > 80) formPercent = 80;
    }

    Forms.setProgress("download", "Download form " + String(form) + " of " + String(forms), form, forms, ready, overallProgressPercent(ready, forms, formPercent), bytesWritten, totalBytes, 0, 0);
    Logger.add(label + ": " + sizePart + " in " + String((millis() - startedAt) / 1000) + "s");
}

static bool downloadHttpBodyToFile(HTTPClient& http, File& file, const String& label, int form, int forms, int ready, size_t totalBytes, size_t& bytesWritten) {
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        Logger.add(label + ": no HTTP stream", "error");
        return false;
    }

    uint8_t buffer[FORM_DOWNLOAD_BUFFER_SIZE];
    unsigned long startedAt = millis();
    unsigned long lastDataAt = startedAt;
    size_t nextProgressBytes = FORM_DOWNLOAD_PROGRESS_STEP;
    bytesWritten = 0;

    while (true) {
        int availableBytes = stream->available();
        if (availableBytes > 0) {
            size_t toRead = (size_t)availableBytes;
            if (toRead > sizeof(buffer)) toRead = sizeof(buffer);

            int readLen = stream->readBytes(buffer, toRead);
            if (readLen > 0) {
                size_t written = file.write(buffer, (size_t)readLen);
                if (written != (size_t)readLen) {
                    Logger.add(label + ": LittleFS write failed at " + String(bytesWritten) + " bytes", "error");
                    return false;
                }

                bytesWritten += written;
                lastDataAt = millis();

                if (bytesWritten >= nextProgressBytes) {
                    reportFormDownloadProgress(label, form, forms, ready, bytesWritten, totalBytes, startedAt);
                    nextProgressBytes = bytesWritten + FORM_DOWNLOAD_PROGRESS_STEP;
                }

                if (totalBytes > 0 && bytesWritten >= totalBytes) break;
            }
        } else {
            if (totalBytes > 0 && bytesWritten >= totalBytes) break;
            if (!http.connected()) {
                if (totalBytes == 0 && bytesWritten > 0) break;
                break;
            }
            if (millis() - lastDataAt > FORM_DOWNLOAD_IDLE_TIMEOUT_MS) {
                Logger.add(label + ": download stalled after " + bytesToKbString(bytesWritten), "error");
                return false;
            }
            delay(5);
            esp_task_wdt_reset();
        }
    }

    file.flush();
    if (bytesWritten > 0) {
        reportFormDownloadProgress(label, form, forms, ready, bytesWritten, totalBytes, startedAt);
    }

    if (totalBytes > 0 && bytesWritten < totalBytes) {
        Logger.add(label + ": connection closed early, got " + String(bytesWritten) + " / " + String(totalBytes) + " bytes", "error");
        return false;
    }

    return bytesWritten > 0;
}

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

static bool writeDecodedByte(File& out, uint8_t value, size_t& decodedBytes) {
    if (out.write(value) != 1) return false;
    decodedBytes++;
    return true;
}

static bool writeBase64Decoded(File& out, char c, int quartet[4], int& quartetLen, size_t& decodedBytes) {
    int v = base64Value(c);
    if (v == -3) return true;
    if (v == -1) return false;

    quartet[quartetLen++] = v;
    if (quartetLen < 4) return true;

    if (quartet[0] < 0 || quartet[1] < 0) return false;

    uint8_t b0 = (quartet[0] << 2) | (quartet[1] >> 4);
    if (!writeDecodedByte(out, b0, decodedBytes)) return false;

    if (quartet[2] != -2) {
        if (quartet[2] < 0) return false;
        uint8_t b1 = ((quartet[1] & 0x0F) << 4) | (quartet[2] >> 2);
        if (!writeDecodedByte(out, b1, decodedBytes)) return false;
    }

    if (quartet[3] != -2) {
        if (quartet[2] < 0 || quartet[3] < 0) return false;
        uint8_t b2 = ((quartet[2] & 0x03) << 6) | quartet[3];
        if (!writeDecodedByte(out, b2, decodedBytes)) return false;
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

static void trackOutputChar(String& tail, char c) {
    tail += c;
    if (tail.length() > 32) {
        tail.remove(0, tail.length() - 32);
    }
}

static void writeTrackedChar(File& out, String& tail, char c) {
    out.write((uint8_t)c);
    trackOutputChar(tail, c);
}

static void writeTrackedString(File& out, String& tail, const String& value) {
    out.print(value);
    for (unsigned int i = 0; i < value.length(); i++) {
        trackOutputChar(tail, value[i]);
    }
}

static char dataUriTerminatorFromTail(const String& tail) {
    if (tail.length() == 0) return 0;
    char last = tail[tail.length() - 1];
    if (last == '\'' || last == '"') return last;
    if (last == '(') return ')';
    return 0;
}

static bool isDataUriTerminator(char c, char terminator) {
    if (terminator != 0) return c == terminator;
    return c == '\'' || c == '"' || c == ')' || c == '<' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool extractAndReplaceDataImageUri(File& in, File& out, int formIndex, int& imageIndex, char terminator, String& tail, const String& matchedPrefix, size_t expectedImages) {
    String meta = "image/";
    meta.reserve(64);
    bool foundComma = false;
    String failure = "";

    while (in.available()) {
        char c = in.read();
        if (c == ',') {
            foundComma = true;
            break;
        }
        if (isDataUriTerminator(c, terminator) || meta.length() > 96) {
            failure = "bad data URI header";
            writeTrackedString(out, tail, matchedPrefix + meta);
            writeTrackedChar(out, tail, c);
            Logger.add("Form #" + String(formIndex + 1) + " image " + String(imageIndex + 1) + " skipped: " + failure, "warn");
            return false;
        }
        meta += c;
    }

    if (!foundComma) {
        failure = "missing base64 comma";
        writeTrackedString(out, tail, matchedPrefix + meta);
        Logger.add("Form #" + String(formIndex + 1) + " image " + String(imageIndex + 1) + " skipped: " + failure, "warn");
        return false;
    }

    String metaLower = meta;
    metaLower.toLowerCase();
    int b64Idx = metaLower.indexOf(";base64");
    if (b64Idx == -1) {
        failure = "not base64";
        writeTrackedString(out, tail, matchedPrefix + meta + ",");
        Logger.add("Form #" + String(formIndex + 1) + " image " + String(imageIndex + 1) + " skipped: " + failure, "warn");
        return false;
    }

    String mime = metaLower.substring(0, b64Idx);
    String imgPath = "/img_" + String(formIndex) + "_" + String(imageIndex) + imageExtensionForMime(mime);
    LittleFS.remove(imgPath);

    File img = LittleFS.open(imgPath, FILE_WRITE);
    if (!img) {
        failure = "cannot open image file";
        writeTrackedString(out, tail, "");
        Logger.add("Form #" + String(formIndex + 1) + " image " + String(imageIndex + 1) + " skipped: " + failure, "error");
        return false;
    }

    int quartet[4] = {0, 0, 0, 0};
    int quartetLen = 0;
    uint32_t b64Chars = 0;
    bool ok = true;
    bool ended = false;
    char endChar = 0;
    size_t imageSize = 0;

    while (in.available()) {
        char c = in.read();
        if (isDataUriTerminator(c, terminator)) {
            ended = true;
            endChar = c;
            break;
        }

        if (!writeBase64Decoded(img, c, quartet, quartetLen, imageSize)) {
            ok = false;
        }

        b64Chars++;
        if ((b64Chars & 0x0FFF) == 0) {
            delay(1);
            esp_task_wdt_reset();
        }
    }

    if (ok && quartetLen == 2) {
        if (quartet[0] >= 0 && quartet[1] >= 0) ok = writeDecodedByte(img, (uint8_t)((quartet[0] << 2) | (quartet[1] >> 4)), imageSize);
    } else if (ok && quartetLen == 3) {
        if (quartet[0] >= 0 && quartet[1] >= 0 && quartet[2] >= 0) {
            ok = writeDecodedByte(img, (uint8_t)((quartet[0] << 2) | (quartet[1] >> 4)), imageSize);
            if (ok) ok = writeDecodedByte(img, (uint8_t)(((quartet[1] & 0x0F) << 4) | (quartet[2] >> 2)), imageSize);
        }
    } else if (quartetLen != 0) {
        ok = false;
    }

    img.close();

    if (!ok || !ended || imageSize == 0) {
        LittleFS.remove(imgPath);
        writeTrackedString(out, tail, "");
        if (ended) writeTrackedChar(out, tail, endChar);
        if (!ok) failure = "invalid base64";
        else if (!ended) failure = "missing URI terminator";
        else failure = "empty image file";
        Logger.add("Form #" + String(formIndex + 1) + " image " + String(imageIndex + 1) + " skipped: " + failure + ", chars: " + String(b64Chars), "warn");
        return false;
    }

    writeTrackedString(out, tail, imgPath);
    writeTrackedChar(out, tail, endChar);

    imageIndex++;
    Logger.add("Form #" + String(formIndex + 1) + " image " + String(imageIndex) + "/" + String(expectedImages) + " saved: " + String(imageSize / 1024) + " KB");
    return true;
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
    size_t imageSize = 0;

    while (in.available()) {
        char c = in.read();
        if (c == quote) {
            break;
        }
        if (!writeBase64Decoded(img, c, quartet, quartetLen, imageSize)) {
            ok = false;
        }
        b64Chars++;
        if ((b64Chars & 0x0FFF) == 0) {
            delay(1);
            esp_task_wdt_reset();
        }
    }

    if (ok && quartetLen == 2) {
        if (quartet[0] >= 0 && quartet[1] >= 0) ok = writeDecodedByte(img, (uint8_t)((quartet[0] << 2) | (quartet[1] >> 4)), imageSize);
    } else if (ok && quartetLen == 3) {
        if (quartet[0] >= 0 && quartet[1] >= 0 && quartet[2] >= 0) {
            ok = writeDecodedByte(img, (uint8_t)((quartet[0] << 2) | (quartet[1] >> 4)), imageSize);
            if (ok) ok = writeDecodedByte(img, (uint8_t)(((quartet[1] & 0x0F) << 4) | (quartet[2] >> 2)), imageSize);
        }
    } else if (quartetLen != 0) {
        ok = false;
    }

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

    const char* needle = "data:image/";
    int match = 0;
    int imageIndex = 0;
    int extracted = 0;
    String candidate = "";
    candidate.reserve(16);
    String tail = "";
    tail.reserve(40);
    size_t totalSize = in.size();
    uint32_t nextProgressPos = htmlOffset + 32768;

    while (in.available()) {
        char c = in.read();

        if (c == needle[match]) {
            candidate += c;
            match++;
            if (needle[match] == '\0') {
                Forms.setProgress("images", "Обработка картинок " + String(imageIndex + 1) + " из " + String(expectedImages), formIndex + 1, totalForms, readyCount, overallProgressPercent(readyCount, totalForms, 85), in.position(), totalSize, imageIndex + 1, expectedImages);
                char terminator = dataUriTerminatorFromTail(tail);
                if (extractAndReplaceDataImageUri(in, out, formIndex, imageIndex, terminator, tail, candidate, expectedImages)) {
                    extracted++;
                }
                candidate = "";
                match = 0;
            }
        } else {
            if (match > 0) {
                writeTrackedString(out, tail, candidate);
                candidate = "";
                match = 0;

                if (c == needle[0]) {
                    candidate += c;
                    match = 1;
                } else {
                    writeTrackedChar(out, tail, c);
                }
            } else {
                writeTrackedChar(out, tail, c);
            }
        }

        if ((in.position() & 0x0FFF) == 0) {
            delay(1);
            esp_task_wdt_reset();
        }
        if (in.position() >= nextProgressPos) {
            int scanPercent = totalSize > htmlOffset ? (int)(((in.position() - htmlOffset) * 100ULL) / (totalSize - htmlOffset)) : 100;
            int formPercent = 80 + (scanPercent / 5);
            Forms.setProgress("images", "Обработка картинок " + String(extracted) + " из " + String(expectedImages), formIndex + 1, totalForms, readyCount, overallProgressPercent(readyCount, totalForms, formPercent), in.position(), totalSize, extracted, expectedImages);
            nextProgressPos += 32768;
        }
    }

    if (candidate.length() > 0) writeTrackedString(out, tail, candidate);

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

static bool prepareCachedFormForRam(int formIndex, uint32_t htmlOffset, int readyCount, int totalForms) {
    String fileName = "/raw_" + String(formIndex) + ".txt";
    int dataImageCount = countDataImages(fileName, htmlOffset);
    if (dataImageCount <= 0) return true;

    Logger.add("Cached form #" + String(formIndex + 1) + " contains embedded images: " + String(dataImageCount) + ". Preparing fast cache...");
    Forms.setProgress("images", "Preparing cached form " + String(formIndex + 1), formIndex + 1, totalForms, readyCount, overallProgressPercent(readyCount, totalForms, 82), 0, 0, 0, dataImageCount);

    removeImagesForForm(formIndex);
    bool ok = splitEmbeddedImages(fileName, formIndex, htmlOffset, dataImageCount, readyCount, totalForms);
    if (ok) {
        Logger.add("Cached form #" + String(formIndex + 1) + " prepared for fast opening.");
    } else {
        Logger.add("Cached form #" + String(formIndex + 1) + " image preparation failed.", "error");
    }
    return ok;
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

bool FormManager::checkForUpdates() {
    lastUpdateCheckAt = millis();
    updatesAvailable = false;

    if (syncInProgress) {
        updateStatus = "Синхронизация уже идет";
        return false;
    }

    if (!ESPNetwork.isConnected()) {
        updateStatus = "Нет сети для проверки версий";
        Logger.add(updateStatus);
        return false;
    }

    Logger.add("Проверка версий опросников...");

    int remoteCount = -1;
    {
        WiFiClientSecure countClient;
        countClient.setInsecure();
        HTTPClient http;
        http.begin(countClient, Config.cloud_url + "?action=getFormCount");
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.setTimeout(15000);
        http.setReuse(false);
        http.useHTTP10(true);
        http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");

        int code = http.GET();
        if (code == 200) {
            remoteCount = http.getString().toInt();
        } else {
            Logger.add("Не удалось проверить список опросников: HTTP " + String(code), "warn");
        }
        http.end();
    }

    if (remoteCount <= 0) {
        updateStatus = "Список опросников не получен";
        return false;
    }

    if (remoteCount != formCount) {
        updatesAvailable = true;
        updateStatus = "Изменилось количество опросников: " + String(formCount) + " -> " + String(remoteCount);
        Logger.add(updateStatus, "warn");
        return true;
    }

    bool foundUpdates = false;
    int checked = 0;

    for (int i = 0; i < remoteCount; i++) {
        esp_task_wdt_reset();

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
            String remoteTitle = "";
            String remoteRevision = "";
            int remoteMaxDaily = 1;
            if (parseMetaPayload(metaHttp.getString(), remoteTitle, remoteMaxDaily, remoteRevision) && remoteRevision.length() > 0) {
                String localRevision = "";
                if (i < (int)formRevisions.size()) localRevision = formRevisions[i];
                if (localRevision.length() == 0) readSmallFile(metaPathForForm(i), localRevision);

                bool ready = i < (int)formReady.size() && formReady[i];
                if (localRevision.length() == 0 && ready) {
                    if (i >= (int)formRevisions.size()) formRevisions.resize(remoteCount, "");
                    formRevisions[i] = remoteRevision;
                    writeSmallFile(metaPathForForm(i), remoteRevision);
                    Logger.add("Form #" + String(i + 1) + " revision metadata initialized.");
                } else if (localRevision.length() == 0 || localRevision != remoteRevision) {
                    foundUpdates = true;
                    Logger.add("Form #" + String(i + 1) + " has a newer version.", "warn");
                }
                checked++;
            }
        } else {
            Logger.add("Не удалось проверить опрос #" + String(i + 1) + ": HTTP " + String(metaCode), "warn");
        }
        metaHttp.end();
        delay(20);
    }

    updatesAvailable = foundUpdates;
    if (foundUpdates) {
        updateStatus = "Есть новые версии опросников. Нажми обновление, когда удобно.";
        Logger.add(updateStatus, "warn");
    } else {
        updateStatus = "Версии опросников актуальны. Проверено: " + String(checked) + " из " + String(remoteCount);
        Logger.add(updateStatus);
    }

    return foundUpdates;
}

String FormManager::getLocalToken(const String& formNumber, IPAddress ip) {
    int hash = (ip[3] * 137) ^ (formNumber.toInt() * 89);
    hash = hash + 4096; 
    char hexBuf[10]; 
    sprintf(hexBuf, "%X", hash);
    return "TK-" + String(hexBuf); 
}

bool FormManager::fetchFromServer() {
    syncInProgress = true;
    finishProgress("Проверка сети");
    if (!ESPNetwork.isConnected()) {
        Logger.add("Нет сети, пропускаем скачивание опросников.");
        syncInProgress = false;
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
        syncInProgress = false;
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
    std::vector<String> pendingRevisions(formCount, "");
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
                    if (!prepareCachedFormForRam(i, formHtmlOffsets[i], loadedCount, formCount) || !cacheFormHtml(i)) {
                        formReady[i] = false;
                        if (loadedCount > 0) loadedCount--;
                    }
                    setProgress("cache", "Опрос " + String(i + 1) + " из " + String(formCount) + ": без изменений", i + 1, formCount, loadedCount, overallProgressPercent(loadedCount, formCount, 100), 0, 0, 0, 0);
                    Logger.add("Form #" + String(i + 1) + " unchanged, using cache.");
                } else if (hasPublishedCache && formReady[i] && formRevisions[i].length() == 0 && remoteRevision.length() > 0) {
                    bool cacheOk = prepareCachedFormForRam(i, formHtmlOffsets[i], loadedCount, formCount) && cacheFormHtml(i);
                    if (cacheOk) {
                        shouldDownload = false;
                        formRevisions[i] = remoteRevision;
                        writeSmallFile(metaPathForForm(i), remoteRevision);
                        setProgress("cache", "Опрос " + String(i + 1) + " из " + String(formCount) + ": кэш принят", i + 1, formCount, loadedCount, overallProgressPercent(loadedCount, formCount, 100), 0, 0, 0, 0);
                        Logger.add("Form #" + String(i + 1) + " existing cache adopted with remote revision.");
                    } else {
                        formReady[i] = false;
                        if (loadedCount > 0) loadedCount--;
                    }
                } else if (remoteRevision.length() > 0) {
                    if (!hasPublishedCache || !formReady[i]) {
                        if (formReady[i] && loadedCount > 0) loadedCount--;
                        formReady[i] = false;
                        if (i < (int)formHtmlCache.size()) formHtmlCache[i] = "";
                    } else {
                        Logger.add("Form #" + String(i + 1) + " update is available; current cache stays active until download succeeds.");
                    }
                    pendingRevisions[i] = remoteRevision;
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
                size_t bytesWritten = 0;
                bool downloadOk = downloadHttpBodyToFile(formHttp, file, "Downloading form #" + String(i + 1), i + 1, formCount, loadedCount, totalBytes, bytesWritten);
                file.close();

                if (!downloadOk || bytesWritten == 0 || (totalBytes > 0 && bytesWritten < totalBytes)) {
                    Logger.add("Empty or interrupted form file #" + String(i + 1) + ". Bytes: " + String(bytesWritten) + " / " + String(totalBytes), "error");
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
                            if (pendingRevisions[i].length() > 0) formRevisions[i] = pendingRevisions[i];
                            writeSmallFile(metaPathForForm(i), formRevisions[i]);
                            if (!cacheFormHtml(i)) {
                                formReady[i] = false;
                                if (loadedCount > 0) loadedCount--;
                            }
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
                if (idx < (int)pendingRevisions.size() && pendingRevisions[idx].length() > 0) formRevisions[idx] = pendingRevisions[idx];
                writeSmallFile(metaPathForForm(idx), formRevisions[idx]);
                if (!cacheFormHtml(idx)) {
                    formReady[idx] = false;
                    if (loadedCount > 0) loadedCount--;
                }
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
    syncInProgress = false;
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
