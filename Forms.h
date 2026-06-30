#ifndef FORMS_H
#define FORMS_H

#include <Arduino.h>
#include <vector>

class FormManager {
public:
    int formCount = 0;
    std::vector<String> formTitles;
    std::vector<int> formMaxDaily;
    std::vector<uint32_t> formHtmlOffsets;
    std::vector<bool> formReady;
    std::vector<String> formRevisions;
    std::vector<String> formHtmlCache;

    bool progressActive = false;
    String progressPhase = "idle";
    String progressMessage = "";
    int progressForm = 0;
    int progressForms = 0;
    int progressReady = 0;
    int progressPercent = 0;
    size_t progressBytes = 0;
    size_t progressTotalBytes = 0;
    int progressImage = 0;
    int progressImages = 0;
    unsigned long progressUpdatedAt = 0;
    volatile bool syncRequested = false;
    volatile bool syncInProgress = false;
    bool updatesAvailable = false;
    String updateStatus = "";
    unsigned long lastUpdateCheckAt = 0;

    void begin();
    bool loadCachedForms();
    bool fetchFromServer();
    bool checkForUpdates();
    void requestSync();
    bool cacheFormHtml(int index);
    void cacheAllReadyForms();
    int readyCount() const;
    void setProgress(const String& phase, const String& message, int form, int forms, int ready, int percent, size_t bytes, size_t totalBytes, int image, int images);
    void finishProgress(const String& message);
    String getLocalToken(const String& formNumber, IPAddress ip);
    String urlEncode(const String& str);

};

extern FormManager Forms;

#endif
