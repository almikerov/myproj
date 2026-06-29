#include "WebUI.h"
#include "Config.h"
#include "Logger.h"
#include "ESPNetwork.h"
#include "Forms.h"
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

WebUIManager WebUI;

void WebUIManager::begin() {
    const char * headerKeys[] = {"Cookie", "Host"};
    server.collectHeaders(headerKeys, 2);

    server.on("/", [this]() { handleRoot(); });
    server.on("/login", HTTP_POST, [this]() { handleLogin(); });
    server.on("/logout", [this]() { handleLogout(); });
    server.on("/change_pass", HTTP_POST, [this]() { handleChangePass(); });
    server.on("/form", [this]() { handleForm(); });
    server.on("/submit", HTTP_POST, [this]() { handleSubmit(); });
    server.on("/admin", [this]() { handleAdmin(); });
    server.on("/save_settings", HTTP_POST, [this]() { handleSaveSettings(); });
    server.on("/update", [this]() { handleUpdate(); });
    server.on("/reboot", [this]() { handleReboot(); });
    
    // Explicit Captive Portal routes for aggressive OSes
    server.on("/generate_204", [this]() { handleNotFound(); }); // Android
    server.on("/gen_204", [this]() { handleNotFound(); });      // Android
    server.on("/hotspot-detect.html", [this]() { handleNotFound(); }); // iOS
    server.on("/ncsi.txt", [this]() { handleNotFound(); });     // Windows
    
    server.onNotFound([this]() { handleNotFound(); });

    server.begin();
}

void WebUIManager::loop() {
    server.handleClient();
}

bool WebUIManager::checkAuth() {
    if (server.hasHeader("Cookie")) {
        String cookie = server.header("Cookie");
        if (cookie.indexOf("SESSION=" + Config.admin_pass) != -1) return true;
    }
    return false;
}

