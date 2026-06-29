
    const state = { devices: [] };
    const token = document.getElementById("token");
    try { token.value = localStorage.getItem("esp32_admin_token") || ""; } catch(e) {}

    const api = async (path, options = {}) => {
      const response = await fetch(path, {
        ...options,
        headers: {
          "Content-Type": "application/json",
          "Authorization": "Bearer " + token.value,
          ...(options.headers || {})
        }
      });
      const data = await response.json();
      if (!response.ok) throw new Error(data.error || "request_failed");
      return data;
    };

    const formatDate = (iso) => {
      if(!iso) return "never";
      const d = new Date(iso);
      return d.toLocaleString();
    };

    window.showError = (error) => { 
      const msg = error && error.message ? error.message : String(error);
      const h = document.getElementById("health");
      const d = document.getElementById("health-dot");
      if (h) h.textContent = msg; 
      if (d) d.className = "dot bad"; 
      console.error("UI Error:", error);
    };
    
    window.refresh = async () => {
      try {
        let health;
        try {
          health = await fetch("/api/health").then(r => r.json());
        } catch(e) {
          throw new Error("Cannot connect to API");
        }
        
        const h = document.getElementById("health");
        const d = document.getElementById("health-dot");
        if (h) h.textContent = health.d1 ? "D1 connected" : "D1 missing";
        if (d) d.className = "dot " + (health.d1 ? "ok" : "bad");
        
        if (!token.value) {
           if (h) h.textContent = "Auth required";
           if (d) d.className = "dot warn";
           return;
        }

        const [devices, commands, logs] = await Promise.all([
          api("/api/admin/devices"),
          api("/api/admin/commands"),
          api("/api/admin/logs?limit=100")
        ]);
        
        state.devices = devices.devices;
        renderDevices(devices.devices);
        renderCommands(commands.commands);
        renderLogs(logs.logs);
      } catch (err) {
        window.showError(err);
      }
    };

    const renderDevices = (items) => {
      document.getElementById("devices").innerHTML = items.map(d =>
        "<tr><td><div style='font-weight:600; color:var(--text);'>" + esc(d.device_id) + "</div></td><td><span class='status " + esc(d.status) + "'>" + esc(d.status) + "</span></td><td>" + esc(d.firmware_version || "") + "</td><td><div style='font-weight:500;'>" + (d.wifi_ssid ? esc(d.wifi_ssid) : "—") + "</div><div class='muted' style='margin-top:4px; font-family:monospace;'>" + (d.wifi_pass ? "***" : "—") + "</div></td><td class='muted'>" + formatDate(d.last_seen_at) + "</td><td><div style='display:flex; gap:4px;'><button class='secondary' onclick='editDevice("" + escAttr(d.device_id) + "")' title='Edit' style='padding:4px 8px;'>✏️</button><button class='secondary' onclick='delDevice("" + escAttr(d.device_id) + "")' title='Delete' style='padding:4px 8px;'>🗑️</button><button class='secondary' onclick='queue("" + escAttr(d.device_id) + "","reboot")' title='Reboot' style='padding:4px 8px;'>🔄</button><button class='secondary' onclick='queue("" + escAttr(d.device_id) + "","shutdown")' title='Sleep/Shutdown' style='padding:4px 8px;'>💤</button></div></td></tr>"
      ).join("");
      
      document.getElementById("commandDevice").innerHTML = items.map(d =>
        "<option value='" + escAttr(d.device_id) + "'>" + esc(d.device_id) + "</option>"
      ).join("");
    };

    window.editDevice = (id) => {
      const d = state.devices.find(x => x.device_id === id);
      if(!d) return;
      document.getElementById("deviceId").value = d.device_id;
      document.getElementById("deviceFirmware").value = d.firmware_version;
      document.getElementById("deviceSecret").value = "";
      if (d.wifi_ssid) {
        const ssid = prompt("Set new Wi-Fi SSID for " + d.device_id, d.wifi_ssid);
        if (ssid !== null) {
          const pass = prompt("Set new Wi-Fi Password for " + d.device_id, "");
          if (pass !== null) {
             api("/api/admin/commands", { method: "POST", body: JSON.stringify({
               deviceId: id, type: "update_wifi", payload: { ssid, pass }
             }) }).then(() => { alert("Wi-Fi update command queued!"); refresh(); }).catch(showError);
          }
        }
      } else {
        document.getElementById("deviceId").focus();
      }
    };

    window.delDevice = (id) => {
      if(confirm("Delete device " + id + "?")) {
        api("/api/admin/devices", { method: "DELETE", body: JSON.stringify({ deviceId: id }) })
          .then(() => refresh()).catch(showError);
      }
    };

    window.queue = (id, type) => {
      api("/api/admin/commands", { method: "POST", body: JSON.stringify({
        deviceId: id, type: type, payload: {}
      }) }).then(() => refresh()).catch(showError);
    };

    const renderCommands = (items) => {};

    const renderLogs = (items) => {
      const logsEl = document.getElementById("logs");
      if (!logsEl) return;
      logsEl.innerHTML = items.map(l =>
        "<span style='color:var(--muted)'>[" + formatDate(l.created_at) + "]</span> <span style='color:#fcd34d;'>" + esc(l.device_id) + "</span> <span style='color:var(--ok)'>" + esc(l.level) + "</span>: " + esc(l.message)
      ).join("\n");
    };

    window.submitAuth = () => {
      try { localStorage.setItem("esp32_admin_token", token.value); } catch(e) {}
      const h = document.getElementById("health");
      if (h) h.textContent = "Loading...";
      window.refresh();
    };

    const tokenEl = document.getElementById("token");
    if (tokenEl) {
      tokenEl.onkeydown = (e) => { if(e.key === "Enter") window.submitAuth(); };
    }

    const btnCreate = document.getElementById("createDevice");
    if (btnCreate) btnCreate.onclick = async () => {
      try {
        await api("/api/admin/devices", { method: "POST", body: JSON.stringify({
          deviceId: value("deviceId"), secret: value("deviceSecret"),
          firmwareVersion: value("deviceFirmware")
        }) });
        alert("Device saved!");
        await refresh();
      } catch(e) { alert(e.message); }
    };

    document.getElementById("queueCommand").onclick = async () => {
      try {
        await api("/api/admin/commands", { method: "POST", body: JSON.stringify({
          deviceId: value("commandDevice"), type: value("commandType"),
          payload: JSON.parse(value("commandPayload") || "{}")
        }) });
        alert("Command queued!");
        await refresh();
      } catch(e) { alert(e.message); }
    };

    document.getElementById("publishFirmware").onclick = async () => {
      try {
        const fileInput = document.getElementById("fwFile");
        if (!fileInput.files.length) return alert("Select a .bin file");
        const version = value("fwVersion");
        if (!version) return alert("Enter target version");
        
        document.getElementById("publishFirmware").textContent = "Uploading...";
        const res = await fetch("/api/admin/firmware/upload?version=" + encodeURIComponent(version), {
          method: "POST",
          headers: { "Authorization": "Bearer " + token.value },
          body: fileInput.files[0]
        });
        const data = await res.json();
        if (!res.ok) throw new Error(data.error);
        alert("Firmware published! URL: " + data.url);
        document.getElementById("publishFirmware").textContent = "Upload & Publish";
        await refresh();
      } catch(e) { 
        alert(e.message); 
        document.getElementById("publishFirmware").textContent = "Upload & Publish";
      }
    };

    const value = (id) => {
      const el = document.getElementById(id);
      return el ? el.value.trim() : "";
    };
    const esc = (s) => String(s !== null && s !== undefined ? s : "").replace(/[&<>"']/g, ch => ({ "&":"&amp;", "<":"&lt;", ">":"&gt;", '"':"&quot;", "'":"&#39;" }[ch]));
    window.escAttr = esc;
    
    try {
      window.refresh();
      setInterval(window.refresh, 15000);
    } catch(e) {
      window.showError(e);
    }
  