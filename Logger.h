#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>
#include <vector>

struct LogEntry {
    String level;
    String message;
};

class SystemLogger {
public:
    void begin();
    void add(const String& msg, const String& level = "info");
    String getSystemLog();
    void clear();
    
    // For CloudClient to consume
    bool hasPendingCloudLogs();
    LogEntry popCloudLog();

private:
    static const int MAX_LOG_LINES = 50;
    String logBuffer[MAX_LOG_LINES];
    int logIndex = 0;
    bool logFull = false;

    std::vector<LogEntry> cloudQueue;
};

extern SystemLogger Logger;

#endif
