#include "Logger.h"
#include <time.h>

SystemLogger Logger;

void SystemLogger::begin() {
    // Initialization if necessary
}

void SystemLogger::add(const String& msg, const String& level) {
    struct tm timeinfo;
    String timeStr = "";
    if (getLocalTime(&timeinfo, 10) && timeinfo.tm_year > 120) {
        char timeStringBuff[50];
        strftime(timeStringBuff, sizeof(timeStringBuff), "[%H:%M:%S] ", &timeinfo);
        timeStr = String(timeStringBuff);
    } else {
        timeStr = "[" + String(millis() / 1000) + " сек] ";
    }
    
    String fullMsg = timeStr + msg;
    Serial.println(fullMsg);
    
    // Add to in-memory buffer
    logBuffer[logIndex] = fullMsg;
    logIndex++;
    if (logIndex >= MAX_LOG_LINES) {
        logIndex = 0;
        logFull = true;
    }

    // Add to cloud queue (keep max 100 to avoid memory leak if offline)
    if (cloudQueue.size() < 100) {
        cloudQueue.push_back({level, msg});
    }
}

String SystemLogger::getSystemLog() {
    String res = "";
    if (logFull) {
        for (int i = logIndex - 1; i >= 0; i--) res += logBuffer[i] + "\n";
        for (int i = MAX_LOG_LINES - 1; i >= logIndex; i--) res += logBuffer[i] + "\n";
    } else {
        for (int i = logIndex - 1; i >= 0; i--) res += logBuffer[i] + "\n";
    }
    return res;
}

void SystemLogger::clear() {
    logIndex = 0;
    logFull = false;
    Logger.add("Системный лог очищен пользователем.");
}

bool SystemLogger::hasPendingCloudLogs() {
    return !cloudQueue.empty();
}

LogEntry SystemLogger::popCloudLog() {
    if (cloudQueue.empty()) return {"", ""};
    LogEntry entry = cloudQueue.front();
    cloudQueue.erase(cloudQueue.begin());
    return entry;
}
