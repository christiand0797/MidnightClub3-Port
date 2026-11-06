// Load test for the World (docs/notes/online_scaling.md): N game clients join the Main Lobby and send System Link sized
// datagrams; reports how instances filled, delivery and latency.
//   npx wrangler dev --var MAX_PER_ADDRESS:100000 --var MAX_PLAYERS:5000      (local, in another terminal)
//   node load_test.mjs [players=300] [rate per player per s=5] [seconds=20] [server=ws://127.0.0.1:8787/ws]
// Never point it at mc3.top: it is a load generator.
const N = Number(process.argv[2] || 300), RATE = Number(process.argv[3] || 5), SECONDS = Number(process.argv[4] || 20);
const URL_ = process.argv[5] || 'ws://127.0.0.1:8787/ws';
if (!/^ws:\/\/(127\.0\.0\.1|localhost)[:/]/.test(URL_)) { console.error('local servers only'); process.exit(2); }

const payload = Buffer.alloc(120, 7).toString('base64');  // a typical System Link datagram
const clients = [];
let sent = 0, received = 0, lat = [], errors = 0, queued = 0;
const rooms = new Map();

function client(i) {
  return new Promise(resolve => {
    const ws = new WebSocket(URL_);
    const c = { ws, room: null, ready: false };
    ws.onopen = () => ws.send(JSON.stringify({ type: 'join', clientType: 'game', name: 'load' + i }));
    ws.onmessage = e => {
      const m = JSON.parse(e.data);
      if (m.type === 'auth_ok') ws.send(JSON.stringify({ type: 'enter_room', name: '' }));
      else if (m.type === 'queue') queued++;
      else if (m.type === 'game_joined') { c.room = m.game.name; c.ready = true; rooms.set(c.room, (rooms.get(c.room) || 0) + 1); resolve(c); }
      else if (m.type === 'input') {
        received++;
        try { const t = JSON.parse(m.data).t; if (t) lat.push(Date.now() - t); } catch (_) {}
      } else if (m.type === 'error') errors++;
    };
    ws.onerror = () => { errors++; resolve(c); };
    ws.onclose = () => resolve(c);
  });
}

const t0 = Date.now();
for (let i = 0; i < N; i += 50) clients.push(...await Promise.all(Array.from({ length: Math.min(50, N - i) }, (_, k) => client(i + k))));
const ready = clients.filter(c => c.ready);
console.log(`${ready.length}/${N} joined in ${((Date.now() - t0) / 1000).toFixed(1)} s, ${queued} queue notices, rooms:`, Object.fromEntries(rooms));

const end = Date.now() + SECONDS * 1000;
const timer = setInterval(() => {
  for (const c of ready) {
    if (c.ws.readyState !== 1) continue;
    c.ws.send(JSON.stringify({ type: 'input', data: JSON.stringify({ payload, src_port: 1000, dst_port: 1000, t: Date.now() }) }));
    sent++;
  }
}, 1000 / RATE);
await new Promise(r => setTimeout(r, end - Date.now()));
clearInterval(timer);
await new Promise(r => setTimeout(r, 2000));
lat.sort((a, b) => a - b);
const pct = p => lat.length ? lat[Math.min(lat.length - 1, Math.floor(lat.length * p))] : NaN;
const perRoom = [...rooms.values()].reduce((a, n) => a + n * (n - 1), 0) / Math.max(1, ready.length);  // expected copies per send
console.log(`sent ${sent} datagrams (${(sent / SECONDS).toFixed(0)}/s), delivered ${received} (${(received / SECONDS).toFixed(0)}/s, ` +
  `${(received / Math.max(1, sent * perRoom) * 100).toFixed(1)}% of expected), latency p50 ${pct(0.5)} ms p95 ${pct(0.95)} ms p99 ${pct(0.99)} ms, errors ${errors}`);
for (const c of clients) try { c.ws.close(); } catch (_) {}
process.exit(0);
