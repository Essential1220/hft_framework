#pragma once
// Auto-generated embedded HTML for HFT WebUI dashboard.
// Single-page app: vanilla JS + fetch polling, no framework dependencies.

static const char EMBEDDED_HTML[] = R"html(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>HFT 交易监控</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:'Microsoft YaHei','PingFang SC','Consolas',monospace;font-size:13px;background:#0d1117;color:#c9d1d9;overflow-x:hidden}
a{color:#58a6ff}
::-webkit-scrollbar{width:6px;height:6px}
::-webkit-scrollbar-thumb{background:#30363d;border-radius:3px}

.header{display:flex;align-items:center;justify-content:space-between;padding:8px 16px;background:#161b22;border-bottom:1px solid #30363d}
.header h1{font-size:15px;color:#58a6ff;font-weight:600}
.header .indicators{display:flex;gap:16px;font-size:12px;align-items:center}
.ind{display:flex;align-items:center;gap:4px}
.dot{width:8px;height:8px;border-radius:50%;display:inline-block}
.dot-green{background:#3fb950}
.dot-red{background:#f85149}
.dot-yellow{background:#d29922}
.dot-gray{background:#484f58}
.header-pnl{font-size:14px;font-weight:700;padding:2px 10px;border-radius:4px;background:#0d1117}

.grid{display:grid;grid-template-columns:1fr 1fr 1fr;grid-template-rows:auto auto 1fr auto;gap:8px;padding:8px;height:calc(100vh - 44px)}
.panel{background:#161b22;border:1px solid #30363d;border-radius:6px;overflow:hidden;display:flex;flex-direction:column}
.panel-title{font-size:12px;font-weight:600;color:#8b949e;letter-spacing:0.5px;padding:8px 12px;border-bottom:1px solid #21262d;background:#0d1117;display:flex;justify-content:space-between;align-items:center}
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
.lat-card .value{font-size:18px;font-weight:700}
.lat-card .unit{font-size:11px;color:#8b949e}
.lat-green{color:#3fb950}
.lat-yellow{color:#d29922}
.lat-red{color:#f85149}

.alert-item{padding:4px 0;border-bottom:1px solid #21262d;font-size:12px}
.alert-warn{color:#d29922}
.alert-info{color:#58a6ff}
.alert-error{color:#f85149}
.chart-container{width:100%;height:100%;min-height:150px}

.span2{grid-column:span 2}
.span3{grid-column:span 3}

.dir-buy{color:#f85149}
.dir-sell{color:#3fb950}
.status-running{color:#3fb950}
.status-paused{color:#d29922}
.status-stopped{color:#f85149}

.btn{border:none;padding:2px 8px;border-radius:3px;font-size:11px;cursor:pointer;font-family:inherit}
.btn-cancel{background:#30363d;color:#f85149;border:1px solid #f8514933}
.btn-cancel:hover{background:#f8514922}
.btn-cancel-all{background:#21262d;color:#d29922;border:1px solid #d2992233;padding:3px 10px;font-size:11px}
.btn-cancel-all:hover{background:#d2992222}

.instr-name{color:#8b949e;font-size:11px;margin-left:4px}
</style>
</head>
<body>

<div class="header">
  <h1>HFT 交易监控</h1>
  <div class="indicators">
    <span class="ind"><span id="dot-engine" class="dot dot-gray"></span> 引擎</span>
    <span class="ind"><span id="dot-trading" class="dot dot-gray"></span> 交易</span>
    <span class="ind"><span id="dot-risk" class="dot dot-green"></span> 风控: <span id="risk-mode" class="risk-normal">--</span></span>
    <span class="ind">今日盈亏: <span id="header-pnl" class="header-pnl val-zero">--</span></span>
    <span class="ind">运行: <span id="uptime">--</span></span>
    <span class="ind" id="clock"></span>
  </div>
</div>

<div class="grid">
  <div class="panel">
    <div class="panel-title">账户资金</div>
    <div class="panel-body"><table><thead><tr><th>账户</th><th>权益</th><th>可用</th><th>保证金</th><th>手续费</th><th>浮盈</th></tr></thead><tbody id="tb-accounts"></tbody></table></div>
  </div>
  <div class="panel span2">
    <div class="panel-title">当前持仓</div>
    <div class="panel-body"><table><thead><tr><th>合约</th><th>方向</th><th>手数</th><th>均价</th><th>最新价</th><th>浮盈</th><th>保证金</th></tr></thead><tbody id="tb-positions"></tbody></table></div>
  </div>

  <div class="panel span3">
    <div class="panel-title">策略状态</div>
    <div class="panel-body"><table><thead><tr><th>策略</th><th>类型</th><th>状态</th><th>成交数</th><th>胜率</th><th>盈亏</th><th>最新信号</th><th>信号时间</th></tr></thead><tbody id="tb-strategies"></tbody></table></div>
  </div>

  <div class="panel">
    <div class="panel-title">实时行情</div>
    <div class="panel-body"><table><thead><tr><th>合约</th><th>最新</th><th>买一</th><th>卖一</th><th>成交量</th><th>时间</th></tr></thead><tbody id="tb-ticks"></tbody></table></div>
  </div>
  <div class="panel">
    <div class="panel-title">资金曲线</div>
    <div class="panel-body"><canvas id="pnl-chart" class="chart-container"></canvas></div>
  </div>
  <div class="panel">
    <div class="panel-title">最近成交</div>
    <div class="panel-body"><table><thead><tr><th>时间</th><th>合约</th><th>方向</th><th>价格</th><th>手数</th></tr></thead><tbody id="tb-trades"></tbody></table></div>
  </div>

  <div class="panel">
    <div class="panel-title">延迟监控</div>
    <div class="panel-body">
      <div class="latency-grid">
        <div class="lat-card"><div class="label">行情到信号</div><div class="value lat-green" id="lat-t2s">--</div><div class="unit">微秒</div></div>
        <div class="lat-card"><div class="label">信号到报单</div><div class="value lat-green" id="lat-s2o">--</div><div class="unit">微秒</div></div>
        <div class="lat-card"><div class="label">报单到成交</div><div class="value lat-green" id="lat-o2t">--</div><div class="unit">微秒</div></div>
        <div class="lat-card"><div class="label">行情处理</div><div class="value lat-green" id="lat-tp">--</div><div class="unit">微秒</div></div>
        <div class="lat-card"><div class="label">委托处理</div><div class="value lat-green" id="lat-op">--</div><div class="unit">微秒</div></div>
        <div class="lat-card"><div class="label">成交处理</div><div class="value lat-green" id="lat-trp">--</div><div class="unit">微秒</div></div>
      </div>
    </div>
  </div>
  <div class="panel">
    <div class="panel-title">最近委托 <button class="btn btn-cancel-all" onclick="cancelAll()">一键全撤</button></div>
    <div class="panel-body"><table><thead><tr><th>时间</th><th>合约</th><th>方向</th><th>价格</th><th>手数</th><th>状态</th><th></th></tr></thead><tbody id="tb-orders"></tbody></table></div>
  </div>
  <div class="panel">
    <div class="panel-title">风控报警</div>
    <div class="panel-body"><div id="alert-list"></div></div>
  </div>
</div>

<script>
const $ = id => document.getElementById(id);
function pnlClass(v) { return v > 0.001 ? 'val-pos' : v < -0.001 ? 'val-neg' : 'val-zero'; }
function fmtNum(v, d) { return v == null ? '--' : Number(v).toFixed(d !== undefined ? d : 2); }
function fmtPnl(v) { if (v == null) return '--'; const s = Number(v).toFixed(2); return v > 0 ? '+' + s : s; }
function fmtLat(v) { return v < 0 ? '--' : v.toString(); }
function latColor(v) { if (v < 0) return 'lat-green'; if (v < 10) return 'lat-green'; if (v < 100) return 'lat-yellow'; return 'lat-red'; }

const dirMap = { buy: '买入', sell: '卖出' };
const statusMap = { all_traded:'全部成交', part_traded:'部分成交', pending:'挂单中', cancelled:'已撤单', error:'错误', submitted:'已报', created:'已创建', risk_rejected:'风控拒绝', cancel_pending:'撤单中' };
const riskModeMap = { normal:'正常', warning:'警告', no_open:'禁开仓', reduce_only:'仅减仓', liquidating:'强平中', halted:'熔断' };
const instrNameMap = { rb:'螺纹钢', au:'黄金', ag:'白银', cu:'沪铜', al:'沪铝', zn:'沪锌', ni:'沪镍', sn:'沪锡', sc:'原油', fu:'燃油', bu:'沥青', ru:'橡胶', IF:'沪深300', IC:'中证500', IH:'上证50', IM:'中证1000', T:'十债', TF:'五债', TS:'二债', pp:'聚丙烯', l:'聚乙烯', m:'豆粕', y:'豆油', p:'棕榈油', c:'玉米', i:'铁矿石', j:'焦炭', jm:'焦煤', eg:'乙二醇', TA:'PTA', MA:'甲醇', CF:'棉花', SR:'白糖', SA:'纯碱', FG:'玻璃', AP:'苹果' };

function getInstrName(id) {
  if (!id) return '';
  for (const [k,v] of Object.entries(instrNameMap)) { if (id.startsWith(k)) return v; }
  return '';
}
function dirClass(d) { return d === 'buy' ? 'dir-buy' : 'dir-sell'; }
function riskClass(m) { if (!m || m==='normal') return 'risk-normal'; if (m==='warning') return 'risk-warning'; return 'risk-danger'; }
function setLat(id, val) { const el=$(id); el.textContent=fmtLat(val); el.className='value '+latColor(val); }

function cancelOrder(ref, aid) {
  fetch('/api/cancel_order', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'order_ref='+ref+'&account_id='+(aid||'')});
}
function cancelAll() {
  fetch('/api/cancel_all', {method:'POST'});
}

async function fetchJson(url) {
  try { const r = await fetch(url); return r.ok ? await r.json() : null; } catch { return null; }
}

async function refresh() {
  const [status, accounts, strategies, latency, pnl, ticks, orders, trades, alerts, risk] = await Promise.all([
    fetchJson('/api/status'), fetchJson('/api/accounts'), fetchJson('/api/strategies'),
    fetchJson('/api/latency'), fetchJson('/api/pnl'), fetchJson('/api/ticks'),
    fetchJson('/api/orders'), fetchJson('/api/trades'), fetchJson('/api/alerts'), fetchJson('/api/risk')
  ]);

  if (status) {
    $('dot-engine').className = 'dot ' + (status.running ? 'dot-green' : 'dot-red');
    $('dot-trading').className = 'dot ' + (status.trading_ready ? 'dot-green' : 'dot-red');
    const rm = status.risk_mode || 'unknown';
    $('risk-mode').textContent = riskModeMap[rm] || rm;
    $('risk-mode').className = riskClass(rm);
    $('dot-risk').className = 'dot ' + (rm==='normal'?'dot-green':rm==='warning'?'dot-yellow':'dot-red');
    if (status.uptime_seconds != null) {
      const s = status.uptime_seconds;
      const h=Math.floor(s/3600), m=Math.floor((s%3600)/60), sec=s%60;
      $('uptime').textContent = h+'时'+m+'分'+sec+'秒';
    }
  }

  if (accounts) {
    let totalPnl = 0;
    $('tb-accounts').innerHTML = accounts.map(a => {
      totalPnl += (a.position_profit||0);
      return `<tr><td>${a.account_id||''}</td><td>${fmtNum(a.balance)}</td><td>${fmtNum(a.available)}</td><td>${fmtNum(a.margin)}</td><td>${fmtNum(a.commission)}</td><td class="${pnlClass(a.position_profit)}">${fmtPnl(a.position_profit)}</td></tr>`;
    }).join('');
    const hp = $('header-pnl');
    hp.textContent = fmtPnl(totalPnl);
    hp.className = 'header-pnl ' + pnlClass(totalPnl);

    // Positions from accounts
    let posHtml = '';
    for (const a of accounts) {
      if (a.positions) {
        for (const p of a.positions) {
          const name = getInstrName(p.instrument_id);
          const dir = p.direction === 'buy' ? '多' : '空';
          const dc = p.direction === 'buy' ? 'dir-buy' : 'dir-sell';
          posHtml += `<tr><td>${p.instrument_id}<span class="instr-name">${name}</span></td><td class="${dc}">${dir}</td><td>${p.volume||0}</td><td>${fmtNum(p.avg_price)}</td><td>${fmtNum(p.last_price)}</td><td class="${pnlClass(p.floating_pnl)}">${fmtPnl(p.floating_pnl)}</td><td>${fmtNum(p.margin)}</td></tr>`;
        }
      }
    }
    $('tb-positions').innerHTML = posHtml || '<tr><td colspan="7" style="color:#8b949e;text-align:center">暂无持仓</td></tr>';
  }

  if (strategies) {
    $('tb-strategies').innerHTML = strategies.map(s => {
      const st = s.status||'';
      const sc = st==='running'?'status-running':st==='paused'?'status-paused':'status-stopped';
      const stText = st==='running'?'运行中':st==='paused'?'已暂停':st==='stopped'?'已停止':st;
      return `<tr><td>${s.strategy_id}</td><td>${s.strategy_type||''}</td><td class="${sc}">${stText}</td><td>${s.trade_count||0}</td><td>${fmtNum(s.win_rate,1)}%</td><td class="${pnlClass(s.total_pnl)}">${fmtPnl(s.total_pnl)}</td><td>${s.last_signal||''}</td><td>${s.last_signal_time||''}</td></tr>`;
    }).join('');
  }

  if (ticks) {
    $('tb-ticks').innerHTML = ticks.map(t => {
      const name = getInstrName(t.instrument_id);
      return `<tr><td>${t.instrument_id}<span class="instr-name">${name}</span></td><td>${fmtNum(t.last_price)}</td><td>${fmtNum(t.bid_price1)}</td><td>${fmtNum(t.ask_price1)}</td><td>${t.volume||0}</td><td>${t.update_time||''}</td></tr>`;
    }).join('');
  }

  if (latency) {
    setLat('lat-t2s', latency.tick_to_signal_us);
    setLat('lat-s2o', latency.signal_to_order_us);
    setLat('lat-o2t', latency.order_to_trade_us);
    setLat('lat-tp',  latency.tick_process_us);
    setLat('lat-op',  latency.order_process_us);
    setLat('lat-trp', latency.trade_process_us);
  }

  if (orders) {
    $('tb-orders').innerHTML = orders.slice(0,30).map(o => {
      const cancelBtn = (o.status==='pending'||o.status==='submitted') ? `<button class="btn btn-cancel" onclick="cancelOrder('${o.order_ref}','${o.account_id||''}')">撤单</button>` : '';
      return `<tr><td>${o.insert_time||''}</td><td>${o.instrument_id}</td><td class="${dirClass(o.direction)}">${dirMap[o.direction]||o.direction}</td><td>${fmtNum(o.price)}</td><td>${o.total_volume||0}</td><td>${statusMap[o.status]||o.status}</td><td>${cancelBtn}</td></tr>`;
    }).join('');
  }

  if (trades) {
    $('tb-trades').innerHTML = trades.slice(0,30).map(t =>
      `<tr><td>${t.trade_time||''}</td><td>${t.instrument_id}</td><td class="${dirClass(t.direction)}">${dirMap[t.direction]||t.direction}</td><td>${fmtNum(t.price)}</td><td>${t.volume||0}</td></tr>`
    ).join('');
  }

  if (alerts) {
    $('alert-list').innerHTML = alerts.slice(0,50).map(a => {
      const s = typeof a === 'string' ? a : JSON.stringify(a);
      const cls = s.includes('ERROR')||s.includes('错误')||s.includes('拒') ? 'alert-error' : s.includes('WARN')||s.includes('警告')||s.includes('接近') ? 'alert-warn' : 'alert-info';
      return `<div class="alert-item ${cls}">${s}</div>`;
    }).join('');
  }

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

  ctx.strokeStyle = '#21262d'; ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = pad.t + ch * i / 4;
    ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(W-pad.r, y); ctx.stroke();
    ctx.fillStyle = '#8b949e'; ctx.font = '10px monospace'; ctx.textAlign = 'right';
    ctx.fillText(fmtNum(mx - range*i/4, 0), pad.l-4, y+3);
  }

  if (mn < 0 && mx > 0) {
    const zy = pad.t + ch * (mx / range);
    ctx.strokeStyle = '#d29922'; ctx.lineWidth = 1; ctx.setLineDash([6,3]);
    ctx.beginPath(); ctx.moveTo(pad.l, zy); ctx.lineTo(W-pad.r, zy); ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = '#d29922'; ctx.font = '10px monospace'; ctx.textAlign = 'left';
    ctx.fillText('0 (盈亏线)', pad.l + 4, zy - 4);
  }

  ctx.lineWidth = 1.5;
  ctx.beginPath();
  for (let i = 0; i < data.length; i++) {
    const x = pad.l + cw * i / (data.length-1);
    const y = pad.t + ch * (1 - (vals[i]-mn)/range);
    if (i === 0) { ctx.moveTo(x,y); continue; }
    ctx.strokeStyle = vals[i] >= 0 ? '#3fb950' : '#f85149';
    ctx.lineTo(x,y); ctx.stroke(); ctx.beginPath(); ctx.moveTo(x,y);
  }

  ctx.beginPath();
  for (let i = 0; i < data.length; i++) {
    const x = pad.l + cw * i / (data.length-1);
    const y = pad.t + ch * (1 - (vals[i]-mn)/range);
    i === 0 ? ctx.moveTo(x,y) : ctx.lineTo(x,y);
  }
  ctx.lineTo(pad.l + cw, pad.t+ch);
  ctx.lineTo(pad.l, pad.t+ch);
  ctx.closePath();
  const lastVal = vals[vals.length-1];
  ctx.fillStyle = lastVal >= 0 ? 'rgba(63,185,80,0.06)' : 'rgba(248,81,73,0.06)';
  ctx.fill();

  ctx.fillStyle = '#8b949e'; ctx.font = '10px monospace'; ctx.textAlign = 'center';
  const step = Math.max(1, Math.floor(data.length/5));
  for (let i = 0; i < data.length; i += step) {
    const x = pad.l + cw * i / (data.length-1);
    const t = data[i].time || '';
    ctx.fillText(t.substring(0,5), x, H-pad.b+14);
  }

  const lastX = pad.l + cw;
  const lastY = pad.t + ch * (1 - (lastVal-mn)/range);
  ctx.fillStyle = lastVal >= 0 ? '#3fb950' : '#f85149';
  ctx.font = 'bold 11px monospace'; ctx.textAlign = 'right';
  ctx.fillText(fmtPnl(lastVal), lastX - 4, lastY - 6);
}

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
