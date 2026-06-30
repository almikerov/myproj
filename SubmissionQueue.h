#ifndef SUBMISSION_QUEUE_H
#define SUBMISSION_QUEUE_H

#include <Arduino.h>
#include <vector>

class SubmissionQueueManager {
public:
    void begin();
    bool enqueue(const String& payload, const String& tokenEntry);
    bool flush();
    bool sendNow(const String& payload, String& response);
    int count();
    size_t totalBytes();
    std::vector<String> listFiles();
    bool readQueueFile(const String& path, String& tokenEntry, String& payload);

private:
    static const int MAX_QUEUE_FILES = 80;
    static const size_t MAX_QUEUE_BYTES = 900 * 1024;
    static const size_t MAX_PAYLOAD_BYTES = 24 * 1024;

    String makeQueuePath();
    void removeTempFiles();
};

extern SubmissionQueueManager SubmissionQueue;

#endif
