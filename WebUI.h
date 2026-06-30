#ifndef WEBUI_H
#define WEBUI_H

#include <Arduino.h>
#include <WebServer.h>

class WebUIManager {
public:
    void begin();
    void loop();

private:
    WebServer server{80};
    
    bool checkAuth();
    String getHtmlHeader();
    
    void handleRoot();
    void handleLogin();
    void handleLogout();
    void handleChangePass();
    void handleImage();
    void handleForm();
    void handleFormBody();
    void handleSubmit();
    void handleAdmin();
    void handleProgress();
    void handleSaveSettings();
    void handleUpdate();
    void handleReboot();
    void handleNotFound();
};

extern WebUIManager WebUI;

#endif
