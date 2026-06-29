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

    void begin();
    bool fetchFromServer();
    String getLocalToken(const String& formNumber, IPAddress ip);
    String urlEncode(const String& str);

};

extern FormManager Forms;

#endif
