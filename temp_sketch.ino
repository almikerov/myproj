Created At: 2026-06-25T13:11:54Z
Completed At: 2026-06-25T13:11:54Z
File Path: `file:///c:/Users/sanya/Desktop/myproj/sketch_jun17a.ino`
Total Lines: 693
Total Bytes: 40864
Showing lines 1 to 693
The following code has been modified to include a line number before every line, in the format: <line_number>: <original_line>. Please note that any changes targeting the original code should remove the line number, colon, and leading space.
1: #include <WiFi.h>
2: #include <WiFiClientSecure.h> 
3: #include <DNSServer.h>
4: #include <WebServer.h>
5: #include <HTTPClient.h>
6: #include <Preferences.h>
7: #include <LittleFS.h>
8: #include <vector>
9: #include "time.h"
10: #include <esp_task_wdt.h> 
11: 
12: #define WDT_TIMEOUT 300 // секунд на зависание до авто-перезагрузки
13: 
14: const byte DNS_PORT = 53;
15: IPAddress apIP(192, 168, 4, 1);
16: DNSServer dnsServer;
17: WebServer webServer(80);
18: Preferences preferences;
19: 
20: // ================= АППАРАТНЫЕ ПИНЫ =================
21: const int BOOT_BUTTON_PIN = 0; 
22: const int LED_PIN = 2;         
23: // ===================================================
24: 
25: // ================= ВАШИ НАСТРОЙКИ =================
26: String router_ssid;
27: String router_pass;
28: String ap_name;
29: String admin_pass; 
30: const char* google_script_url = "https://dawn-rain-7850.al-mikerov.workers.dev/";
31: // ==================================================
32: 
33: const char* ntpServer = "pool.ntp.org";
34: int lastUpdateDay = -1; 
35: 
36: std::vector<String> formTitles;
37: std::vector<int> formMaxDaily;
38: std::vector<uint32_t> formHtmlOffsets;
39: int formCount = 0;
40: String usedTokensStr = "";
41: 
42: const int MAX_LOG_LINES = 50;
43: String logBuffer[MAX_LOG_LINES];
44: int logIndex = 0;
45: bool logFull = false;
46: 
47: unsigned long lastLedBlink = 0;
48: bool ledState = false;
49: int ledBlinkInterval = 500; 
50: unsigned long buttonPressTime = 0;
51: bool buttonPressed = false;
52: 
53: void addLog(String msg) {
54:   struct tm timeinfo;
55:   String timeStr = "";
56:   if (getLocalTime(&timeinfo, 10) && timeinfo.tm_year > 120) {
57:     char timeStringBuff[50];
58:     strftime(timeStringBuff, sizeof(timeStringBuff), "[%H:%M:%S] ", &timeinfo);
59:     timeStr = String(timeStringBuff);
60:   } else {
61:     timeStr = "[" + String(millis() / 1000) + " сек] ";
62:   }
63:   
64:   String fullMsg = timeStr + msg;
65:   Serial.println(fullMsg);
66:   
67:   logBuffer[logIndex] = fullMsg;
68:   logIndex++;
69:   if (logIndex >= MAX_LOG_LINES) {
70:     logIndex = 0;
71:     logFull = true;
72:   }
73: }
74: 
75: String getSystemLog() {
76:   String res = "";
77:   if (logFull) {
78:     for (int i = logIndex - 1; i >= 0; i--) res += logBuffer[i] + "\n";
79:     for (int i = MAX_LOG_LINES - 1; i >= logIndex; i--) res += logBuffer[i] + "\n";
80:   } else {
81:     for (int i = logIndex - 1; i >= 0; i--) res += logBuffer[i] + "\n";
82:   }
83:   return res;
84: }
85: 
86: void loadTokens() { usedTokensStr = preferences.getString("tokens", ""); }
87: void saveTokens() { preferences.putString("tokens", usedTokensStr); }
88: 
89: int getTokenUsageCount(String entry) {
90:   String searchStr = "," + entry + ",";
91:   int count = 0; int pos = usedTokensStr.indexOf(searchStr);
92:   while (pos != -1) { count++; pos = usedTokensStr.indexOf(searchStr, pos + 1); }
93:   return count;
94: }
95: 
96: void markTokenUsed(String entry) {
97:   if (usedTokensStr.length() == 0) usedTokensStr = ",";
98:   usedTokensStr += entry + ","; saveTokens();
99: }
100: 
101: void freeToken(String t) {
102:   for(int i = 0; i < 10; i++) {
103:     String searchStr = "," + String(i) + "_" + t + ",";
104:     while(usedTokensStr.indexOf(searchStr) != -1) usedTokensStr.replace(searchStr, ",");
105:   }
106:   saveTokens();
107: }
108: 
109: void clearAllTokens() { usedTokensStr = ""; saveTokens(); addLog("Все лимиты токенов были обнулены."); }
110: 
111: String urlEncode(String str) {
112:   String encodedString = ""; char c, code0, code1;
113:   for (int i = 0; i < str.length(); i++) {
114:     c = str.charAt(i);
115:     if (c == ' ') encodedString += '+'; else if (isalnum(c)) encodedString += c;
116:     else {
117:       code1 = (c & 0xf) + '0'; if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
118:       c = (c >> 4) & 0xf; code0 = c + '0'; if (c > 9) code0 = c - 10 + 'A';
119:       encodedString += '%'; encodedString += code0; encodedString += code1;
120:     }
121:   }
122:   return encodedString;
123: }
124: 
125: String getLocalToken(String formNumber) {
126:   IPAddress ip = webServer.client().remoteIP();
127:   int hash = (ip[3] * 137) ^ (formNumber.toInt() * 89);
128:   hash = hash + 4096; char hexBuf[10]; sprintf(hexBuf, "%X", hash);
129:   return "TK-" + String(hexBuf); 
130: }
131: 
132: bool checkAuth() {
133:   if (webServer.hasHeader("Cookie")) {
134:     String cookie = webServer.header("Cookie");
135:     if (cookie.indexOf("SESSION=" + admin_pass) != -1) return true;
136:   }
137:   return false;
138: }
139: 
140: void fetchFormFromServer() {
141:   addLog("--- НАЧАЛО: Запрос списка опросников ---");
142: 
143:   // === ПРОВЕРКА DNS: Узнаем IP-адрес Гугла вручную ===
144:   IPAddress googleIP;
145:   if (WiFi.hostByName("script.google.com", googleIP)) {
146:     addLog("DNS Гугла определен: " + googleIP.toString());
147:   } else {
148:     addLog("❌ ОШИБКА DNS: Роутер скрывает IP-адрес Гугла!");
149:   }
150:   // =================================================
151: 
152:   // === ФИКС ОТ КЛОДА: Создаем свежий локальный клиент ===
153:   WiFiClientSecure client;
154:   client.setInsecure();
155:   HTTPClient http;
156:   
157:   http.begin(client, String(google_script_url) + "?action=getFormCount");
158:   http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); 
159:   http.setTimeout(30000);
160:   
161:   // === МАСКИРОВКА ПОД ОБЫЧНЫЙ КОМПЬЮТЕР ===
162:   http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
163:   
164:   int countCode = http.GET();
165:   addLog("HTTP GET Код ответа (Список): " + String(countCode));
166: 
167:   if (countCode == 200) {
168:     formCount = http.getString().toInt();
169:     addLog("✅ Успех. Найдено опросов: " + String(formCount));
170:   } else if (countCode < 0) {
171:     addLog("❌ ОШИБКА СЕТИ (DPI/Firewall): " + http.errorToString(countCode));
172:   } else {
173:     addLog("❌ ОШИБКА СЕРВЕРА Google: " + http.getString());
174:   }
175:   http.end();
176:   
177:   if (formCount <= 0) return;
178: 
179:   formTitles.assign(formCount, "Загрузка опроса...");
180:   formMaxDaily.assign(formCount, 1);
181:   formHtmlOffsets.assign(formCount, 0);
182: 
183:   for (int i = 0; i < formCount; i++) {
184:     esp_task_wdt_reset(); 
185:     addLog("Скачивание опроса #" + String(i + 1) + "...");
186:     
187:     http.begin(client, String(google_script_url) + "?action=getForm&index=" + String(i));
188:     http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); 
189:     http.setTimeout(45000); 
190:     
191:     // Снова маскируемся для скачивания самого файла
192:     http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
193: 
194:     int formCode = http.GET();
195:     addLog("HTTP GET Код (Форма #" + String(i + 1) + "): " + String(formCode));
196: 
197:     if (formCode == 200) {
198:       String fileName = "/raw_" + String(i) + ".txt";
199:       File file = LittleFS.open(fileName, FILE_WRITE);
200:       if (file) {
201:         http.writeToStream(&file);
202:         file.close();
203:         
204:         file = LittleFS.open(fileName, FILE_READ);
205:         if (file) {
206:           String title = file.readStringUntil('|'); file.read(); file.read();
207:           String maxDailyStr = file.readStringUntil('|'); file.read(); file.read();
208:           title.trim(); maxDailyStr.trim();
209:           if (title.length() == 0) title = "Опросник " + String(i + 1);
210:           title.replace("<", "&lt;"); title.replace(">", "&gt;");
211:           formTitles[i] = title;
212:           formMaxDaily[i] = maxDailyStr.toInt();
213:           formHtmlOffsets[i] = file.position(); 
214:           file.close();
215:           addLog("✅ Опрос [" + title + "] загружен!");
216:         }
217:       } else {
218:         addLog("❌ Ошибка записи на диск LittleFS.");
219:       }
220:     } else {
221:       addLog("❌ Провал скачивания. Детали: " + http.errorToString(formCode));
222:     }
223:     http.end();
224:   }
225:   addLog("--- КОНЕЦ: Обновление опросников ---");
226: }
227: 
228: String getHtmlHeader() {
229:   String html = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
230:   html += "<style>body{font-family:sans-serif; background:#f4f7f6; padding:20px; padding-top:80px; color:#333; margin:0;} "; 
231:   html += ".top-bar {position:fixed; top:0; left:0; width:100%; background:#1a73e8; color:white; text-align:left; padding:20px 25px; box-sizing:border-box; font-weight:bold; font-size:18px; box-shadow:0 4px 6px rgba(0,0,0,0.1); cursor:pointer; z-index:1000;} ";
232:   html += ".form-card{background:white; max-width:450px; margin:0 auto; padding:30px; border-radius:16px; box-shadow:0 8px 24px rgba(0,0,0,0.05);} ";
233:   html += ".top-token{text-align:center; background:#f8f9fa; color:#95a5a6; font-size:11px; padding:6px; border-radius:8px; margin-bottom:20px; font-family:monospace; border:1px dashed #ddd; letter-spacing:1px;} ";
234:   
235:   html += "h2{text-align:center; margin-bottom:20px; color:#1a73e8;} .form-label{font-weight:normal; display:block; margin-top:15px; margin-bottom:5px;} ";
236:   html += ".req-label::after { content: ' *'; color: #ff8c00; font-size: 16px; margin-left: 4px;} ";
237:   
238:   html += "input[type='text'], input[type='password'], select, textarea{width:100%; padding:12px; border:1px solid #ddd; border-radius:8px; box-sizing:border-box; font-size:16px; font-family:inherit;} ";
239:   
240:   html += ".radio-group, .scale-wrapper {background:#fafafa; padding:10px; border-radius:8px; border:2px solid #ddd; transition: 0.3s;} ";
241:   html += ".radio-label{display:flex; align-items:center; font-weight:normal; margin-top:5px; cursor:pointer;} .radio-label input{width:auto; margin-right:10px;} ";
242:   
243:   html += ".scale-wrapper { display: flex; justify-content: space-between; gap: 4px; margin-top: 10px; flex-wrap: nowrap; box-sizing: border-box; overflow-x: auto; padding-bottom: 5px; } ";
244:   html += ".scale-btn { flex: 1 1 0; min-width: 0; text-align: center; cursor: pointer; } ";
245:   html += ".scale-btn input { position: absolute; opacity: 0; width: 0; height: 0; } ";
246:   html += ".scale-btn span { display: block; padding: 12px 0; background: #e8eaed; border-radius: 8px; font-weight: bold; color: #5f6368; transition: 0.2s; box-sizing: border-box; border: 2px solid transparent; font-size: 14px;} ";
247:   html += ".scale-btn input:checked + span { background: #1a73e8; color: white; border-color: #1a73e8; transform: scale(1.05); box-shadow: 0 4px 10px rgba(26,115,232,0.3); } ";
248:   
249:   html += ".btn{background:#1a73e8; color:white; border:none; width:100%; padding:15px; border-radius:8px; font-weight:bold; font-size:16px; margin-top:20px; cursor:pointer; display:block; text-align:center; text-decoration:none; box-sizing:border-box;} ";
250:   html += ".btn-green{background:#2ecc71;} .btn-orange{background:#f39c12;} .btn-red{background:#e74c3c;} .btn-gray{background:#7f8c8d;} ";
251:   html += ".loader{border:5px solid #f3f3f3; border-top:5px solid #1a73e8; border-radius:50%; width:50px; height:50px; animation:spin 1s linear infinite; margin:20px auto;} @keyframes spin{0%{transform:rotate(0deg);} 100%{transform:rotate(360deg);}}</style>";
252:   
253:   html += "<script>";
254:   html += "function sendData(e, f) { e.preventDefault(); var formEl = e.target; ";
255:   html += "var wrappers = formEl.querySelectorAll('.scale-wrapper, .radio-group'); wrappers.forEach(w => w.style.borderColor = '#ddd'); ";
256:   html += "var requiredRadios = {}; ";
257:   html += "formEl.querySelectorAll('input[required][type=\"radio\"]').forEach(r => { requiredRadios[r.name] = false; }); ";
258:   html += "formEl.querySelectorAll('input[type=\"radio\"]:checked').forEach(r => { if (requiredRadios[r.name] !== undefined) requiredRadios[r.name] = true; }); ";
259:   html += "var radioValid = true; var firstErrorEl = null; ";
260:   html += "for (var name in requiredRadios) { if (!requiredRadios[name]) { radioValid = false; ";
261:   html += "  var wrapper = formEl.querySelector('input[name=\"' + name + '\"]').closest('.scale-wrapper, .radio-group'); ";
262:   html += "  if (wrapper) { wrapper.style.borderColor = '#e74c3c'; if(!firstErrorEl) firstErrorEl = wrapper; } ";
263:   html += "} } ";
264:   html += "if (!formEl.checkValidity() || !radioValid) { ";
265:   html += "  if(firstErrorEl) firstErrorEl.scrollIntoView({ behavior: 'smooth', block: 'center' }); ";
266:   html += "  alert('Пожалуйста, заполните все обязательные вопросы!'); return; ";
267:   html += "} ";
268:   html += "document.getElementById(f).style.display='none'; document.getElementById('load').style.display='block'; "; 
269:   html += "var fd = new FormData(formEl); var d = new URLSearchParams(fd).toString(); ";
270:   html += "fetch('/submit', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:d}) ";
271:   html += ".then(r => r.text()).then(t => { document.getElementById('load').style.display='none'; document.getElementById('res').style.display='block'; ";
272:   html += "if(t.trim()=='SUCCESS') document.getElementById('res-msg').innerHTML='<h2 style=\"color:#2ecc71;\">✅ Успешно!</h2><p>Ваши данные сохранены.</p>'; ";
273:   html += "else { ";
274:   html += "  document.getElementById('res-msg').innerHTML='<h2 style=\"color:#e74c3c;\">❌ Доступ закрыт</h2><p>Лимит прохождений этого опроса на сегодня исчерпан.</p>'; ";
275:   html += "} }) ";
276:   html += ".catch(err => { document.getElementById('load').style.display='none'; document.getElementById('res').style.display='block'; document.getElementById('res-msg').innerHTML='<h2 style=\"color:#e74c3c;\">❌ Обрыв связи</h2><p>Плата потеряла сеть.</p>'; }); } ";
277:   html += "</script></head><body>";
278:   return html;
279: }
280: 
281: void setup() {
282:   Serial.begin(115200);
283:   
284:   pinMode(LED_PIN, OUTPUT);
285:   pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
286:   digitalWrite(LED_PIN, LOW);
287: 
288:   esp_task_wdt_deinit();
289:   esp_task_wdt_config_t wdt_config = {
290:     .timeout_ms = WDT_TIMEOUT * 1000,
291:     .idle_core_mask = 0,
292:     .trigger_panic = true
293:   };
294:   esp_task_wdt_init(&wdt_config);
295:   esp_task_wdt_add(NULL); 
296: 
297:   addLog("Запуск системы...");
298: 
299:   if(!LittleFS.begin(true)){
300:     addLog("❌ Ошибка монтирования LittleFS!");
301:   }
302: 
303:   preferences.begin("config", false);
304:   router_ssid = preferences.getString("ssid", "ASUS");
305:   router_pass = preferences.getString("pass", "19642005");
306:   ap_name = preferences.getString("ap", "Base_Opros_V25");
307:   admin_pass = preferences.getString("admin_pass", "19642005"); 
308:   
309:   loadTokens();
310:   lastUpdateDay = preferences.getInt("last_day", -1);
311: 
312:   const char * headerKeys[] = {"Cookie"};
313:   webServer.collectHeaders(headerKeys, 1);
314: 
315:   addLog("Попытка подключения к Wi-Fi роутеру: [" + router_ssid + "]...");
316:   ledBlinkInterval = 500; 
317:   
318:   WiFi.begin(router_ssid.c_str(), router_pass.c_str());
319:   int tries = 0; 
320:   while (WiFi.status() != WL_CONNECTED && tries < 20) { 
321:     delay(500); 
322:     tries++; 
323:     ledState = !ledState; digitalWrite(LED_PIN, ledState); 
324:     esp_task_wdt_reset(); 
325:   }
326:   
327:   if(WiFi.status() == WL_CONNECTED) {
328:     addLog("✅ Wi-Fi подключен! Внутренний IP: " + WiFi.localIP().toString());
329:     
330:     ledBlinkInterval = 0; 
331:     digitalWrite(LED_PIN, HIGH); 
332:     
333:     configTime(10800, 0, ntpServer); 
334:     struct tm timeinfo;
335:     if (getLocalTime(&timeinfo, 5000) && timeinfo.tm_year > 120) { 
336:       addLog("✅ Время синхронизировано успешно.");
337:       if (lastUpdateDay != -1 && timeinfo.tm_mday != lastUpdateDay) {
338:         addLog("Обнаружены новые сутки! Сброс лимитов.");
339:         clearAllTokens(); 
340:       }
341:       lastUpdateDay = timeinfo.tm_mday;
342:       preferences.putInt("last_day", lastUpdateDay);
343:     } else {
344:       addLog("❌ Ошибка синхронизации времени по NTP.");
345:     }
346:     fetchFormFromServer(); 
347:   } else {
348:     addLog("❌ Не удалось подключиться к роутеру.");
349:     ledBlinkInterval = 150; 
350:   }
351: 
352:   WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
353:   WiFi.softAP(ap_name.c_str()); dnsServer.start(DNS_PORT, "*", apIP);
354:   addLog("Точка доступа платы [" + ap_name + "] активирована.");
355: 
356:   webServer.on("/", []() {
357:     String content = getHtmlHeader();
358:     content += "<div class='form-card'><div id='menu'><h2>Главное меню</h2>";
359:     for (int i = 0; i < formCount; i++) {
360:       String btnColor = "btn";
361:       if (i % 3 == 1) btnColor = "btn btn-green";
362:       if (i % 3 == 2) btnColor = "btn btn-orange";
363:       content += "<button class='" + btnColor + "' onclick=\"window.location.href='/form?id=" + String(i) + "&token=" + getLocalToken(String(i)) + "'\">" + formTitles[i] + "</button>";
364:     }
365:     content += "</div></div>";
366:     
367:     content += "<div style='text-align:center; margin-top:30px; margin-bottom:20px; padding:20px;'><span onclick=\"let n=Date.now(); if(n-(this.l||0)<600) window.location.href='/admin'; this.l=n;\" style='color:#bdc3c7; font-size:12px; letter-spacing:1px; cursor:pointer;'>АДМИН-ПАНЕЛЬ</span></div>";
368:     content += "</body></html>";
369:     webServer.send(200, "text/html", content);
370:   });
371: 
372:   webServer.on("/login", HTTP_POST, []() {
373:     String p = webServer.arg("pwd");
374:     if (p == admin_pass) {
375:       addLog("Успешный вход в админ-панель.");
376:       webServer.sendHeader("Set-Cookie", "SESSION=" + admin_pass + "; Path=/; HttpOnly");
377:       webServer.sendHeader("Location", "/admin");
378:       webServer.send(302, "text/plain", "");
379:     } else {
380:       addLog("⚠️ Неудачная попытка входа в админ-панель (Неверный пароль)!");
381:       webServer.send(200, "text/html", "<html><head><meta charset='utf-8'></head><body style='text-align:center;padding:50px; font-family:sans-serif;'><h2 style='color:#e74c3c;'>❌ Неверный пароль!</h2><br><button onclick=\"window.location.href='/admin'\" style='background:none; border:none; color:#1a73e8; font-size:18px; cursor:pointer;'>Попробовать снова</button></body></html>");
382:     }
383:   });
384: 
385:   webServer.on("/logout", []() {
386:     webServer.sendHeader("Set-Cookie", "SESSION=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
387:     webServer.sendHeader("Location", "/");
388:     webServer.send(302, "text/plain", "");
389:   });
390: 
391:   webServer.on("/change_pass", HTTP_POST, []() {
392:     if (!checkAuth()) { webServer.sendHeader("Location", "/admin"); webServer.send(302, "text/plain", ""); return; }
393: 
394:     String oldP = webServer.arg("old_pass");
395:     String newP = webServer.arg("new_pass");
396:     String newP2 = webServer.arg("new_pass2");
397: 
398:     if (oldP != admin_pass) {
399:       webServer.send(200, "text/html", "<html><head><meta charset='utf-8'></head><body style='text-align:center;padding:50px; font-family:sans-serif;'><h2 style='color:#e74c3c;'>❌ Неверный старый пароль!</h2><br><button onclick=\"window.location.href='/admin'\" style='background:none; border:none; color:#1a73e8; font-size:18px; cursor:pointer;'>Назад</button></body></html>"); return;
400:     }
401:     if (newP != newP2) {
402:       webServer.send(200, "text/html", "<html><head><meta charset='utf-8'></head><body style='text-align:center;padding:50px; font-family:sans-serif;'><h2 style='color:#e74c3c;'>❌ Новые пароли не совпадают!</h2><br><button onclick=\"window.location.href='/admin'\" style='background:none; border:none; color:#1a73e8; font-size:18px; cursor:pointer;'>Назад</button></body></html>"); return;
403:     }
404:     if (newP.length() < 4) {
405:       webServer.send(200, "text/html", "<html><head><meta charset='utf-8'></head><body style='text-align:center;padding:50px; font-family:sans-serif;'><h2 style='color:#e74c3c;'>❌ Пароль слишком короткий!</h2><br><button onclick=\"window.location.href='/admin'\" style='background:none; border:none; color:#1a73e8; font-size:18px; cursor:pointer;'>Назад</button></body></html>"); return;
406:     }
407: 
408:     admin_pass = newP;
409:     preferences.putString("admin_pass", admin_pass);
410:     addLog("⚠️ Пароль администратора был изменен!");
411:     webServer.sendHeader("Set-Cookie", "SESSION=" + admin_pass + "; Path=/; HttpOnly"); 
412:     webServer.send(200, "text/html", "<html><head><meta charset='utf-8'></head><body style='text-align:center;padding:50px; font-family:sans-serif;'><h2 style='color:#2ecc71;'>✅ Пароль успешно изменен!</h2><br><button onclick=\"window.location.href='/admin'\" style='background:none; border:none; color:#1a73e8; font-size:18px; cursor:pointer; font-weight:bold;'>Вернуться в админку</button></body></html>");
413:   });
414: 
415:   webServer.on("/form", []() {
416:     String idStr = webServer.arg("id");
417:     String t = webServer.arg("token");
418:     int id = idStr.toInt();
419: 
420:     if (id < 0 || id >= formCount) {
421:       webServer.send(404, "text/plain", "Форма не найдена"); return;
422:     }
423: 
424:     webServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
425:     webServer.send(200, "text/html", "");
426:     
427:     webServer.sendContent(getHtmlHeader());
428:     webServer.sendContent("<div id='top-bar' class='top-bar' onclick=\"window.location.href='/'\"><b>← Назад в меню</b></div><div class='form-card'>");
429: 
430:     if (getTokenUsageCount(idStr + "_" + t) >= formMaxDaily[id]) {
431:       String err = "<div id='error-page' style='text-align:center; padding-top:20px;'>";
432:       err += "<div class='top-token'>Ваш токен: <b>" + t + "</b></div>";
433:       err += "<h2 style='color:#e74c3c; font-size:28px;'>❌ Доступ закрыт</h2><p style='color:#555; font-size:18px;'>Лимит прохождений этого опроса на сегодня исчерпан.</p>";
434:       err += "<p style='color:gray; font-size:14px; margin-top:30px;'>Попросите администратора дать вам доступ.</p>";
435:       err += "<br><button class='btn btn-gray' onclick=\"window.location.href='/'\">В меню</button></div>";
436:       webServer.sendContent(err);
437:     } else {
438:       String fHead = "<div id='f" + String(id) + "' class='form-block'>";
439:       fHead += "<div class='top-token'>Ваш токен: <b>" + t + "</b></div>";
440:       fHead += "<h2>" + formTitles[id] + "</h2>";
441:       fHead += "<form onsubmit='sendData(event, \"f" + String(id) + "\")'>";
442:       fHead += "<input type='hidden' name='token' value='" + t + "'><input type='hidden' name='form_index' value='" + String(id) + "'>";
443:       webServer.sendContent(fHead);
444: 
445:       File file = LittleFS.open("/raw_" + String(id) + ".txt", FILE_READ);
446:       if (file) {
447:         file.seek(formHtmlOffsets[id]);
448:         char buf[1025];
449:         while(file.available()) {
450:           size_t len = file.read((uint8_t*)buf, 1024);
451:           buf[len] = '\0'; 
452:           webServer.sendContent(String(buf)); 
453:         }
454:         file.close();
455:       } else {
456:         webServer.sendContent("<p style='color:red;'>Ошибка чтения файла формы с диска платы.</p>");
457:       }
458: 
459:       String btnColor = "btn";
460:       if (id % 3 == 1) btnColor = "btn btn-green"; if (id % 3 == 2) btnColor = "btn btn-orange";
461:       String fTail = "<button type='submit' class='" + btnColor + "'>ОТПРАВИТЬ</button></form></div>";
462:       fTail += "<div id='load' style='display:none; text-align:center;'><h2>Отправка...</h2><div class='loader'></div></div>";
463:       fTail += "<div id='res' style='display:none; text-align:center;'><div id='res-msg'></div><button class='btn btn-gray' onclick=\"window.location.href='/'\">В меню</button></div>";
464:       webServer.sendContent(fTail);
465:     }
466:     
467:     webServer.sendContent("</div></body></html>");
468:     webServer.sendContent(""); 
469:   });
470: 
471:   webServer.on("/submit", HTTP_POST, []() {
472:     String submittedToken = webServer.arg("token");
473:     String formIndex = webServer.arg("form_index");
474:     int idx = formIndex.toInt();
475: 
476:     if (idx < formCount && getTokenUsageCount(formIndex + "_" + submittedToken) >= formMaxDaily[idx]) {
477:       webServer.send(200, "text/plain", "ERROR_TOKEN"); return;
478:     }
479: 
480:     String payload = "";
481:     for (int i = 0; i < webServer.args(); i++) {
482:       payload += urlEncode(webServer.argName(i)) + "=" + urlEncode(webServer.arg(i));
483:       if (i < webServer.args() - 1) payload += "&";
484:     }
485: 
486:     addLog("--- НАЧАЛО: Отправка ответов ---");
487:     
488:     // === ФИКС ОТ КЛОДА: Создаем свежий локальный клиент ===
489:     WiFiClientSecure client;
490:     client.setInsecure();
491:     HTTPClient http; 
492:     
493:     http.begin(client, google_script_url); 
494:     http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS); http.setTimeout(30000); 
495:     http.addHeader("Content-Type", "application/x-www-form-urlencoded");
496:     http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
497:     
498:     const char * headerKeys[] = {"Location"}; http.collectHeaders(headerKeys, 1);
499:     
500:     int postCode = http.POST(payload); 
501:     addLog("HTTP POST Код (Отправка): " + String(postCode));
502:     
503:     String response = "";
504:     if (postCode == 302 || postCode == 303) {
505:       String redirectUrl = http.header("Location"); http.end(); 
506:       addLog("Сервер перенаправляет (Redirect)...");
507:       if (redirectUrl.length() > 0) {
508:         
509:         http.begin(client, redirectUrl); 
510:         http.setTimeout(30000);
511:         http.addHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36");
512: 
513:         int getCode = http.GET();
514:         addLog("HTTP GET Код (Редирект): " + String(getCode));
515:         if (getCode == 200) response = http.getString();
516:         else response = "SUCCESS";
517:       } else response = "SUCCESS";
518:     } else if (postCode == 200) {
519:         response = "SUCCESS"; 
520:     } else {
521:         response = "ERROR_CONNECTION";
522:         addLog("❌ ОШИБКА ОТПРАВКИ: " + http.errorToString(postCode));
523:     }
524:     http.end(); response.trim();
525: 
526:     if (idx < formCount && response == "SUCCESS") {
527:       markTokenUsed(formIndex + "_" + submittedToken);
528:       addLog("✅ Ответы успешно сохранены на сервере.");
529:     } else {
530:       addLog("❌ Итоговый статус: ПРОВАЛ.");
531:     }
532:     webServer.send(200, "text/plain", response);
533:   });
534: 
535:   webServer.on("/admin", []() {
536:     if (!checkAuth()) {
537:       String loginHtml = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><style>body{font-family:sans-serif; background:#f4f7f6; padding:20px; display:flex; justify-content:center; align-items:center; height:100vh; margin:0;} .box{background:white; padding:30px; border-radius:10px; box-shadow:0 4px 10px rgba(0,0,0,0.1); text-align:center; width:100%; max-width:320px;} input{width:100%; padding:12px; margin-bottom:15px; border:1px solid #ccc; border-radius:5px; box-sizing:border-box; font-size:16px;} button{width:100%; padding:15px; background:#1a73e8; color:white; border:none; border-radius:5px; font-weight:bold; font-size:16px; cursor:pointer;}</style></head><body><div class='box'><h2 style='color:#1a73e8; margin-top:0;'>Админ-панель</h2><form action='/login' method='POST'><input type='password' name='pwd' placeholder='Введите пароль' required><button type='submit'>ВОЙТИ</button></form><br><br><button onclick=\"window.location.href='/'\" style='background:none; border:none; color:#95a5a6; font-size:16px; cursor:pointer; padding:10px; font-weight:normal;'>⬅ В главное меню</button></div></body></html>";
538:       webServer.send(200, "text/html", loginHtml);
539:       return;
540:     }
541:     
542:     if (webServer.hasArg("clear_log")) {
543:       logIndex = 0;
544:       logFull = false;
545:       addLog("Системный лог очищен пользователем.");
546:       webServer.sendHeader("Location", "/admin");
547:       webServer.send(302, "text/plain", ""); return;
548:     }
549: 
550:     if (webServer.hasArg("reset_my")) {
551:       String fId = webServer.arg("my_form_id");
552:       if (fId == "all") {
553:         for(int i = 0; i < formCount; i++) {
554:           String searchStr = "," + String(i) + "_" + getLocalToken(String(i)) + ",";
555:           while(usedTokensStr.indexOf(searchStr) != -1) usedTokensStr.replace(searchStr, ",");
556:         }
557:         addLog("Сброшены локальные лимиты пользователя (Все опросы).");
558:       } else {
559:         int i = fId.toInt();
560:         if (i >= 0 && i < formCount) {
561:           String searchStr = "," + String(i) + "_" + getLocalToken(String(i)) + ",";
562:           while(usedTokensStr.indexOf(searchStr) != -1) usedTokensStr.replace(searchStr, ",");
563:           addLog("Сброшен локальный лимит (Опрос #" + String(i+1) + ").");
564:         }
565:       }
566:       saveTokens();
567:       String content = "<html><head><meta charset='utf-8'></head><body style='text-align:center;padding:30px; font-family:sans-serif;'><h2>✅ Ваши лимиты успешно сброшены!</h2><br><button onclick=\"window.location.href='/admin'\" style='background:none; border:none; color:#1a73e8; font-size:18px; cursor:pointer;'>Назад в админку</button><br><br><button onclick=\"window.location.href='/'\" style='background:none; border:none; color:#1a73e8; font-size:18px; cursor:pointer; font-weight:bold;'>В главное меню</button></body></html>";
568:       webServer.send(200, "text/html", content); return;
569:     }
570: 
571:     if (webServer.hasArg("reset_token")) {
572:       String t = webServer.arg("reset_token"); t.trim(); freeToken(t); 
573:       addLog("Сброшены лимиты для токена: " + t);
574:       String content = "<html><head><meta charset='utf-8'></head><body style='text-align:center;padding:30px; font-family:sans-serif;'><h2>✅ Все лимиты для токена " + t + " сброшены!</h2><br><button onclick=\"window.location.href='/admin'\" style='background:none; border:none; color:#1a73e8; font-size:18px; cursor:pointer;'>Назад в админку</button><br><br><button onclick=\"window.location.href='/'\" style='background:none; border:none; color:#1a73e8; font-size:18px; cursor:pointer; font-weight:bold;'>В главное меню</button></body></html>";
575:       webServer.send(200, "text/html", content); return;
576:     }
577:     
578:     if (webServer.hasArg("reset_all")) {
579:       clearAllTokens();
580:       String content = "<html><head><meta charset='utf-8'></head><body style='text-align:center;padding:30px; font-family:sans-serif;'><h2>✅ Все лимиты всех пользователей обнулены!</h2><br><button onclick=\"window.location.href='/admin'\" style='background:none; border:none; color:#1a73e8; font-size:18px; cursor:pointer;'>Назад в админку</button><br><br><button onclick=\"window.location.href='/'\" style='background:none; border:none; color:#1a73e8; font-size:18px; cursor:pointer; font-weight:bold;'>В главное меню</button></body></html>";
581:       webServer.send(200, "text/html", content); return;
582:     }
583:     
584:     String content = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><style>body{font-family:sans-serif; padding:20px;} input, select{padding:12px; width:100%; box-sizing:border-box; margin-bottom:15px; border-radius:5px; border:1px solid #ccc;} .btn{padding:15px 20px; width:100%; color:white; border:none; border-radius:5px; font-weight:bold; cursor:pointer; margin-bottom:10px;} .btn-blue{background:#1a73e8;} .btn-orange{background:#f39c12;} .btn-red{background:#e74c3c;} .btn-green{background:#2ecc71;} .btn-gray{background:#7f8c8d;}</style>";
585:     
586:     content += "<script>function copyLog(){var t=document.getElementById('syslog');t.select();document.execCommand('copy');var b=document.getElementById('copyBtn');b.innerText='✅ СКОПИРОВАНО!';setTimeout(function(){b.innerText='КОПИРОВАТЬ ЛОГ';},2000);}</script>";
587:     content += "</head><body>";
588:     
589:     content += "<button class='btn btn-blue' style='margin-bottom:20px; box-shadow: 0 4px 6px rgba(0,0,0,0.1);' onclick=\"window.location.href='/'\">⬅ В ГЛАВНОЕ МЕНЮ</button>";
590:     content += "<h2>Панель admin</h2>";
591:     
592:     String wifiStatus = (WiFi.status() == WL_CONNECTED) ? "<span style='color:#2ecc71;'>✅ Подключено к <b>" + router_ssid + "</b> (IP: " + WiFi.localIP().toString() + ")</span>" : "<span style='color:#e74c3c;'>❌ Отключено от сети</span>";
593:     content += "<div style='background:#f9f9f9; padding:20px; border-radius:10px; margin-bottom:20px;'><h3>Диагностика системы</h3>";
594:     content += "<p style='font-size:16px; margin-bottom:15px;'>Статус Wi-Fi: " + wifiStatus + "</p>";
595:     content += "<textarea id='syslog' readonly style='width:100%; height:180px; background:#2c3e50; color:#ecf0f1; font-family:monospace; font-size:13px; padding:10px; border-radius:5px; resize:vertical;'>" + getSystemLog() + "</textarea>";
596:     
597:     content += "<button type='button' id='copyBtn' class='btn btn-blue' style='margin-top:15px; margin-bottom:5px;' onclick='copyLog()'>КОПИРОВАТЬ ЛОГ</button>";
598:     content += "<form action='/admin' method='GET' style='margin:0;'><input type='hidden' name='clear_log' value='1'><button type='submit' class='btn btn-gray'>ОЧИСТИТЬ ЛОГ</button></form></div>";
599:     
600:     content += "<div style='background:#f9f9f9; padding:20px; border-radius:10px; margin-bottom:20px;'><h3>Сбросить мои лимиты (на этом устройстве)</h3>";
601:     content += "<form action='/admin' method='GET'><input type='hidden' name='reset_my' value='1'>";
602:     content += "<select name='my_form_id' required>";
603:     content += "<option value='all'>-- Все опросники --</option>";
604:     for (int i = 0; i < formCount; i++) {
605:       content += "<option value='" + String(i) + "'>" + formTitles[i] + "</option>";
606:     }
607:     content += "</select><button type='submit' class='btn btn-orange'>СБРОСИТЬ СВОИ ЛИМИТЫ</button></form></div>";
608: 
609:     content += "<div style='background:#f9f9f9; padding:20px; border-radius:10px; margin-bottom:20px;'><h3>Сбросить лимиты спортсмена</h3>";
610:     content += "<form action='/admin' method='GET'><input type='text' name='reset_token' placeholder='Введите токен...' required><button type='submit' class='btn btn-orange'>ОБНУЛИТЬ ТОКЕН</button></form>";
611:     content += "<form action='/admin' method='GET'><input type='hidden' name='reset_all' value='1'><button type='submit' class='btn btn-red' onclick='return confirm(\"Обнулить лимиты вообще ВСЕХ?\");'>ОБНУЛИТЬ ВСЕХ СЕГОДНЯ</button></form></div>";
612:     
613:     content += "<div style='background:#f9f9f9; padding:20px; border-radius:10px; margin-bottom:20px;'><h3>Управление системой</h3>";
614:     content += "<button class='btn btn-green' onclick=\"window.location.href='/update'\">ОБНОВИТЬ ОПРОСНИКИ</button>";
615:     content += "<button class='btn btn-gray' onclick=\"if(confirm('Точно перезагрузить плату?')) window.location.href='/reboot'\">ПЕРЕЗАГРУЗИТЬ ПЛАТУ</button></div>";
616:     
617:     content += "<div style='background:#f9f9f9; padding:20px; border-radius:10px; margin-bottom:20px;'><h3>Настройки Wi-Fi</h3><form action='/save_wifi' method='POST'><label>Имя сети базы:</label><input type='text' name='ap_name' value='" + ap_name + "' required><label>Роутер SSID:</label><input type='text' name='router_ssid' value='" + router_ssid + "' required><label>Пароль:</label><input type='text' name='router_pass' value='" + router_pass + "'><button type='submit' class='btn btn-blue'>СОХРАНИТЬ И ПЕРЕЗАГРУЗИТЬ</button></form></div>";
618: 
619:     content += "<div style='background:#f9f9f9; padding:20px; border-radius:10px; margin-bottom:20px;'><h3>Смена пароля администратора</h3>";
620:     content += "<form action='/change_pass' method='POST'><input type='password' name='old_pass' placeholder='Старый пароль' required><input type='password' name='new_pass' placeholder='Новый пароль' required><input type='password' name='new_pass2' placeholder='Повторите новый пароль' required><button type='submit' class='btn btn-orange'>ИЗМЕНИТЬ ПАРОЛЬ</button></form></div>";
621: 
622:     content += "<button class='btn btn-gray' onclick=\"window.location.href='/logout'\">ВЫЙТИ ИЗ АДМИНКИ</button>";
623:     
624:     content += "</body></html>";
625:     webServer.send(200, "text/html", content);
626:   });
627: 
628:   webServer.on("/save_wifi", HTTP_POST, []() {
629:     if (!checkAuth()) { webServer.sendHeader("Location", "/admin"); webServer.send(302, "text/plain", ""); return; }
630:     preferences.putString("ssid", webServer.arg("router_ssid")); preferences.putString("pass", webServer.arg("router_pass")); preferences.putString("ap", webServer.arg("ap_name"));
631:     addLog("Изменены настройки Wi-Fi. Отправка команды на перезагрузку.");
632:     webServer.send(200, "text/html", "<html><head><meta charset='utf-8'></head><body style='text-align:center; padding:50px;'><h2>Настройки сохранены! Перезагрузка...</h2></body></html>");
633:     delay(2000); ESP.restart(); 
634:   });
635:   
636:   webServer.on("/update", []() { 
637:     if (!checkAuth()) { webServer.sendHeader("Location", "/admin"); webServer.send(302, "text/plain", ""); return; }
638:     addLog("Запущено ручное обновление опросников...");
639:     fetchFormFromServer(); 
640:     webServer.send(200, "text/html", "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'></head><body style='text-align:center; padding:50px; font-family:sans-serif;'><h2 style='color:#2ecc71;'>✅ Опросники обновлены!</h2><br><button onclick=\"window.location.href='/admin'\" style='padding:15px; font-size:16px; border-radius:5px; border:none; background:#1a73e8; color:white; font-weight:bold; cursor:pointer;'>Вернуться в админку</button></body></html>"); 
641:   });
642:   
643:   webServer.on("/reboot", []() {
644:     if (!checkAuth()) { webServer.sendHeader("Location", "/admin"); webServer.send(302, "text/plain", ""); return; }
645:     addLog("Получена команда на ручную перезагрузку.");
646:     webServer.send(200, "text/html", "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'></head><body style='text-align:center; padding:50px; font-family:sans-serif;'><h2 style='color:#333;'>Плата перезагружается...</h2><p style='color:gray;'>Подключитесь к сети заново через 10 секунд.</p></body></html>");
647:     delay(1000); ESP.restart();
648:   });
649: 
650:   webServer.onNotFound([]() { webServer.sendHeader("Location", "/", true); webServer.send(302, "text/plain", ""); });
651:   webServer.begin();
652: }
653: 
654: void loop() {
655:   esp_task_wdt_reset(); 
656:   dnsServer.processNextRequest(); 
657:   webServer.handleClient();
658:   
659:   if (ledBlinkInterval > 0) {
660:     if (millis() - lastLedBlink >= ledBlinkInterval) {
661:       lastLedBlink = millis();
662:       ledState = !ledState;
663:       digitalWrite(LED_PIN, ledState);
664:     }
665:   }
666: 
667:   if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
668:     if (!buttonPressed) {
669:       buttonPressed = true;
670:       buttonPressTime = millis();
671:     } else if (millis() - buttonPressTime > 5000) {
672:       digitalWrite(LED_PIN, LOW); 
673:       Serial.println("!!! ХАРД РЕСЕТ ЧЕРЕЗ КНОПКУ BOOT !!!");
674:       preferences.clear(); 
675:       delay(1000);
676:       ESP.restart();
677:     }
678:   } else {
679:     buttonPressed = false;
680:   }
681:   
682:   struct tm timeinfo;
683:   if (WiFi.status() == WL_CONNECTED && getLocalTime(&timeinfo, 0) && timeinfo.tm_year > 120) {
684:     if (lastUpdateDay != -1 && timeinfo.tm_mday != lastUpdateDay) {
685:       addLog("Наступил новый день (" + String(timeinfo.tm_mday) + "-е число). Запущено авто-обновление.");
686:       fetchFormFromServer(); 
687:       clearAllTokens(); 
688:       lastUpdateDay = timeinfo.tm_mday; 
689:       preferences.putInt("last_day", lastUpdateDay);
690:     }
691:   }
692: }
693: 
The above content shows the entire, complete file contents of the requested file.

