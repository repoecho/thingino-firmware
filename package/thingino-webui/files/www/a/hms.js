(function () {
  "use strict";
  var POLL = 2000;
  var sensors = [], currentState = "DISARMED", currentArmed = "none", delayEnd = 0;
  var editingMac = null, timerInt = null, kpdBuf = "", armingType = null;

  function $(s) { return document.querySelector(s); }
  function $$(s) { return document.querySelectorAll(s); }

  async function jf(url, opts) {
    var fo = { headers: { Accept: "application/json" }, cache: "no-store", ...opts };
    if (opts && opts.method === "POST") fo.headers["Content-Type"] = "application/x-www-form-urlencoded";
    try { var r = await fetch(url, fo); return JSON.parse(await r.text()); } catch(e) { return null; }
  }

  function esc(s) { return String(s||"").replace(/[&<>"]/g,function(m){return {"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;"}[m]||m;}); }
  function tAgo(e) { var d = Math.floor(Date.now()/1000) - e; if (d < 10) return "now"; if (d < 60) return d + "s"; if (d < 3600) return Math.floor(d/60) + "m"; return new Date(e*1000).toLocaleTimeString(); }
  function tm(e) { return new Date(e*1000).toLocaleTimeString(); }

  function sLabel(s,a) { switch(s) { case "DISARMED": return "Disarmed"; case "ARMING_EXIT_DELAY": return "Arming " + a; case "ARMED_HOME": return "Armed Home"; case "ARMED_AWAY": return "Armed Away"; case "ENTRY_DELAY": return "Entry Delay"; case "ALARM": return "ALARM!"; default: return s; } }
  function sClass(s) { switch(s) { case "DISARMED": return "bg-secondary"; case "ARMING_EXIT_DELAY": case "ARMED_HOME": case "ENTRY_DELAY": return "bg-warning text-dark"; case "ARMED_AWAY": case "ALARM": return "bg-danger"; default: return "bg-secondary"; } }
  function cIcon(c) { switch(c) { case "CONTACT": return "bi bi-door-open"; case "MOTION": return "bi bi-person-walking"; case "LEAK": return "bi bi-droplet"; default: return "bi bi-question-circle"; } }
  function sBadge(s) { if (s==="OPEN"||s==="ACTIVE") return '<span class="badge bg-danger">'+s+"</span>"; if (s==="CLOSED"||s==="INACTIVE") return '<span class="badge bg-success">'+s+"</span>"; return '<span class="badge bg-secondary">'+s+"</span>"; }
  function sRow(s) { return (s==="OPEN"||s==="ACTIVE") ? "list-group-item-danger" : ""; }

  // === State ===
  async function loadState() {
    var d = await jf("/x/json-hms-mode.cgi");
    if (!d) return;
    currentState = d.state || "DISARMED"; currentArmed = d.armed_type || "none"; delayEnd = parseInt(d.delay_end)||0;
    updateUI();
    updateTimer();
  }

  function updateUI() {
    var sb = $("#state-badge"); if (sb) { sb.textContent = sLabel(currentState, currentArmed); sb.className = "badge state-badge " + sClass(currentState); }
    var ab = $("#alarm-banner"); if (ab) ab.classList.toggle("d-none", currentState !== "ALARM");
    var s = currentState;
    $("#btn-disarm").disabled = (s === "DISARMED");
    $("#btn-arm-home").disabled = (s !== "DISARMED");
    $("#btn-arm-away").disabled = (s !== "DISARMED");
    $("#kpd-arm-home").disabled = (s !== "DISARMED");
    $("#kpd-arm-away").disabled = (s !== "DISARMED");
  }

  function updateTimer() {
    var ts = $("#timer-section"), td = $("#timer-seconds"), tl = $("#timer-label");
    if (delayEnd <= 0 || Math.floor(Date.now()/1000) >= delayEnd) { ts.classList.add("d-none"); return; }
    ts.classList.remove("d-none");
    var rem = Math.max(0, delayEnd - Math.floor(Date.now()/1000));
    td.textContent = rem;
    tl.textContent = currentState === "ARMING_EXIT_DELAY" ? "Exit delay — close door" : currentState === "ENTRY_DELAY" ? "Enter PIN to disarm!" : "";
    if (timerInt) clearInterval(timerInt);
    timerInt = setInterval(function() {
      var r = Math.max(0, delayEnd - Math.floor(Date.now()/1000));
      td.textContent = r;
      if (r <= 0) ts.classList.add("d-none");
    }, 1000);
  }

  // === Sensors ===
  async function loadSensors() {
    var d = await jf("/x/json-hms-sensors.cgi");
    if (!d) return;
    sensors = d.sensors || [];
    var gw = $("#gateway-status");
    if (gw) { gw.textContent = d.gateway_status || "offline"; gw.className = "badge bg-" + (d.gateway_status === "online" ? "success" : "secondary"); }
    var c = $("#sensor-count"); if (c) c.textContent = sensors.length;
    renderSensors();
    loadEvents();
  }

  function renderSensors() {
    var l = $("#sensor-list"); if (!l) return;
    if (!sensors.length) { l.innerHTML = '<div class="list-group-item text-secondary text-center py-4">No sensors found</div>'; return; }
    l.innerHTML = sensors.map(function(s) {
      return '<div class="list-group-item ' + sRow(s.state) + ' d-flex justify-content-between align-items-center py-1">' +
        '<div class="d-flex align-items-center gap-2"><i class="' + cIcon(s.class) + '"></i><div><div class="fw-bold small">' + esc(s.name) + '</div><small class="text-secondary">' + esc(s.mac) + " &middot; " + (s.type||"entry") + "</small></div></div>" +
        '<div class="d-flex align-items-center gap-1"><small class="text-secondary">' + s.battery + '%</small>' + sBadge(s.state) +
        '<button class="btn btn-sm btn-outline-secondary sen-edit py-0 px-1" data-mac="' + s.mac + '" data-name="' + esc(s.name) + '" data-type="' + (s.type||"entry") + '" data-tune="1" title="Configure"><i class="bi bi-pencil"></i></button></div></div>';
    }).join("");
    l.querySelectorAll(".sen-edit").forEach(function(b) {
      b.addEventListener("click", function() {
        editingMac = this.dataset.mac;
        $("#sen-name").value = this.dataset.name === editingMac ? "" : this.dataset.name;
        $("#sen-type").value = this.dataset.type;
        $("#sen-tune").checked = this.dataset.tune !== "0";
        $("#sen-mac").textContent = "MAC: " + this.dataset.mac;
        new bootstrap.Modal($("#sensor-modal")).show();
      });
    });
  }

  async function loadEvents() {
    var d = await jf("/x/json-hms-events.cgi");
    if (!d) return;
    var ev = Array.isArray(d) ? d : [];
    var el = $("#event-list"); if (!el) return;
    if (!ev.length) { el.innerHTML = '<div class="list-group-item text-secondary text-center py-4">No events yet</div>'; return; }
    el.innerHTML = ev.slice(0,100).map(function(e) {
      return '<div class="list-group-item d-flex justify-content-between align-items-center py-1"><div class="d-flex align-items-center gap-2"><i class="' + cIcon(e.class) + '"></i><div><div class="fw-bold small">' + esc(e.name||e.sensor) + "</div></div></div><div class='d-flex align-items-center gap-1'>" + sBadge(e.state) + '<small class="text-secondary">' + tAgo(e.time) + "</small></div></div>";
    }).join("");
  }

  async function loadLive() {
    var d = await jf("/x/json-hms-livelog.cgi");
    if (!d) return;
    var en = Array.isArray(d) ? d : [];
    var c = $("#live-count"); if (c) c.textContent = en.length;
    var l = $("#live-feed-list"); if (!l) return;
    if (!en.length) { l.innerHTML = '<div class="list-group-item text-secondary text-center py-3">Waiting...</div>'; return; }
    l.innerHTML = en.slice(0,50).map(function(e) {
      return '<div class="list-group-item d-flex align-items-center gap-2 py-1 px-2"><span class="text-secondary" style="min-width:4rem">' + tm(e.time) + '</span><span class="fw-bold" style="min-width:4rem">' + esc(e.topic) + '</span><span class="text-break">' + esc(e.detail) + "</span></div>";
    }).join("");
  }

  async function loadProtected() {
    var d = await jf("/x/json-hms-protected.cgi");
    if (!d) return;
    var w = $("#protected-week"); if (w) w.textContent = d.week_hours || 0;
    var t = $("#protected-total"); if (t) t.textContent = (d.total_hours||0) + " total hours";
  }

  // === Arm/Disarm ===
  function doArm(type) {
    jf("/x/json-hms-mode.cgi", { method: "POST", body: "action=arm&type=" + type });
  }

  function doDisarm(pin) {
    jf("/x/json-hms-mode.cgi", { method: "POST", body: "action=disarm&pin=" + encodeURIComponent(pin) }).then(function(d) {
      if (d && d.status === "alarm_triggered") { loadState(); return; }
      if (d && d.status === "error") {
        var err = $("#pin-error"); if (err) { err.textContent = "Wrong PIN (" + (d.attempts||1) + "/3)"; err.classList.remove("d-none"); }
        return showPin("Enter PIN", doDisarm);
      }
      loadState();
    });
  }

  function doSilenceAlarm() {
    showPin("Enter PIN to silence", function(pin) {
      jf("/x/json-hms-mode.cgi", { method: "POST", body: "action=cancel_alarm&pin=" + encodeURIComponent(pin) }).then(function(d) {
        if (d && d.status === "disarmed") loadState();
        else { showPin("Enter PIN to silence", doSilenceAlarm); }
      });
    });
  }

  // === Arming Confirmation Modal ===
  function showArmConfirm(type) {
    armingType = type;
    $("#arm-confirm-title").textContent = "Arm " + (type === "home" ? "Home" : "Away") + "?";
    $("#arm-confirm-label").textContent = "Exit delay will begin — close door";
    var modal = new bootstrap.Modal($("#arm-confirm-modal"), { backdrop: "static" });
    modal.show();
  }

  // === PIN Pad ===
  var pinCb = null, pinVal = "";
  function showPin(title, cb) { pinCb = cb; pinVal = ""; $("#pin-title").textContent = title||"Enter PIN"; $("#pin-display").textContent = ""; $("#pin-error").classList.add("d-none"); new bootstrap.Modal($("#pin-modal"),{backdrop:"static"}).show(); }
  function initPinPad() {
    $$("#pin-modal .pin-btn").forEach(function(b) {
      b.addEventListener("click", function() {
        var v = this.dataset.pin;
        if (v === "clear") { pinVal = pinVal.slice(0,-1); }
        else if (v === "enter") { if (pinCb) { pinCb(pinVal); pinCb = null; } bootstrap.Modal.getInstance($("#pin-modal")).hide(); return; }
        else if (pinVal.length < 6) { pinVal += v; }
        $("#pin-display").textContent = "\u2022".repeat(pinVal.length);
      });
    });
  }

  // === Keypad ===
  function initKeypad() {
    $$("#keypad-body .kpd-btn").forEach(function(b) {
      b.addEventListener("click", function() {
        var v = this.dataset.kpd;
        if (v === "clear") { kpdBuf = ""; }
        else if (v === "enter") {
          if (kpdBuf.length > 0) {
            doDisarm(kpdBuf);
            kpdBuf = "";
          }
        } else if (kpdBuf.length < 6) { kpdBuf += v; }
        $("#kpd-display").textContent = "\u2022".repeat(kpdBuf.length);
      });
    });
    $("#kpd-arm-home").addEventListener("click", function(){ showArmConfirm("home"); });
    $("#kpd-arm-away").addEventListener("click", function(){ showArmConfirm("away"); });
    $("#kpd-panic").addEventListener("click", function(){
      jf("/x/json-hms-keypad.cgi", { method: "POST", body: "cmd=panic" });
    });
  }

  // === Settings ===
  async function loadSettings() {
    var d = await jf("/x/json-hms-config.cgi");
    if (!d) return;
    if (d.system_pin) $("#cfg-system-pin").value = d.system_pin;
    if (d.entry_delay_home !== undefined) $("#cfg-entry-delay-home").value = d.entry_delay_home;
    if (d.entry_delay_away !== undefined) $("#cfg-entry-delay-away").value = d.entry_delay_away;
    if (d.exit_delay_home !== undefined) $("#cfg-exit-delay-home").value = d.exit_delay_home;
    if (d.exit_delay_away !== undefined) $("#cfg-exit-delay-away").value = d.exit_delay_away;
    if (d.silent_entry) $("#cfg-silent-entry").checked = true;
    if (d.silent_exit) $("#cfg-silent-exit").checked = true;
    if (d.tune_volume) $("#cfg-tune-volume").value = d.tune_volume;
  }

  function saveSettings() {
    var fields = {
      system_pin: $("#cfg-system-pin").value, entry_delay_home: parseInt($("#cfg-entry-delay-home").value)||30, entry_delay_away: parseInt($("#cfg-entry-delay-away").value)||15,
      exit_delay_home: parseInt($("#cfg-exit-delay-home").value)||45, exit_delay_away: parseInt($("#cfg-exit-delay-away").value)||60,
      silent_entry: $("#cfg-silent-entry").checked ? "true" : "false", silent_exit: $("#cfg-silent-exit").checked ? "true" : "false",
      tune_volume: $("#cfg-tune-volume").value
    };
    Object.keys(fields).forEach(function(k) { jf("/x/json-hms-config.cgi", { method: "POST", body: "field=" + encodeURIComponent(k) + "&value=" + encodeURIComponent(fields[k]) }); });
    if (typeof showAlert === "function") showAlert("success", "Settings saved", 2000);
  }

  function saveSensor() {
    var n = $("#sen-name").value.trim() || editingMac;
    var t = $("#sen-type").value;
    var tune = $("#sen-tune").checked ? "1" : "0";
    jf("/x/json-hms-config.cgi", { method: "POST", body: "action=update_sensor&mac=" + encodeURIComponent(editingMac) + "&name=" + encodeURIComponent(n) + "&sensor_type=" + encodeURIComponent(t) + "&tune=" + tune }).then(function() {
      bootstrap.Modal.getInstance($("#sensor-modal")).hide();
      loadSensors();
    });
  }

  // === Export/Import ===
  async function exportConfig() {
    var d = await jf("/x/json-hms-config.cgi");
    if (!d) return;
    var a = document.createElement("a"); a.href = URL.createObjectURL(new Blob([JSON.stringify(d,null,2)],{type:"application/json"})); a.download = "hms-config.json"; a.click();
  }
  function importConfig() { $("#import-file-input").click(); }
  function handleImport(file) {
    var r = new FileReader();
    r.onload = function(e) {
      jf("/x/json-hms-config.cgi", { method: "POST", body: "action=import&data=" + encodeURIComponent(e.target.result) }).then(function(r2) {
        if (r2 && r2.status === "imported") { if (typeof showAlert === "function") showAlert("success","Config restored",2000); loadSettings(); }
        else if (typeof showAlert === "function") showAlert("danger","Invalid config file",3000);
      });
    };
    r.readAsText(file);
  }

  // === Sound Test ===
  function initSoundTest() {
    var c = $("#sound-test-buttons"); if (!c) return;
    ["tiny_pluck","fast_alert_beep","slow_alert_beep","system_is_arming","armed_home","armed_away","disarmed","entry_delay_started","hazard_siren"].forEach(function(t) {
      var b = document.createElement("button"); b.className = "btn btn-outline-secondary btn-sm"; b.textContent = t.replace(/_/g," ");
      b.addEventListener("click", function() { jf("/x/json-hms-sound-test.cgi",{method:"POST",body:"track="+encodeURIComponent(t)}); });
      c.appendChild(b);
    });
  }

  // === Init ===
  function init() {
    initPinPad();
    initKeypad();
    initSoundTest();
    $("#btn-arm-home").addEventListener("click", function(){ showArmConfirm("home"); });
    $("#btn-arm-away").addEventListener("click", function(){ showArmConfirm("away"); });
    $("#arm-confirm-ok").addEventListener("click", function() { bootstrap.Modal.getInstance($("#arm-confirm-modal")).hide(); doArm(armingType); });
    $("#arm-confirm-cancel").addEventListener("click", function() { bootstrap.Modal.getInstance($("#arm-confirm-modal")).hide(); });
    $("#btn-disarm").addEventListener("click", function() { showPin("Enter PIN to disarm", doDisarm); });
    $("#btn-silence-alarm").addEventListener("click", doSilenceAlarm);
    $("#btn-save-settings").addEventListener("click", saveSettings);
    $("#sen-save").addEventListener("click", saveSensor);
    $("#btn-export-config").addEventListener("click", exportConfig);
    $("#btn-import-config").addEventListener("click", importConfig);
    $("#import-file-input").addEventListener("change", function(e) { if (e.target.files && e.target.files[0]) handleImport(e.target.files[0]); });
    $("#clear-events").addEventListener("click", function() { jf("/x/json-hms-events.cgi", { method: "POST", body: "action=clear" }); loadEvents(); });
    function poll() { loadState(); loadSensors(); loadLive(); loadProtected(); }
    loadSettings();
    poll();
    setInterval(poll, POLL);
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init, { once: true });
  else init();
})();