String WebUIManager::getHtmlHeader() {
    String html = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<style>body{font-family:sans-serif; background:#f4f7f6; padding:20px; padding-top:80px; color:#333; margin:0;} "; 
    html += ".top-bar {position:fixed; top:0; left:0; width:100%; background:#1a73e8; color:white; text-align:left; padding:20px 25px; box-sizing:border-box; font-weight:bold; font-size:18px; box-shadow:0 4px 6px rgba(0,0,0,0.1); cursor:pointer; z-index:1000;} ";
    html += ".form-card{background:white; max-width:450px; margin:0 auto; padding:30px; border-radius:16px; box-shadow:0 8px 24px rgba(0,0,0,0.05);} ";
    html += ".top-token{text-align:center; background:#f8f9fa; color:#95a5a6; font-size:11px; padding:6px; border-radius:8px; margin-bottom:20px; font-family:monospace; border:1px dashed #ddd; letter-spacing:1px;} ";
    html += "h2{text-align:center; margin-bottom:20px; color:#1a73e8;} .form-label{font-weight:normal; display:block; margin-top:15px; margin-bottom:5px;} ";
    html += ".req-label::after { content: ' *'; color: #ff8c00; font-size: 16px; margin-left: 4px;} ";
    html += "input[type='text'], input[type='password'], select, textarea{width:100%; padding:12px; border:1px solid #ddd; border-radius:8px; box-sizing:border-box; font-size:16px; font-family:inherit;} ";
    html += ".radio-group, .scale-wrapper {background:#fafafa; padding:10px; border-radius:8px; border:2px solid #ddd; transition: 0.3s;} ";
    html += ".radio-label{display:flex; align-items:center; font-weight:normal; margin-top:5px; cursor:pointer;} .radio-label input{width:auto; margin-right:10px;} ";
    html += ".scale-wrapper { display: flex; justify-content: space-between; gap: 4px; margin-top: 10px; flex-wrap: nowrap; box-sizing: border-box; overflow-x: auto; padding-bottom: 5px; } ";
    html += ".scale-btn { flex: 1 1 0; min-width: 0; text-align: center; cursor: pointer; } ";
    html += ".scale-btn input { position: absolute; opacity: 0; width: 0; height: 0; } ";
    html += ".scale-btn span { display: block; padding: 12px 0; background: #e8eaed; border-radius: 8px; font-weight: bold; color: #5f6368; transition: 0.2s; box-sizing: border-box; border: 2px solid transparent; font-size: 14px;} ";
    html += ".scale-btn input:checked + span { background: #1a73e8; color: white; border-color: #1a73e8; transform: scale(1.05); box-shadow: 0 4px 10px rgba(26,115,232,0.3); } ";
    html += ".btn{background:#1a73e8; color:white; border:none; width:100%; padding:15px; border-radius:8px; font-weight:bold; font-size:16px; margin-top:20px; cursor:pointer; display:block; text-align:center; text-decoration:none; box-sizing:border-box;} ";
    html += ".btn-green{background:#2ecc71;} .btn-orange{background:#f39c12;} .btn-red{background:#e74c3c;} .btn-gray{background:#7f8c8d;} ";
    html += ".loader{border:5px solid #f3f3f3; border-top:5px solid #1a73e8; border-radius:50%; width:50px; height:50px; animation:spin 1s linear infinite; margin:20px auto;} @keyframes spin{0%{transform:rotate(0deg);} 100%{transform:rotate(360deg);}}</style>";
    
    html += "<script>";
    html += "function sendData(e, f) { e.preventDefault(); var formEl = e.target; ";
    html += "var wrappers = formEl.querySelectorAll('.scale-wrapper, .radio-group'); wrappers.forEach(w => w.style.borderColor = '#ddd'); ";
    html += "var requiredRadios = {}; ";
    html += "formEl.querySelectorAll('input[required][type=\"radio\"]').forEach(r => { requiredRadios[r.name] = false; }); ";
    html += "formEl.querySelectorAll('input[type=\"radio\"]:checked').forEach(r => { if (requiredRadios[r.name] !== undefined) requiredRadios[r.name] = true; }); ";
    html += "var radioValid = true; var firstErrorEl = null; ";
    html += "for (var name in requiredRadios) { if (!requiredRadios[name]) { radioValid = false; ";
    html += "  var wrapper = formEl.querySelector('input[name=\"' + name + '\"]').closest('.scale-wrapper, .radio-group'); ";
    html += "  if (wrapper) { wrapper.style.borderColor = '#e74c3c'; if(!firstErrorEl) firstErrorEl = wrapper; } } } ";
    html += "if (!formEl.checkValidity() || !radioValid) { ";
    html += "  if(firstErrorEl) firstErrorEl.scrollIntoView({ behavior: 'smooth', block: 'center' }); ";
    html += "  alert('Пожалуйста, заполните все обязательные вопросы!'); return; } ";
    html += "document.getElementById(f).style.display='none'; document.getElementById('load').style.display='block'; "; 
    html += "var fd = new FormData(formEl); var d = new URLSearchParams(fd).toString(); ";
    html += "fetch('/submit', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:d}) ";
    html += ".then(r => r.text()).then(t => { document.getElementById('load').style.display='none'; document.getElementById('res').style.display='block'; ";
    html += "if(t.trim()=='SUCCESS') document.getElementById('res-msg').innerHTML='<h2 style=\"color:#2ecc71;\">✅ Успешно!</h2><p>Ваши данные сохранены.</p>'; ";
    html += "else { document.getElementById('res-msg').innerHTML='<h2 style=\"color:#e74c3c;\">❌ Доступ закрыт</h2><p>Лимит прохождений этого опроса исчерпан.</p>'; } }) ";
    html += ".catch(err => { document.getElementById('load').style.display='none'; document.getElementById('res').style.display='block'; document.getElementById('res-msg').innerHTML='<h2 style=\"color:#e74c3c;\">❌ Обрыв связи</h2><p>Плата потеряла сеть.</p>'; }); } ";
    html += "</script></head><body>";
    return html;
}

void WebUIManager::handleRoot() {
    if (server.hasHeader("Host")) {
        String host = server.header("Host");
        if (host.indexOf("192.168.4.1") == -1 && host.indexOf("arsenal.local") == -1 && host.indexOf(ESPNetwork.getIP()) == -1) {
            server.sendHeader("Location", "http://192.168.4.1/", true);
            server.send(302, "text/plain", "");
            return;
        }
    }

    String content = getHtmlHeader();
    content += "<div class='form-card'><div id='menu'><h2>Главное меню</h2>";
    for (int i = 0; i < Forms.formCount; i++) {
        String btnColor = "btn";
        if (i % 3 == 1) btnColor = "btn btn-green";
        if (i % 3 == 2) btnColor = "btn btn-orange";
        content += "<button class='" + btnColor + "' onclick=\"window.location.href='/form?id=" + String(i) + "&token=" + Forms.getLocalToken(String(i), server.client().remoteIP()) + "'\">" + Forms.formTitles[i] + "</button>";
    }
    content += "</div></div>";
    
    content += "<div style='text-align:center; margin-top:30px; margin-bottom:20px; padding:20px;'><span onclick=\"let n=Date.now(); if(n-(this.l||0)<600) window.location.href='/admin'; this.l=n;\" style='color:#bdc3c7; font-size:12px; letter-spacing:1px; cursor:pointer;'>АДМИН-ПАНЕЛЬ</span></div>";
    content += "</body></html>";
    server.send(200, "text/html", content);
}

void WebUIManager::handleLogin() {
    String p = server.arg("pwd");
    if (p == Config.admin_pass) {
        Logger.add("Успешный вход в админ-панель.");
        server.sendHeader("Set-Cookie", "SESSION=" + Config.admin_pass + "; Path=/; HttpOnly");
        server.sendHeader("Location", "/admin");
        server.send(302, "text/plain", "");
    } else {
        Logger.add("⚠️ Неудачная попытка входа в админ-панель!", "warn");
        server.send(200, "text/html", "<html><head><meta charset='utf-8'></head><body style='text-align:center;padding:50px;'><h2 style='color:#e74c3c;'>❌ Неверный пароль!</h2><br><button onclick=\"window.location.href='/admin'\" style='background:none; border:none; color:#1a73e8; font-size:18px; cursor:pointer;'>Попробовать снова</button></body></html>");
    }
}

void WebUIManager::handleLogout() {
    server.sendHeader("Set-Cookie", "SESSION=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
}

void WebUIManager::handleChangePass() {
    if (!checkAuth()) { server.sendHeader("Location", "/admin"); server.send(302, "text/plain", ""); return; }

    String oldP = server.arg("old_pass");
    String newP = server.arg("new_pass");
    String newP2 = server.arg("new_pass2");

    if (oldP != Config.admin_pass) {
        server.send(200, "text/html", "<html><body style='text-align:center;padding:50px;'><h2 style='color:#e74c3c;'>❌ Неверный старый пароль!</h2><button onclick=\"window.location.href='/admin'\">Назад</button></body></html>"); return;
    }
    if (newP != newP2) {
        server.send(200, "text/html", "<html><body style='text-align:center;padding:50px;'><h2 style='color:#e74c3c;'>❌ Новые пароли не совпадают!</h2><button onclick=\"window.location.href='/admin'\">Назад</button></body></html>"); return;
    }
    if (newP.length() < 4) {
        server.send(200, "text/html", "<html><body style='text-align:center;padding:50px;'><h2 style='color:#e74c3c;'>❌ Пароль слишком короткий!</h2><button onclick=\"window.location.href='/admin'\">Назад</button></body></html>"); return;
    }

    Config.saveAdminPass(newP);
    Logger.add("⚠️ Пароль администратора был изменен!", "warn");
    server.sendHeader("Set-Cookie", "SESSION=" + Config.admin_pass + "; Path=/; HttpOnly"); 
    server.send(200, "text/html", "<html><body style='text-align:center;padding:50px;'><h2 style='color:#2ecc71;'>✅ Пароль успешно изменен!</h2><button onclick=\"window.location.href='/admin'\">Вернуться</button></body></html>");
}

void WebUIManager::handleForm() {
    String idStr = server.arg("id");
    String t = server.arg("token");
    int id = idStr.toInt();

    if (id < 0 || id >= Forms.formCount) {
        server.send(404, "text/plain", "Форма не найдена"); return;
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    server.sendContent(getHtmlHeader());
    server.sendContent("<div id='top-bar' class='top-bar' onclick=\"window.location.href='/'\"><b>← Назад в меню</b></div><div class='form-card'>");

    if (Config.getTokenUsageCount(idStr + "_" + t) >= Forms.formMaxDaily[id]) {
        String err = "<div id='error-page' style='text-align:center; padding-top:20px;'>";
        err += "<div class='top-token'>Ваш токен: <b>" + t + "</b></div>";
        err += "<h2 style='color:#e74c3c; font-size:28px;'>❌ Доступ закрыт</h2><p>Лимит прохождений исчерпан.</p>";
        err += "<br><button class='btn btn-gray' onclick=\"window.location.href='/'\">В меню</button></div>";
        server.sendContent(err);
    } else {
        String fHead = "<div id='f" + String(id) + "' class='form-block'>";
        fHead += "<div class='top-token'>Ваш токен: <b>" + t + "</b></div>";
        fHead += "<h2>" + Forms.formTitles[id] + "</h2>";
        fHead += "<form onsubmit='sendData(event, \"f" + String(id) + "\")'>";
        fHead += "<input type='hidden' name='token' value='" + t + "'><input type='hidden' name='form_index' value='" + String(id) + "'>";
        server.sendContent(fHead);

        File file = LittleFS.open("/raw_" + String(id) + ".txt", FILE_READ);
        if (file) {
            file.seek(Forms.formHtmlOffsets[id]);
            char buf[1025];
            while(file.available()) {
                size_t len = file.read((uint8_t*)buf, 1024);
                buf[len] = '\0'; 
                server.sendContent(buf); 
            }
            file.close();
        } else {
            server.sendContent("<p style='color:red;'>Ошибка чтения файла формы.</p>");
        }

        String btnColor = "btn";
        if (id % 3 == 1) btnColor = "btn btn-green"; if (id % 3 == 2) btnColor = "btn btn-orange";
        String fTail = "<button type='submit' class='" + btnColor + "'>ОТПРАВИТЬ</button></form></div>";
        fTail += "<div id='load' style='display:none; text-align:center;'><h2>Отправка...</h2><div class='loader'></div></div>";
        fTail += "<div id='res' style='display:none; text-align:center;'><div id='res-msg'></div><button class='btn btn-gray' onclick=\"window.location.href='/'\">В меню</button></div>";
        server.sendContent(fTail);
    }
    
    server.sendContent("</div></body></html>");
    server.sendContent(""); 
}

void WebUIManager::handleSubmit() {
    String submittedToken = server.arg("token");
    String formIndex = server.arg("form_index");
    int idx = formIndex.toInt();

    if (idx < Forms.formCount && Config.getTokenUsageCount(formIndex + "_" + submittedToken) >= Forms.formMaxDaily[idx]) {
        server.send(200, "text/plain", "ERROR_TOKEN"); return;
    }

    String payload = "";
    for (int i = 0; i < server.args(); i++) {
        payload += Forms.urlEncode(server.argName(i)) + "=" + Forms.urlEncode(server.arg(i));
        if (i < server.args() - 1) payload += "&";
    }

    Logger.add("--- НАЧАЛО: Отправка ответов ---");
    
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http; 
    
    http.begin(client, Config.cloud_url); 
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS); 
    http.setTimeout(30000); 
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    
    const char * headerKeys[] = {"Location"}; 
    http.collectHeaders(headerKeys, 1);
    
    int postCode = http.POST(payload); 
    Logger.add("HTTP POST Код (Отправка): " + String(postCode));
    
    String response = "";
    if (postCode == 302 || postCode == 303) {
        String redirectUrl = http.header("Location"); 
        http.end(); 
        Logger.add("Сервер перенаправляет...");
        if (redirectUrl.length() > 0) {
            http.begin(client, redirectUrl); 
            http.setTimeout(30000);
            http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");

            int getCode = http.GET();
            Logger.add("HTTP GET Код (Редирект): " + String(getCode));
            if (getCode == 200) response = http.getString();
            else response = "SUCCESS";
        } else response = "SUCCESS";
    } else if (postCode == 200) {
        response = "SUCCESS"; 
    } else {
        response = "ERROR_CONNECTION";
        Logger.add("❌ ОШИБКА ОТПРАВКИ: " + http.errorToString(postCode), "error");
    }
    http.end(); 
    response.trim();

    if (idx < Forms.formCount && response == "SUCCESS") {
        Config.markTokenUsed(formIndex + "_" + submittedToken);
        Logger.add("✅ Ответы сохранены.");
    } else {
        Logger.add("❌ Итоговый статус: ПРОВАЛ.", "error");
    }
    server.send(200, "text/plain", response);
}

void WebUIManager::handleAdmin() {
    if (!checkAuth()) {
        String loginHtml = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><style>body{font-family:sans-serif; background:#f4f7f6; padding:20px; display:flex; justify-content:center; align-items:center; height:100vh; margin:0;} .box{background:white; padding:30px; border-radius:10px; box-shadow:0 4px 10px rgba(0,0,0,0.1); text-align:center; width:100%; max-width:320px;} input{width:100%; padding:12px; margin-bottom:15px; border:1px solid #ccc; border-radius:5px; box-sizing:border-box; font-size:16px;} button{width:100%; padding:15px; background:#1a73e8; color:white; border:none; border-radius:5px; font-weight:bold; font-size:16px; cursor:pointer;}</style></head><body><div class='box'><h2 style='color:#1a73e8; margin-top:0;'>Админ-панель</h2><form action='/login' method='POST'><input type='password' name='pwd' placeholder='Введите пароль' required><button type='submit'>ВОЙТИ</button></form><br><br><button onclick=\"window.location.href='/'\" style='background:none; border:none; color:#95a5a6; font-size:16px; cursor:pointer; padding:10px; font-weight:normal;'>⬅ В главное меню</button></div></body></html>";
        server.send(200, "text/html", loginHtml);
        return;
    }
    
    if (server.hasArg("clear_log")) {
        Logger.clear();
        server.sendHeader("Location", "/admin");
        server.send(302, "text/plain", ""); return;
    }

    if (server.hasArg("reset_all")) {
        Config.clearAllTokens();
        String content = "<html><body style='text-align:center;padding:30px;'><h2>✅ Все лимиты обнулены!</h2><button onclick=\"window.location.href='/admin'\">Назад в админку</button></body></html>";
        server.send(200, "text/html", content); return;
    }
    
    String content = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><style>body{font-family:sans-serif; padding:20px;} input, select{padding:12px; width:100%; box-sizing:border-box; margin-bottom:15px; border-radius:5px; border:1px solid #ccc;} .btn{padding:15px 20px; width:100%; color:white; border:none; border-radius:5px; font-weight:bold; cursor:pointer; margin-bottom:10px;} .btn-blue{background:#1a73e8;} .btn-orange{background:#f39c12;} .btn-red{background:#e74c3c;} .btn-green{background:#2ecc71;} .btn-gray{background:#7f8c8d;}</style></head><body>";
    
    content += "<button class='btn btn-blue' style='margin-bottom:20px;' onclick=\"window.location.href='/'\">⬅ В ГЛАВНОЕ МЕНЮ</button>";
    content += "<h2>Панель admin</h2>";
    
    String wifiStatus = ESPNetwork.isConnected() ? "<span style='color:#2ecc71;'>✅ Подключено (IP: " + ESPNetwork.getIP() + ")</span>" : "<span style='color:#e74c3c;'>❌ Отключено от сети</span>";
    content += "<div style='background:#f9f9f9; padding:20px; border-radius:10px; margin-bottom:20px;'><h3>Диагностика системы</h3>";
    content += "<p style='font-size:16px; margin-bottom:15px;'>Статус Wi-Fi: " + wifiStatus + "</p>";
    content += "<textarea id='syslog' readonly style='width:100%; height:180px; background:#2c3e50; color:#ecf0f1; font-family:monospace; font-size:13px; padding:10px; border-radius:5px; resize:vertical;'>" + Logger.getSystemLog() + "</textarea>";
    
    content += "<div style='display:flex; gap:10px; margin-top:10px;'>";
    content += "<form action='/admin' method='GET' style='margin:0; flex:1;'><input type='hidden' name='clear_log' value='1'><button type='submit' class='btn btn-gray' style='margin:0;'>ОЧИСТИТЬ ЛОГ</button></form>";
    content += "<button class='btn btn-blue' style='margin:0; flex:1;' onclick=\"var t=document.getElementById('syslog'); t.select(); document.execCommand('copy'); alert('Лог скопирован!');\">СКОПИРОВАТЬ ЛОГ</button>";
    content += "</div></div>";

    content += "<div style='background:#f9f9f9; padding:20px; border-radius:10px; margin-bottom:20px;'><h3>Управление системой</h3>";
    content += "<button class='btn btn-green' onclick=\"window.location.href='/update'\">ОБНОВИТЬ ОПРОСНИКИ</button>";
    content += "<button class='btn btn-gray' onclick=\"if(confirm('Точно перезагрузить плату?')) window.location.href='/reboot'\">ПЕРЕЗАГРУЗИТЬ ПЛАТУ</button>";
    content += "<form action='/admin' method='GET' style='margin-top:10px;'><input type='hidden' name='reset_all' value='1'><button type='submit' class='btn btn-red' onclick='return confirm(\"Обнулить лимиты вообще ВСЕХ?\");'>ОБНУЛИТЬ ВСЕ ЛИМИТЫ</button></form></div>";
    
    content += "<form action='/save_settings' method='POST'>";
    content += "<div style='background:#f9f9f9; padding:20px; border-radius:10px; margin-bottom:20px;'><h3>Настройки Wi-Fi</h3><label>Имя сети базы (AP):</label><input type='text' name='ap_name' value='" + Config.ap_name + "' required><label>Роутер SSID:</label><input type='text' name='router_ssid' value='" + Config.router_ssid + "' required><label>Пароль (пусто если нет):</label><input type='text' name='router_pass' value='" + Config.router_pass + "'></div>";

    content += "<div style='background:#f9f9f9; padding:20px; border-radius:10px; margin-bottom:20px;'><h3>Настройки Cloudflare</h3><label>API URL (Worker):</label><input type='text' name='cloud_url' value='" + Config.cloud_url + "' required><label>Device ID:</label><input type='text' name='dev_id' value='" + Config.device_id + "' required><label>Device Secret:</label><input type='password' name='dev_sec' value='" + Config.device_secret + "' required></div>";
    
    content += "<button type='submit' class='btn btn-blue'>СОХРАНИТЬ ВСЕ НАСТРОЙКИ И ПЕРЕЗАГРУЗИТЬ</button></form>";

    content += "<div style='background:#f9f9f9; padding:20px; border-radius:10px; margin-bottom:20px;'><h3>Смена пароля администратора</h3>";
    content += "<form action='/change_pass' method='POST'><input type='password' name='old_pass' placeholder='Старый пароль' required><input type='password' name='new_pass' placeholder='Новый пароль' required><input type='password' name='new_pass2' placeholder='Повторите новый пароль' required><button type='submit' class='btn btn-orange'>ИЗМЕНИТЬ ПАРОЛЬ</button></form></div>";

    content += "<button class='btn btn-gray' onclick=\"window.location.href='/logout'\">ВЫЙТИ ИЗ АДМИНКИ</button>";
    content += "</body></html>";
    server.send(200, "text/html", content);
}

void WebUIManager::handleSaveSettings() {
    if (!checkAuth()) { server.sendHeader("Location", "/admin"); server.send(302, "text/plain", ""); return; }
    Config.saveWifi(server.arg("router_ssid"), server.arg("router_pass"), server.arg("ap_name"));
    Config.saveCloud(server.arg("dev_id"), server.arg("dev_sec"), server.arg("cloud_url"));
    Logger.add("Настройки Wi-Fi и Cloudflare сохранены. Перезагрузка.");
    server.send(200, "text/html", "<html><body style='text-align:center; padding:50px;'><h2>Настройки сохранены! Плата перезагружается...</h2></body></html>");
    delay(2000); ESP.restart(); 
}

void WebUIManager::handleUpdate() { 
    if (!checkAuth()) { server.sendHeader("Location", "/admin"); server.send(302, "text/plain", ""); return; }
    Logger.add("Запущено ручное обновление опросников...");
    bool ok = Forms.fetchFromServer(); 
    if (ok) {
        server.send(200, "text/html", "<html><body style='text-align:center; padding:50px;'><h2 style='color:#2ecc71;'>✅ Опросники обновлены!</h2><button onclick=\"window.location.href='/admin'\">Вернуться в админку</button></body></html>"); 
    } else {
        server.send(200, "text/html", "<html><body style='text-align:center; padding:50px;'><h2 style='color:#e74c3c;'>❌ Опросники не загрузились</h2><p>Проверьте системный лог в админке.</p><button onclick=\"window.location.href='/admin'\">Вернуться в админку</button></body></html>"); 
    }
}

void WebUIManager::handleReboot() {
    if (!checkAuth()) { server.sendHeader("Location", "/admin"); server.send(302, "text/plain", ""); return; }
    Logger.add("Получена команда на ручную перезагрузку.", "warn");
    server.send(200, "text/html", "<html><body style='text-align:center; padding:50px;'><h2>Плата перезагружается...</h2></body></html>");
    delay(1000); ESP.restart();
}

void WebUIManager::handleNotFound() {
    // Captive portal: отдаём HTML-страницу с редиректом вместо голого 302.
    // Некоторые ОС (iOS, новые Android) игнорируют 302 при детекции портала,
    // но корректно обрабатывают HTML-ответ с meta-refresh.
    String html = "<html><head>";
    html += "<meta http-equiv='refresh' content='0; url=http://192.168.4.1/'>";
    html += "<title>Redirect</title></head>";
    html += "<body><a href='http://192.168.4.1/'>Click here</a></body></html>";
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/html", html);
}
