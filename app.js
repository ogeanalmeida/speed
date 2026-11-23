/* Bike Speed — PWA
 *
 * Tudo roda no aparelho: nenhum dado vai para a nuvem (só localStorage).
 * Fonte dos dados: "sim" (simulação dentro do app) ou "ble" (ESP32 via Web Bluetooth).
 *
 * PROTOCOLO BLE (o firmware do Arduino IDE deve seguir isto):
 *   Nome do dispositivo: começa com "BikeSpeed"
 *   Serviço:        b1cec10c-0001-4e5a-9a3b-5eb1c0de0001
 *   TELEMETRIA      b1cec10c-0002-...  (NOTIFY, 12 bytes, little-endian)
 *       [0-1] uint16  velocidade atual   (km/h x 10)
 *       [2-3] uint16  velocidade média   (km/h x 10)
 *       [4-5] uint16  velocidade máxima  (km/h x 10)
 *       [6-9] uint32  distância do percurso (metros)
 *       [10]  uint8   bateria (0-100 %)
 *       [11]  uint8   flags (bit0 = em movimento, bit1 = percurso em andamento no ESP32)
 *   COMANDO         b1cec10c-0003-...  (WRITE)
 *       0x01 + uint32  INICIAR percurso (objetivo em metros) -> ESP32 zera distância/média/máx
 *       0x02           FINALIZAR percurso
 *       0x03 + uint16  circunferência da roda (mm)
 */
