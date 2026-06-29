const fs = require('fs');
const jsdom = require('jsdom');
const { JSDOM } = jsdom;

const code = fs.readFileSync('cloudflare/worker.js', 'utf8');
const htmlContent = code.split('return `')[1].split('`;')[0].replace(/\\n/g, '\n');

const dom = new JSDOM(htmlContent, { runScripts: "dangerously" });
console.log("JSDOM initialized without errors.");
