#pragma once
// Auto-generated embedded HTML for HFT WebUI dashboard.
// Single-page app: vanilla JS + fetch polling, no framework dependencies.

static const char EMBEDDED_HTML[] = R"html(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HFT Monitor</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Consolas','Courier New',monospace;font-size:13px;background:#0d1117;color:#c9d1d9;overflow-x:hidden}
a{color:#58a6ff}
::-webkit-scrollbar{width:6px;height:6px}
::-webkit-scrollbar-thumb{background:#30363d;border-radius:3px}

.header{display:flex;align-items:center;justify-content:space-between;padding:8px 16px;background:#161b22;border-bottom:1px solid #30363d}
.header h1{font-size:15px;color:#58a6ff;font-weight:600}
.header .indicators{display:flex;gap:16px;font-size:12px}
.ind{display:flex;align-items:center;gap:4px}
.dot{width:8px;height:8px;border-radius:50%;display:inline-block}
.dot-green{background:#3fb950}
.dot-red{background:#f85149}
.dot-yellow{background:#d29922}
.dot-gray{background:#484f58}

.grid{display:grid;grid-template-columns:1fr 1fr 1fr;grid-template-rows:auto 1fr auto;gap:8px;padding:8px;height:calc(100vh - 44px)}
.panel{background:#161b22;border:1px solid #30363d;border-radius:6px;overflow:hidden;display:flex;flex-direction:column}
.panel-title{font-size:12px;font-weight:600;color:#8b949e;text-transform:uppercase;letter-spacing:0.5px;padding:8px 12px;border-bottom:1px solid #21262d;background:#0d1117}
.panel-body{flex:1;overflow:auto;padding:8px 12px}

table{width:100%;border-collapse:collapse}
th{text-align:left;color:#8b949e;font-weight:500;font-size:11px;padding:4px 6px;border-bottom:1px solid #21262d;position:sticky;top:0;background:#161b22}
td{padding:4px 6px;border-bottom:1px solid #21262d;white-space:nowrap}
tr:hover td{background:#1c2128}

.val-pos{color:#3fb950}
.val-neg{color:#f85149}
.val-zero{color:#8b949e}

.risk-normal{color:#3fb950}
.risk-warning{color:#d29922}
.risk-danger{color:#f85149}

.latency-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.lat-card{text-align:center;padding:8px;background:#0d1117;border-radius:4px}
.lat-card .label{font-size:11px;color:#8b949e;margin-bottom:4px}
.lat-card .value{font-size:18px;font-weight:700;color:#58a6ff}
.lat-card .unit{font-size:11px;color:#8b949e}

.alert-item{padding:4px 0;border-bottom:1px solid #21262d;font-size:12px;color:#d29922}
.chart-container{width:100%;height:100%;min-height:150px}

.span2{grid-column:span 2}
</style>
</head>
<body>

<div class="header">
  <h1>HFT Trading Monitor</h1>
  <div class="indicators">
    <span class="ind"><span id="dot-engine" class="dot dot-gray"></span> Engine</span>
    <span class="ind"><span id="dot-trading" class="dot dot-gray"></span> Trading</span>
    <span class="ind" id="risk-ind"><span id="dot-risk" class="dot dot-green"></span> Risk: <span id="risk-mode">--</span></span>
    <span class="ind">Uptime: <span id="uptime">--</span></span>
    <span class="ind" id="clock"></span>
  </div>
</div>

<div class="grid">
  <!-- Row 1: Accounts + Strategies -->
  <div class="panel">
    <div class="panel-title">Accounts</div>
    <div class="panel-body"><table><thead><tr><th>Account</th><th>Balance</th><th>Available</th><th>Margin</th><th>PnL</th></tr></thead><tbody id="tb-accounts"></tbody></table></div>
  </div>
  <div class="panel span2">
    <div class="panel-title">Strategies</div>
    <div class="panel-body"><table><thead><tr><th>ID</th><th>Type</th><th>Status</th><th>Trades</th><th>Win%</th><th>PnL</th><th>Signal</th></tr></thead><tbody id="tb-strategies"></tbody></table></div>
  </div>

  <!-- Row 2: Ticks + PnL Chart + Orders/Trades -->
  <div class="panel">
    <div class="panel-title">Market Data</div>
    <div class="panel-body"><table><thead><tr><th>Instrument</th><th>Last</th><th>Bid</th><th>Ask</th><th>Vol</th><th>Time</th></tr></thead><tbody id="tb-ticks"></tbody></table></div>
  </div>
  <div class="panel">
    <div class="panel-title">P&L Curve</div>
    <div class="panel-body"><canvas id="pnl-chart" class="chart-container"></canvas></div>
  </div>
  <div class="panel">
    <div class="panel-title">Recent Trades</div>
    <div class="panel-body"><table><thead><tr><th>Time</th><th>Instr</th><th>Dir</th><th>Price</th><th>Vol</th></tr></thead><tbody id="tb-trades"></tbody></table></div>
  </div>

  <!-- Row 3: Latency + Orders + Alerts -->
  <div class="panel">
    <div class="panel-title">Latency</div>
    <div class="panel-body">
      <div class="latency-grid">
        <div class="lat-card"><div class="label">Tick-to-Signal</div><div class="value" id="lat-t2s">--</div><div class="unit">us</div></div>
        <div class="lat-card"><div class="label">Signal-to-Order</div><div class="value" id="lat-s2o">--</div><div class="unit">us</div></div>
        <div class="lat-card"><div class="label">Order-to-Trade</div><div class="value" id="lat-o2t">--</div><div class="unit">us</div></div>
        <div class="lat-card"><div class="label">Tick Process</div><div class="value" id="lat-tp">--</div><div class="unit">us</div></div>
        <div class="lat-card"><div class="label">Order Process</div><div class="value" id="lat-op">--</div><div class="unit">us</div></div>
        <div class="lat-card"><div class="label">Trade Process</div><div class="value" id="lat-trp">--</div><div class="unit">us</div></div>
      </div>
    </div>
  </div>
  <div class="panel">
    <div class="panel-title">Recent Orders</div>
    <div class="panel-body"><table><thead><tr><th>Time</th><th>Instr</th><th>Dir</th><th>Price</th><th>Vol</th><th>Status</th></tr></thead><tbody id="tb-orders"></tbody></table></div>
  </div>
  <div class="panel">
    <div class="panel-title">Alerts</div>
    <div class="panel-body"><div id="alert-list"></div></div>
  </div>
</div>

<script>
const $ = id => document.getElementById(id);

function pnlClass(v) { return v > 0.001 ? 'val-pos' : v < -0.001 ? 'val-neg' : 'val-zero'; }
function fmtNum(v, d) { return v == null ? '--' : Number(v).toFixed(d !== undefined ? d : 2); }
function fmtLat(v) { return v < 0 ? '--' : v.toString(); }
function riskClass(m) {
  if (!m) return 'risk-normal';
  if (m === 'normal') return 'risk-normal';
  if (m === 'warning') return 'risk-warning';
  return 'risk-danger';
}

async function fetchJson(url) {
  try { const r = await fetch(url); return r.ok ? await r.json() : null; }
  catch { return null; }
}

async function refresh() {
  const [status, accounts, strategies, latency, pnl, ticks, orders, trades, alerts] = await Promise.all([
    fetchJson('/api/status'),
    fetchJson('/api/accounts'),
    fetchJson('/api/strategies'),
    fetchJson('/api/latency'),
    fetchJson('/api/pnl'),
    fetchJson('/api/ticks'),
    fetchJson('/api/orders'),
    fetchJson('/api/trades'),
    fetchJson('/api/alerts')
  ]);

  // Status bar
  if (status) {
    $('dot-engine').className = 'dot ' + (status.running ? 'dot-green' : 'dot-red');
    $('dot-trading').className = 'dot ' + (status.trading_ready ? 'dot-green' : 'dot-red');
    const rm = status.risk_mode || 'unknown';
    $('risk-mode').textContent = rm;
    $('risk-mode').className = riskClass(rm);
    $('dot-risk').className = 'dot ' + (rm === 'normal' ? 'dot-green' : rm === 'warning' ? 'dot-yellow' : 'dot-red');
    if (status.uptime_seconds != null) {
      const s = status.uptime_seconds;
      const h = Math.floor(s/3600), m = Math.floor((s%3600)/60), sec = s%60;
      $('uptime').textContent = `${h}h ${m}m ${sec}s`;
    }
  }

  // Accounts
  if (accounts) {
    $('tb-accounts').innerHTML = accounts.map(a =>
      `<tr><td>${a.account_id||'default'}</td><td>${fmtNum(a.balance)}</td><td>${fmtNum(a.available)}</td><td>${fmtNum(a.margin)}</td><td class="${pnlClass(a.position_profit)}">${fmtNum(a.position_profit)}</td></tr>`
    ).join('');
  }

  // Strategies
  if (strategies) {
    $('tb-strategies').innerHTML = strategies.map(s =>
      `<tr><td>${s.strategy_id}</td><td>${s.strategy_type||''}</td><td>${s.status||''}</td><td>${s.trade_count||0}</td><td>${fmtNum(s.win_rate,1)}</td><td class="${pnlClass(s.total_pnl)}">${fmtNum(s.total_pnl)}</td><td>${s.last_signal||''}</td></tr>`
    ).join('');
  }

  // Ticks
  if (ticks) {
    $('tb-ticks').innerHTML = ticks.map(t =>
      `<tr><td>${t.instrument_id}</td><td>${fmtNum(t.last_price)}</td><td>${fmtNum(t.bid_price1)}</td><td>${fmtNum(t.ask_price1)}</td><td>${t.volume||0}</td><td>${t.update_time||''}</td></tr>`
    ).join('');
  }

  // Latency
  if (latency) {
    $('lat-t2s').textContent = fmtLat(latency.tick_to_signal_us);
    $('lat-s2o').textContent = fmtLat(latency.signal_to_order_us);
    $('lat-o2t').textContent = fmtLat(latency.order_to_trade_us);
    $('lat-tp').textContent = fmtLat(latency.tick_process_us);
    $('lat-op').textContent = fmtLat(latency.order_process_us);
    $('lat-trp').textContent = fmtLat(latency.trade_process_us);
  }

  // Orders
  if (orders) {
    $('tb-orders').innerHTML = orders.slice(0,30).map(o =>
      `<tr><td>${o.insert_time||''}</td><td>${o.instrument_id}</td><td>${o.direction}</td><td>${fmtNum(o.price)}</td><td>${o.total_volume||0}</td><td>${o.status||''}</td></tr>`
    ).join('');
  }

  // Trades
  if (trades) {
    $('tb-trades').innerHTML = trades.slice(0,30).map(t =>
      `<tr><td>${t.trade_time||''}</td><td>${t.instrument_id}</td><td>${t.direction}</td><td>${fmtNum(t.price)}</td><td>${t.volume||0}</td></tr>`
    ).join('');
  }

  // Alerts
  if (alerts) {
    $('alert-list').innerHTML = alerts.slice(0,50).map(a =>
      `<div class="alert-item">${typeof a === 'string' ? a : JSON.stringify(a)}</div>`
    ).join('');
  }

  // PnL chart
  if (pnl && pnl.length > 1) drawPnlChart(pnl);
}

function drawPnlChart(data) {
  const canvas = $('pnl-chart');
  const rect = canvas.parentElement.getBoundingClientRect();
  canvas.width = rect.width - 24;
  canvas.height = rect.height - 16;
  const ctx = canvas.getContext('2d');
  const W = canvas.width, H = canvas.height;
  const pad = {t:20,r:10,b:30,l:60};
  const cw = W-pad.l-pad.r, ch = H-pad.t-pad.b;

  ctx.clearRect(0,0,W,H);

  const vals = data.map(d => d.total_pnl);
  let mn = Math.min(...vals), mx = Math.max(...vals);
  if (mn === mx) { mn -= 1; mx += 1; }
  const range = mx - mn;

  // Grid
  ctx.strokeStyle = '#21262d';
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = pad.t + ch * i / 4;
    ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(W-pad.r, y); ctx.stroke();
    ctx.fillStyle = '#8b949e'; ctx.font = '10px monospace';
    ctx.textAlign = 'right';
    ctx.fillText(fmtNum(mx - range*i/4, 0), pad.l-4, y+3);
  }

  // Zero line
  if (mn < 0 && mx > 0) {
    const zy = pad.t + ch * (mx / range);
    ctx.strokeStyle = '#484f58'; ctx.setLineDash([4,4]);
    ctx.beginPath(); ctx.moveTo(pad.l, zy); ctx.lineTo(W-pad.r, zy); ctx.stroke();
    ctx.setLineDash([]);
  }

  // Line
  ctx.strokeStyle = '#58a6ff'; ctx.lineWidth = 1.5;
  ctx.beginPath();
  for (let i = 0; i < data.length; i++) {
    const x = pad.l + cw * i / (data.length-1);
    const y = pad.t + ch * (1 - (vals[i]-mn)/range);
    i === 0 ? ctx.moveTo(x,y) : ctx.lineTo(x,y);
  }
  ctx.stroke();

  // Fill
  const last = data.length-1;
  ctx.lineTo(pad.l + cw, pad.t+ch);
  ctx.lineTo(pad.l, pad.t+ch);
  ctx.closePath();
  ctx.fillStyle = 'rgba(88,166,255,0.08)';
  ctx.fill();

  // Time labels
  ctx.fillStyle = '#8b949e'; ctx.font = '10px monospace'; ctx.textAlign = 'center';
  const step = Math.max(1, Math.floor(data.length/5));
  for (let i = 0; i < data.length; i += step) {
    const x = pad.l + cw * i / (data.length-1);
    const t = data[i].time || '';
    ctx.fillText(t.substring(0,8), x, H-pad.b+14);
  }
}

// Clock
function updateClock() {
  const now = new Date();
  $('clock').textContent = now.toLocaleTimeString('zh-CN',{hour12:false});
}

updateClock();
setInterval(updateClock, 1000);
refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>)html";