(() => {
  'use strict';

  const $ = (id) => document.getElementById(id);

  const BLE = {
    NAME_PREFIX: 'BikeSpeed',
    SERVICE: 'b1cec10c-0001-4e5a-9a3b-5eb1c0de0001',
    TELEMETRY: 'b1cec10c-0002-4e5a-9a3b-5eb1c0de0001',
    COMMAND: 'b1cec10c-0003-4e5a-9a3b-5eb1c0de0001',
    CMD_START: 0x01,
    CMD_END: 0x02,
    CMD_WHEEL: 0x03,
  };

  /* Wi-Fi: o app só consegue falar com o ESP32 por Wi-Fi quando é aberto pelo endereço http do próprio ESP32
     (páginas https não podem chamar endereços http da rede local). */
  const WIFI_OK = location.protocol === 'http:';

  /* ---------------- tipos de bike (circunferência média, mm) ---------------- */
  const BIKES = [
    { id: '29', num: '29', unit: 'aro', mm: 2300 },
    { id: '27.5', num: '27,5', unit: 'aro', mm: 2200 },
    { id: '26', num: '26', unit: 'aro', mm: 2070 },
    { id: '24', num: '24', unit: 'aro', mm: 1950 },
    { id: '20', num: '20', unit: 'aro', mm: 1600 },
    { id: '700c', num: '700c', unit: '', mm: 2100 },
  ];

  /* ---------------- estado e armazenamento local ---------------- */
  const KEY = 'bikeSpeed.v1';
  const DEFAULTS = { goalKm: 21, accumM: 0, wheelMm: 2100, bike: '700c', wifiHost: 'bikespeed.local', ride: null, mode: 'sim', simBat: 100, simRate: 1 };

  const num = (v, min, max, fallback) => (Number.isFinite(+v) ? Math.min(max, Math.max(min, +v)) : fallback);

  function load() {
    let raw = {};
    try { raw = JSON.parse(localStorage.getItem(KEY)) || {}; } catch (_) { raw = {}; }
    const s = { ...DEFAULTS, ...raw };
    s.goalKm = Math.round(num(s.goalKm, 1, 300, DEFAULTS.goalKm));
    s.accumM = num(s.accumM, 0, 1e9, 0);
    s.wheelMm = Math.round(num(s.wheelMm, 1000, 3000, DEFAULTS.wheelMm));
    if (!raw.bike || !(BIKES.some((b) => b.id === s.bike) || s.bike === 'custom')) {
      const m = BIKES.find((b) => b.mm === s.wheelMm);
      s.bike = m ? m.id : 'custom';
    }
    s.simBat = Math.round(num(s.simBat, 0, 100, 100));
    s.simRate = [1, 10, 60].includes(+s.simRate) ? +s.simRate : 1;
    s.wifiHost = String(s.wifiHost || DEFAULTS.wifiHost).toLowerCase().replace(/[^a-z0-9.:-]/g, '').slice(0, 60) || DEFAULTS.wifiHost;
    s.mode = s.mode === 'ble' ? 'ble' : s.mode === 'usb' ? 'usb' : s.mode === 'wifi' && WIFI_OK ? 'wifi' : 'sim';
    if (s.ride && typeof s.ride === 'object') {
      s.ride = {
        startedAt: s.ride.startedAt || Date.now(),
        distM: num(s.ride.distM, 0, 1e9, 0),
        baseM: num(s.ride.baseM, 0, 1e9, 0),
        movingS: num(s.ride.movingS, 0, 1e9, 0),
        maxKmh: num(s.ride.maxKmh, 0, 200, 0),
        goalKm: Math.round(num(s.ride.goalKm, 1, 300, s.goalKm)),
        goalHit: !!s.ride.goalHit,
      };
    } else {
      s.ride = null;
    }
    return s;
  }

  let S = load();
  function save() {
    try { localStorage.setItem(KEY, JSON.stringify(S)); } catch (_) { /* armazenamento indisponível */ }
  }

  const live = { speed: 0, avg: 0, battery: S.simBat, connected: false };

  /* ---------------- cor da bateria ---------------- */
  // Pontos de referência do Figma (15% vermelho, 50% laranja, 100% verde).
  // Entre eles, interpolação em HSL: o matiz percorre o espectro gradualmente.
  const hexToRgb = (h) => [1, 3, 5].map((i) => parseInt(h.slice(i, i + 2), 16) / 255);
  function rgbToHsl([r, g, b]) {
    const mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn, l = (mx + mn) / 2;
    let h = 0, s = 0;
    if (d) {
      s = d / (1 - Math.abs(2 * l - 1));
      if (mx === r) h = 60 * (((g - b) / d) % 6);
      else if (mx === g) h = 60 * ((b - r) / d + 2);
      else h = 60 * ((r - g) / d + 4);
      if (h < 0) h += 360;
    }
    return { h, s, l };
  }
  function hslToRgb({ h, s, l }) {
    const c = (1 - Math.abs(2 * l - 1)) * s;
    const x = c * (1 - Math.abs(((h / 60) % 2) - 1));
    const m = l - c / 2;
    const t = h < 60 ? [c, x, 0] : h < 120 ? [x, c, 0] : h < 180 ? [0, c, x] : h < 240 ? [0, x, c] : h < 300 ? [x, 0, c] : [c, 0, x];
    return t.map((v) => v + m);
  }
  const ANCHORS = [
    { p: 15, c: rgbToHsl(hexToRgb('#ff4848')) },
    { p: 50, c: rgbToHsl(hexToRgb('#ffa748')) },
    { p: 100, c: rgbToHsl(hexToRgb('#93d379')) },
  ];
  function batteryRgb(p) {
    if (p <= ANCHORS[0].p) return hslToRgb(ANCHORS[0].c);
    for (let i = 0; i < ANCHORS.length - 1; i++) {
      const a = ANCHORS[i], b = ANCHORS[i + 1];
      if (p <= b.p) {
        const t = (p - a.p) / (b.p - a.p);
        return hslToRgb({ h: a.c.h + (b.c.h - a.c.h) * t, s: a.c.s + (b.c.s - a.c.s) * t, l: a.c.l + (b.c.l - a.c.l) * t });
      }
    }
    return hslToRgb(ANCHORS[ANCHORS.length - 1].c);
  }
  const toCss = (rgb) => `rgb(${rgb.map((v) => Math.round(v * 255)).join(',')})`;
  const mixWhite = (rgb, t) => rgb.map((v) => v + (1 - v) * t);

  /* ---------------- formatação ---------------- */
  const fmt = (n, d = 1) => n.toFixed(d).replace('.', ',');
  const fmtAccum = (km) => (km >= 100 ? String(Math.round(km)) : fmt(km, 1));

  /* ---------------- telas ---------------- */
  function showScreen(name) {
    $('app').dataset.screen = name;
    $('screenGoal').hidden = name !== 'goal';
    $('screenRide').hidden = name !== 'ride';
    $('screenBike').hidden = name !== 'bike';
    $('bikeBtn').hidden = name !== 'goal';
    $('backBtn').hidden = name !== 'bike';
    $('connBtn').hidden = name === 'bike';
  }

  /* ---------------- tela: tipo de bike ---------------- */
  let draft = { id: '700c', mm: 2100 };
  function bikeLabel(id, mm) {
    const b = BIKES.find((x) => x.id === id);
    return b ? (b.unit ? 'Aro ' + b.num : b.num) : mm + ' mm';
  }
  function renderBike() {
    const b = BIKES.find((x) => x.id === draft.id);
    $('bikeNum').textContent = b ? b.num : draft.mm;
    $('bikeUnit').textContent = b ? b.unit : 'mm';
    $('bikeMm').textContent = draft.mm;
    document.querySelectorAll('.bikeopt').forEach((el) => el.setAttribute('aria-checked', String(el.dataset.id === draft.id)));
    $('customCard').dataset.on = String(draft.id === 'custom');
  }
  function openBike() {
    draft = { id: S.bike, mm: S.wheelMm };
    $('customMm').value = S.bike === 'custom' ? S.wheelMm : '';
    showScreen('bike');
    renderBike();
  }
  function applyBike() {
    S.bike = draft.id;
    S.wheelMm = draft.mm;
    $('wheelInput').value = S.wheelMm;
    save();
    sendCommand([BLE.CMD_WHEEL, ...u16(S.wheelMm)]);
    showScreen('goal');
    render();
    toast('Roda: ' + bikeLabel(S.bike, S.wheelMm) + ' · ' + S.wheelMm + ' mm');
  }

  /* ---------------- render ---------------- */
  let lastBat = -1;
  function render() {
    $('goalNum').textContent = S.goalKm;
    $('bikeBtnText').textContent = bikeLabel(S.bike, S.wheelMm);
    document.querySelectorAll('.preset').forEach((b) => b.setAttribute('aria-pressed', String(+b.dataset.km === S.goalKm)));

    const rideM = S.ride ? S.ride.distM : 0;
    const accumTxt = fmtAccum((S.accumM + rideM) / 1000);
    document.querySelectorAll('[data-bind="accum"]').forEach((el) => { el.textContent = accumTxt; });

    if (S.ride) {
      const distKm = S.ride.distM / 1000;
      const objKm = S.ride.goalKm;
      $('speedNum').textContent = Math.round(live.speed);
      $('avgNum').textContent = Math.round(live.avg);
      $('maxNum').textContent = Math.round(S.ride.maxKmh);
      $('distNum').textContent = fmt(distKm, 1);
      $('objNum').textContent = objKm;
      $('restNum').textContent = fmt(Math.max(0, objKm - distKm), 1);
      const pf = Math.min(1, distKm / objKm);
      $('progress').style.setProperty('--pf', pf.toFixed(4));
      $('progress').setAttribute('aria-valuenow', String(Math.round(pf * 100)));
    }

    const bat = Math.round(num(live.battery, 0, 100, 0));
    if (bat !== lastBat) {
      lastBat = bat;
      const rgb = batteryRgb(bat);
      const card = $('batCard');
      card.style.setProperty('--bat-bg', toCss(rgb));
      card.style.setProperty('--bat-fg', toCss(mixWhite(rgb, 0.7)));
      $('batNum').textContent = bat;
    }
  }

  /* ---------------- aviso rápido ---------------- */
  let toastTimer;
  function toast(msg) {
    const el = $('toast');
    el.textContent = msg;
    el.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { el.hidden = true; }, 2800);
  }

  /* ---------------- folhas (confirmação e ajustes) ---------------- */
  let confirmResolve = null;
  function closeSheets() {
    $('scrim').hidden = true;
    $('confirmSheet').hidden = true;
    $('settingsSheet').hidden = true;
    if (confirmResolve) { const r = confirmResolve; confirmResolve = null; r(false); }
  }
  function openSheet(id) {
    $('scrim').hidden = false;
    $(id).hidden = false;
  }
  function confirmSheet({ title, text, ok }) {
    $('confirmTitle').textContent = title;
    $('confirmText').textContent = text;
    $('confirmOkLabel').textContent = ok;
    openSheet('confirmSheet');
    return new Promise((resolve) => { confirmResolve = resolve; });
  }
  function answerConfirm(value) {
    const r = confirmResolve;
    confirmResolve = null;
    closeSheets();
    if (r) r(value);
  }

  /* ---------------- percurso ---------------- */
  let wake = null;
  async function requestWake() {
    try {
      if (S.ride && 'wakeLock' in navigator && !wake) {
        wake = await navigator.wakeLock.request('screen');
        wake.addEventListener('release', () => { wake = null; });
      }
    } catch (_) { /* sem suporte ou negado */ }
  }
  function releaseWake() {
    try { if (wake) wake.release(); } catch (_) { /* ok */ }
    wake = null;
  }

  function startRide() {
    S.ride = { startedAt: Date.now(), distM: 0, baseM: 0, movingS: 0, maxKmh: 0, goalKm: S.goalKm, goalHit: false };
    live.speed = 0;
    live.avg = 0;
    save();
    bleStartSentAt = Date.now();
    sendCommand([BLE.CMD_START, ...u32(S.goalKm * 1000)]);
    requestWake();
    showScreen('ride');
    render();
  }

  async function finishRide() {
    if (!S.ride) return;
    const ok = await confirmSheet({
      title: 'Finalizar percurso',
      text: `Somar ${fmt(S.ride.distM / 1000, 1)} km ao total acumulado e voltar ao início?`,
      ok: 'Finalizar',
    });
    if (!ok) return;
    S.accumM += S.ride.distM;
    S.ride = null;
    live.speed = 0;
    live.avg = 0;
    sendCommand([BLE.CMD_END]);
    save();
    releaseWake();
    showScreen('goal');
    render();
    toast('Percurso salvo no acumulado');
  }

  async function resetAccum() {
    const ok = await confirmSheet({
      title: 'Zerar acumulado',
      text: 'Isso apaga o total de KM acumulados deste aparelho. Não dá para desfazer.',
      ok: 'Zerar',
    });
    if (!ok) return;
    S.accumM = 0;
    save();
    render();
    toast('KM acumulados zerados');
  }

  /* ---------------- simulação ---------------- */
  let simTarget = 20;
  let simNext = 0;
  function simStep(dt, scale) {
    simNext -= dt;
    if (simNext <= 0) {
      simTarget = Math.random() < 0.1 ? 0 : 12 + Math.random() * 20;
      simNext = 3 + Math.random() * 6;
    }
    live.speed += (simTarget - live.speed) * Math.min(1, dt * 0.8);
    if (simTarget === 0 && live.speed < 0.5) live.speed = 0;
    const sdt = dt * scale;
    S.ride.distM += (live.speed / 3.6) * sdt;
    if (live.speed > 0) S.ride.movingS += sdt;
    S.ride.maxKmh = Math.max(S.ride.maxKmh, live.speed);
    live.avg = S.ride.movingS > 0 ? (S.ride.distM / S.ride.movingS) * 3.6 : 0;
  }

  let last = performance.now();
  let lastSave = 0;
  function tick() {
    const now = performance.now();
    const dt = Math.min(1, (now - last) / 1000);
    last = now;

    if (S.mode === 'sim') {
      live.battery = S.simBat;
      if (S.ride) simStep(dt, S.simRate);
      else { live.speed = 0; live.avg = 0; }
    } else if (!live.connected) {
      live.speed = 0;
    }

    if (S.ride && !S.ride.goalHit && S.ride.distM >= S.ride.goalKm * 1000) {
      S.ride.goalHit = true;
      toast('Meta batida!');
      if (navigator.vibrate) navigator.vibrate([120, 60, 120]);
      save();
    }

    render();
    if (S.ride && now - lastSave > 5000) { save(); lastSave = now; }
  }

  /* ---------------- Bluetooth (Web Bluetooth) ---------------- */
  const ble = { device: null, cmd: null };
  let bleStartSentAt = 0;
  const u32 = (n) => [n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >>> 24) & 255];
  const u16 = (n) => [n & 255, (n >> 8) & 255];

  async function sendCommand(bytes) {
    if (S.mode === 'usb') {
      if (!usb.writer) return;
      try { await usb.writer.write(new TextEncoder().encode('C,' + bytes.join(',') + '\n')); }
      catch (e) { console.warn('Falha ao enviar comando (USB)', e); }
      return;
    }
    if (S.mode === 'wifi') {
      if (!live.connected) return;
      try { await fetch('/api/cmd?b=' + bytes.join(','), { method: 'POST' }); }
      catch (e) { console.warn('Falha ao enviar comando (Wi-Fi)', e); }
      return;
    }
    if (!ble.cmd) return;
    const data = Uint8Array.from(bytes);
    try {
      if (ble.cmd.writeValueWithResponse) await ble.cmd.writeValueWithResponse(data);
      else await ble.cmd.writeValue(data);
    } catch (e) { console.warn('Falha ao enviar comando', e); }
  }

  /* ---------------- USB (cabo no computador, Web Serial) ----------------
     Linhas de texto a 115200 baud. Arduino -> app:  T,vel*10,media*10,max*10,metros,bateria,flags
     App -> Arduino:  C,byte,byte,...   (mesmos comandos do Bluetooth)                          */
  const usb = { port: null, reader: null, writer: null, on: false };
  function usbLine(line) {
    const p = line.trim().split(',');
    if (p[0] !== 'T' || p.length < 7) return;
    const n = p.slice(1).map((x) => parseInt(x, 10) || 0);
    const dv = new DataView(new ArrayBuffer(12));
    dv.setUint16(0, n[0], true); dv.setUint16(2, n[1], true); dv.setUint16(4, n[2], true);
    dv.setUint32(6, n[3] >>> 0, true); dv.setUint8(10, n[4]); dv.setUint8(11, n[5]);
    if (!live.connected) {
      live.connected = true;
      updateConn();
      toast('Arduino conectado (USB)');
      sendCommand([BLE.CMD_WHEEL, ...u16(S.wheelMm)]);
    }
    usb.last = Date.now();
    onTelemetry({ target: { value: dv } });
  }
  async function usbOpen(port) {
    await port.open({ baudRate: 115200 });
    usb.port = port;
    usb.on = true;
    usb.writer = port.writable.getWriter();
    S.mode = 'usb';
    live.connected = false;
    save();
    updateConn();
    (async () => {
      const dec = new TextDecoder();
      let buf = '';
      try {
        while (usb.on && port.readable) {
          usb.reader = port.readable.getReader();
          try {
            for (;;) {
              const { value, done } = await usb.reader.read();
              if (done) break;
              buf += dec.decode(value, { stream: true });
              let i;
              while ((i = buf.indexOf('\n')) >= 0) { usbLine(buf.slice(0, i)); buf = buf.slice(i + 1); }
              if (buf.length > 400) buf = '';
            }
          } finally { usb.reader.releaseLock(); }
        }
      } catch (e) { console.debug('usb', e && e.message); }
      if (usb.on) { await usbClose(); toast('Arduino desconectado (cabo)'); }
    })();
  }
  async function usbClose() {
    usb.on = false;
    try { if (usb.reader) await usb.reader.cancel(); } catch (_) { /* ok */ }
    try { if (usb.writer) { usb.writer.releaseLock(); } } catch (_) { /* ok */ }
    try { if (usb.port) await usb.port.close(); } catch (_) { /* ok */ }
    usb.port = usb.reader = usb.writer = null;
    live.connected = false;
    updateConn();
  }
  async function usbConnect() {
    if (!navigator.serial) {
      toast('Este navegador não tem USB Serial. Use o Chrome ou Edge no computador.');
      return;
    }
    wifiStop();
    bleDisconnect();
    try {
      if (usb.port) await usbClose();
      await usbOpen(await navigator.serial.requestPort());
      closeSheets();
    } catch (e) {
      if (e && e.name === 'NotFoundError') return;
      console.warn(e);
      toast('Não consegui abrir a porta USB. Feche o Monitor Serial do Arduino IDE e tente de novo.');
    }
  }
  async function usbAuto() {
    if (!navigator.serial || S.mode !== 'usb') return;
    try {
      const ports = await navigator.serial.getPorts();
      if (ports[0]) await usbOpen(ports[0]);
    } catch (e) { console.debug('usb auto', e && e.message); }
  }

  /* ---------------- Wi-Fi (leitura do ESP32 por HTTP) ---------------- */
  const wifi = { on: false, fails: 0, timer: 0 };
  function wifiStop() {
    wifi.on = false;
    clearTimeout(wifi.timer);
  }
  async function wifiPoll() {
    if (!wifi.on) return;
    const ctl = new AbortController();
    const to = setTimeout(() => ctl.abort(), 1500);
    try {
      const r = await fetch('/api/t', { cache: 'no-store', signal: ctl.signal });
      if (!r.ok) throw new Error('HTTP ' + r.status);
      const buf = await r.arrayBuffer();
      if (!wifi.on) return;
      if (!live.connected) {
        live.connected = true;
        updateConn();
        toast('ESP32 conectado (Wi-Fi)');
        sendCommand([BLE.CMD_WHEEL, ...u16(S.wheelMm)]);
      }
      wifi.fails = 0;
      onTelemetry({ target: { value: new DataView(buf) } });
    } catch (e) {
      console.debug('wifi', e && e.message);
      if (++wifi.fails === 4 && live.connected) {
        live.connected = false;
        updateConn();
        toast('ESP32 sem resposta no Wi-Fi');
      }
    } finally {
      clearTimeout(to);
      if (wifi.on) wifi.timer = setTimeout(wifiPoll, wifi.fails >= 4 ? 1000 : 250);
    }
  }
  async function wifiConnect(quiet) {
    if (!WIFI_OK) {
      // página https: abre o app servido pelo próprio ESP32 (mesmo endereço, sem bloqueio do navegador)
      const host = ($('wifiHost').value || S.wifiHost || DEFAULTS.wifiHost).trim().toLowerCase().replace(/[^a-z0-9.:-]/g, '');
      if (!host) return;
      S.wifiHost = host;
      save();
      location.href = 'http://' + host + '/';
      return;
    }
    bleDisconnect();
    wifiStop();
    if (usb.on) usbClose();
    try {
      const r = await fetch('/api/t', { cache: 'no-store' });
      if (!r.ok) throw new Error('HTTP ' + r.status);
    } catch (e) {
      if (!quiet) toast('Não achei o ESP32 neste endereço. Confira se o celular está no mesmo Wi-Fi.');
      return;
    }
    S.mode = 'wifi';
    wifi.on = true;
    wifi.fails = 0;
    live.connected = false;   // o primeiro retorno do poll marca como conectado
    save();
    updateConn();
    closeSheets();
    wifiPoll();
  }
  function onTelemetry(ev) {
    const dv = ev.target.value;
    if (!dv || dv.byteLength < 12) return;
    live.speed = dv.getUint16(0, true) / 10;
    live.avg = dv.getUint16(2, true) / 10;
    const max = dv.getUint16(4, true) / 10;
    const dist = dv.getUint32(6, true);
    live.battery = dv.getUint8(10);
    const espRunning = (dv.getUint8(11) & 2) !== 0;
    if (!S.ride) return;
    if (espRunning) {
      // distância = o que o app já tinha antes de (re)iniciar o ESP32 + o que ele contou
      S.ride.distM = (S.ride.baseM || 0) + dist;
      S.ride.maxKmh = Math.max(S.ride.maxKmh, max);
    } else if (Date.now() - bleStartSentAt > 3000) {
      // o ESP32 reiniciou ou perdeu o início: retoma contando a partir da distância já feita
      bleStartSentAt = Date.now();
      S.ride.baseM = S.ride.distM;
      sendCommand([BLE.CMD_START, ...u32(S.ride.goalKm * 1000)]);
    }
  }
  function onBleDisconnected() {
    live.connected = false;
    ble.cmd = null;
    updateConn();
    toast('ESP32 desconectado');
  }

  async function bleConnect() {
    if (!navigator.bluetooth) {
      toast(window.isSecureContext
        ? 'Este navegador não tem Bluetooth Web. No Android, use o Chrome.'
        : 'Bluetooth só funciona na versão https do app. Aqui use o Wi-Fi.');
      return;
    }
    wifiStop();
    if (usb.on) usbClose();
    let step = 'busca';
    try {
      const device = await navigator.bluetooth.requestDevice({
        filters: [{ services: [BLE.SERVICE] }, { namePrefix: BLE.NAME_PREFIX }],
        optionalServices: [BLE.SERVICE],
      });
      device.addEventListener('gattserverdisconnected', onBleDisconnected);
      step = 'conexão';
      let server;
      try { server = await device.gatt.connect(); } catch (_) {
        await new Promise((r) => setTimeout(r, 800));   // uma segunda tentativa resolve a maioria das falhas
        server = await device.gatt.connect();
      }
      step = 'serviço';
      const service = await server.getPrimaryService(BLE.SERVICE);
      step = 'telemetria';
      const tel = await service.getCharacteristic(BLE.TELEMETRY);
      step = 'comando';
      const cmd = await service.getCharacteristic(BLE.COMMAND);
      step = 'notificações';
      tel.addEventListener('characteristicvaluechanged', onTelemetry);
      await tel.startNotifications();
      ble.device = device;
      ble.cmd = cmd;
      live.connected = true;
      S.mode = 'ble';
      save();
      await sendCommand([BLE.CMD_WHEEL, ...u16(S.wheelMm)]);
      updateConn();
      toast('ESP32 conectado');
      closeSheets();
    } catch (e) {
      if (e && e.name === 'NotFoundError' && step === 'busca') return; // pessoa cancelou a escolha
      console.warn(e);
      const dica = step === 'conexão'
        ? 'Desligue e ligue o Bluetooth do celular e reinicie o ESP32.'
        : 'Confira os UUIDs do firmware.';
      toast('Falhou na etapa "' + step + '" (' + ((e && e.name) || 'erro') + '). ' + dica);
    }
  }

  function bleDisconnect() {
    try { if (ble.device && ble.device.gatt.connected) ble.device.gatt.disconnect(); } catch (_) { /* ok */ }
    ble.device = null;
    ble.cmd = null;
    live.connected = false;
  }

  function useSimulation() {
    usbClose();
    wifiStop();
    bleDisconnect();
    S.mode = 'sim';
    save();
    updateConn();
    closeSheets();
  }

  function updateConn() {
    const btn = $('connBtn');
    let state, text, detail;
    if (S.mode === 'sim') {
      state = 'sim'; text = 'Simulação';
      detail = 'Modo simulação: os dados são gerados no próprio app. Use para testar o visual.';
    } else if (S.mode === 'wifi') {
      state = live.connected ? 'ble' : 'off';
      text = live.connected ? 'ESP32 · Wi-Fi' : 'ESP32 sem Wi-Fi';
      detail = live.connected
        ? 'Recebendo dados do ESP32 pelo Wi-Fi (celular e ESP32 na mesma rede).'
        : 'Sem resposta do ESP32. Confira se o celular está no mesmo Wi-Fi que ele.';
    } else if (S.mode === 'usb') {
      state = live.connected ? 'ble' : 'off';
      text = live.connected ? 'Arduino · USB' : 'Arduino sem dados';
      detail = live.connected
        ? 'Recebendo dados do Arduino pelo cabo USB.'
        : 'Aguardando dados do Arduino. Confira o cabo e se o Monitor Serial do IDE está fechado.';
    } else if (live.connected) {
      state = 'ble'; text = 'ESP32 conectado';
      detail = 'Recebendo dados do ESP32 por Bluetooth.';
    } else {
      state = 'off'; text = 'ESP32 desconectado';
      detail = 'Toque em conectar para procurar o ESP32 (nome que começa com "BikeSpeed").';
    }
    btn.dataset.state = state;
    $('connText').textContent = text;
    $('connDetail').textContent = detail;
    $('simOnly').hidden = S.mode !== 'sim';
    $('wifiHostField').hidden = WIFI_OK;
    $('panelLink').hidden = !WIFI_OK;
    $('wifiBtn').textContent = WIFI_OK ? 'Conectar pelo Wi-Fi' : 'Abrir pelo Wi-Fi do ESP32';
  }

  /* ---------------- ligações da interface ---------------- */
  function holdRepeat(btn, fn) {
    let t, iv;
    const stop = () => { clearTimeout(t); clearInterval(iv); };
    btn.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      fn();
      t = setTimeout(() => { iv = setInterval(fn, 110); }, 420);
    });
    ['pointerup', 'pointerleave', 'pointercancel'].forEach((ev) => btn.addEventListener(ev, stop));
    btn.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); fn(); }
    });
  }
  function changeGoal(delta) {
    S.goalKm = Math.min(300, Math.max(1, S.goalKm + delta));
    save();
    render();
  }

  function init() {
    // metas rápidas
    const presets = $('presets');
    [10, 15, 21, 30].forEach((km) => {
      const b = document.createElement('button');
      b.className = 'preset';
      b.dataset.km = km;
      b.setAttribute('aria-pressed', 'false');
      b.innerHTML = `${km}<small>km</small>`;
      b.addEventListener('click', () => { S.goalKm = km; save(); render(); });
      presets.appendChild(b);
    });

    // tipo de bike
    const grid = $('bikeGrid');
    BIKES.forEach((b) => {
      const el = document.createElement('button');
      el.className = 'bikeopt';
      el.dataset.id = b.id;
      el.setAttribute('role', 'radio');
      el.setAttribute('aria-checked', 'false');
      el.innerHTML = `<span class="mm">${b.mm} mm</span><span class="nm">${b.num}${b.unit ? `<small>${b.unit}</small>` : ''}</span>`;
      el.addEventListener('click', () => { draft = { id: b.id, mm: b.mm }; $('customMm').value = ''; renderBike(); });
      grid.appendChild(el);
    });
    $('customMm').addEventListener('input', (e) => {
      const v = Math.round(+e.target.value);
      if (Number.isFinite(v) && v >= 1000 && v <= 3000) { draft = { id: 'custom', mm: v }; renderBike(); }
    });
    $('bikeBtn').addEventListener('click', openBike);
    $('backBtn').addEventListener('click', () => { showScreen('goal'); render(); });
    $('bikeOk').addEventListener('click', applyBike);

    holdRepeat($('goalMinus'), () => changeGoal(-1));
    holdRepeat($('goalPlus'), () => changeGoal(1));
    $('startBtn').addEventListener('click', startRide);
    $('finishBtn').addEventListener('click', finishRide);
    $('resetBtn').addEventListener('click', resetAccum);

    // confirmação
    $('confirmOk').addEventListener('click', () => answerConfirm(true));
    $('confirmCancel').addEventListener('click', () => answerConfirm(false));

    // ajustes
    $('connBtn').addEventListener('click', () => { updateConn(); openSheet('settingsSheet'); });
    $('settingsClose').addEventListener('click', closeSheets);
    $('scrim').addEventListener('click', closeSheets);
    document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeSheets(); });
    $('bleBtn').addEventListener('click', bleConnect);
    $('usbBtn').addEventListener('click', usbConnect);
    $('wifiHost').value = S.wifiHost;
    $('wifiBtn').addEventListener('click', () => wifiConnect(false));
    $('simBtn').addEventListener('click', useSimulation);

    $('wheelInput').value = S.wheelMm;
    $('wheelInput').addEventListener('change', (e) => {
      S.wheelMm = Math.round(num(e.target.value, 1000, 3000, S.wheelMm));
      const m = BIKES.find((b) => b.mm === S.wheelMm);
      S.bike = m ? m.id : 'custom';
      e.target.value = S.wheelMm;
      save();
      sendCommand([BLE.CMD_WHEEL, ...u16(S.wheelMm)]);
    });

    $('simBat').value = S.simBat;
    $('simBatLabel').textContent = S.simBat;
    $('simBat').addEventListener('input', (e) => {
      S.simBat = Math.round(num(e.target.value, 0, 100, 100));
      $('simBatLabel').textContent = S.simBat;
      save();
    });
    $('simSpeed').value = String(S.simRate);
    $('simSpeed').addEventListener('change', (e) => { S.simRate = +e.target.value; save(); });

    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'visible') { last = performance.now(); requestWake(); }
      else save();
    });
    window.addEventListener('pagehide', save);

    $('buildTag').textContent = 'build ' + (self.BUILD || '?');

    showScreen(S.ride ? 'ride' : 'goal');
    updateConn();
    render();
    if (S.ride) requestWake();
    setInterval(tick, 250);
    if (WIFI_OK) wifiConnect(true);   // aberto pelo endereço do ESP32: liga o Wi-Fi sozinho

    usbAuto();

    if ('serviceWorker' in navigator && /^https?:$/.test(location.protocol)) {
      navigator.serviceWorker.register('sw.js').catch((e) => console.warn('SW', e));
    }
  }

  init();
})();
