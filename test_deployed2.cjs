const jsdom = require("jsdom");
const { JSDOM } = jsdom;
const fs = require('fs');

async function test() {
    const htmlContent = fs.readFileSync("downloaded_admin2.html", "utf8");
    if (!htmlContent) {
       console.log("No content in downloaded_admin.html");
       return;
    }
    const virtualConsole = new jsdom.VirtualConsole();
    virtualConsole.sendTo(console);
    virtualConsole.on("jsdomError", (e) => { console.error("JSDOM Error:", e.stack || e); });

    const dom = new JSDOM(htmlContent, { 
        runScripts: "dangerously", 
        virtualConsole,
        beforeParse(window) {
            window.fetch = async (url) => {
                console.log("Mock fetch called:", url);
                return {
                    ok: false,
                    json: async () => ({ error: "admin_auth_required" })
                };
            };
            window.alert = console.log;
            window.prompt = () => null;
            window.confirm = () => false;
        }
    });

    setTimeout(() => {
        const h = dom.window.document.getElementById("health");
        console.log("Health text after load:", h ? h.textContent : "null");
        const btn = dom.window.document.getElementById("saveToken");
        console.log("Clicking Auth button...");
        if (btn) btn.click();
        else console.log("No Auth button found!");
        setTimeout(() => {
            console.log("Health text after click:", h ? h.textContent : "null");
        }, 100);
    }, 200);
}
test();
