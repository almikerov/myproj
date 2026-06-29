const fs = require('fs');
const jsdom = require('jsdom');
const { JSDOM } = jsdom;

const code = fs.readFileSync('cloudflare/worker.js', 'utf8');
const htmlContent = code.split('return `')[1].split('`;')[0].replace(/\\n/g, '\n');

const virtualConsole = new jsdom.VirtualConsole();
virtualConsole.on("jsdomError", (e) => { console.error("JSDOM Error:", e); });

const dom = new JSDOM(htmlContent, { 
  runScripts: "dangerously", 
  virtualConsole,
  beforeParse(window) {
    window.fetch = async (url) => {
      console.log("Mock fetch called:", url);
      if (url === "/api/health") {
        return {
          ok: true,
          json: async () => ({ ok: true, d1: true })
        };
      }
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
    console.log("Health text after load:", dom.window.document.getElementById("health").textContent);
    console.log("Clicking Auth button");
    dom.window.document.getElementById("saveToken").click();
    setTimeout(() => {
        console.log("Health text after click:", dom.window.document.getElementById("health").textContent);
    }, 100);
}, 200);
