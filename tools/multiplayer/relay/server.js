// MC3 Browser Multiplayer Relay Server
// Central hub: game proxies (UDP<->WS) and browser clients connect via WebSocket.
// Packets are treated as opaque binary - no game protocol knowledge needed.

const WebSocket = require('ws');
const http = require('http');
const { v4: uuidv4 } = require('uuid');

// ---- Config ----
const PORT = process.env.RELAY_PORT || 8080;
const WS_PING_INTERVAL = 30000;
const WS_PING_TIMEOUT = 5000;
const MAX_PLAYERS_PER_GAME = 8;
const HEARTBEAT_INTERVAL = 5000;

// ---- State ----
const games = new Map();      // gameId -> Game
const clients = new Map();    // ws -> Client

class Client {
  constructor(ws, type, name) {
    this.ws = ws;
    this.id = uuidv4();
    this.type = type; // 'game' | 'browser'
    this.name = name || this.id.slice(0, 8);
    this.gameId = null;
    this.role = null;       // 'host' | 'player' | 'spectator'
    this.alive = true;
    this.lastPing = Date.now();
    this.color = null;
  }

  send(data) {
    if (this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(data);
    }
  }

  close() {
    this.ws.close();
  }
}

class Game {
  constructor(id, hostWs) {
    this.id = id;
    this.hostWs = hostWs;
    this.players = new Map(); // clientId -> {ws, name, color, ready}
    this.spectators = new Map();
    this.createdAt = Date.now();
    this.started = false;
    this.maxPlayers = MAX_PLAYERS_PER_GAME;
  }

  addPlayer(ws, name) {
    if (this.players.size >= this.maxPlayers) return false;
    const color = this.nextColor();
    this.players.set(ws, { ws, name, color, ready: false });
    return color;
  }

  removePlayer(ws) {
    this.players.delete(ws);
  }

  addSpectator(ws) {
    this.spectators.set(ws, { ws });
  }

  removeSpectator(ws) {
    this.spectators.delete(ws);
  }

  nextColor() {
    const used = new Set();
    for (const p of this.players.values()) used.add(p.color);
    const colors = ['#ff4444', '#44ff44', '#4444ff', '#ffff44', '#ff44ff', '#44ffff', '#ffaa44', '#aa44ff'];
    for (const c of colors) if (!used.has(c)) return c;
    return '#888888';
  }

  broadcast(data, excludeWs = null) {
    const msg = typeof data === 'string' ? data : JSON.stringify(data);
    for (const ws of [...this.players.keys(), ...this.spectators.keys()]) {
      if (ws !== excludeWs && ws.readyState === WebSocket.OPEN) ws.send(msg);
    }
  }

  broadcastBinary(buffer, excludeWs = null) {
    for (const ws of [...this.players.keys(), ...this.spectators.keys()]) {
      if (ws !== excludeWs && ws.readyState === WebSocket.OPEN) ws.send(buffer);
    }
  }

  getSummary() {
    return {
      id: this.id,
      playerCount: this.players.size,
      spectatorCount: this.spectators.size,
      started: this.started,
      maxPlayers: this.maxPlayers,
      players: [...this.players.values()].map(p => ({ name: p.name, color: p.color, ready: p.ready })),
    };
  }
}

// ---- HTTP server for health check ----
const httpServer = http.createServer((req, res) => {
  res.writeHead(200, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ ok: true, games: games.size, clients: clients.size }));
});

const wss = new WebSocket.Server({ server: httpServer });

wss.on('connection', (ws, req) => {
  const url = new URL(req.url || '', `http://${req.headers.host}`);
  const type = url.searchParams.get('type') || 'browser';
  const name = url.searchParams.get('name') || '';
  const gameId = url.searchParams.get('game') || null;

  const client = new Client(ws, type, name);
  clients.set(ws, client);

  ws.isAlive = true;
  ws.on('pong', () => {
    ws.isAlive = true;
    client.lastPing = Date.now();
  });

  ws.on('message', (data) => {
    handleMessage(client, data);
  });

  ws.on('close', () => {
    handleDisconnect(client);
  });

  ws.on('error', (err) => {
    console.error(`[relay] ws error for ${client.id}:`, err.message);
    handleDisconnect(client);
  });

  console.log(`[relay] client connected: ${client.id} (${type}) ${name}`);
});

function handleMessage(client, data) {
  let msg;
  try {
    msg = JSON.parse(data);
  } catch (e) {
    // Binary packet from game proxy - forward to game peers
    if (client.type === 'game' && client.gameId) {
      const game = games.get(client.gameId);
      if (game) game.broadcastBinary(data, client.ws);
    }
    return;
  }

  switch (msg.type) {
    case 'join':
      handleJoin(client, msg);
      break;
    case 'create':
      handleCreate(client, msg);
      break;
    case 'leave':
      handleLeave(client);
      break;
    case 'ready':
      handleReady(client, msg);
      break;
    case 'start':
      handleStart(client, msg);
      break;
    case 'packet':
      // Game proxy forwarding an opaque packet to peers
      if (client.type === 'game' && client.gameId) {
        const game = games.get(client.gameId);
        if (game) game.broadcastBinary(msg.data, client.ws);
      }
      break;
    case 'chat':
      if (client.gameId) {
        const game = games.get(client.gameId);
        if (game) game.broadcast({ type: 'chat', from: client.name, text: msg.text }, client.ws);
      }
      break;
    case 'list':
      sendGameList(client.ws);
      break;
    case 'ping':
      client.ws.send(JSON.stringify({ type: 'pong', ts: Date.now() }));
      break;
    default:
      console.log(`[relay] unknown msg type: ${msg.type}`);
  }
}

function handleJoin(client, msg) {
  const { gameId } = msg;
  if (!gameId || !games.has(gameId)) {
    client.ws.send(JSON.stringify({ type: 'error', error: 'game not found' }));
    return;
  }
  const game = games.get(gameId);
  if (game.started) {
    // Join as spectator
    game.addSpectator(client.ws);
    client.gameId = gameId;
    client.role = 'spectator';
    client.ws.send(JSON.stringify({ type: 'joined', gameId, role: 'spectator', game: game.getSummary() }));
    game.broadcast({ type: 'spectatorJoined', name: client.name });
  } else {
    const color = game.addPlayer(client.ws, client.name);
    if (!color) {
      client.ws.send(JSON.stringify({ type: 'error', error: 'game full' }));
      return;
    }
    client.gameId = gameId;
    client.role = 'player';
    client.ws.send(JSON.stringify({ type: 'joined', gameId, role: 'player', color, game: game.getSummary() }));
    game.broadcast({ type: 'playerJoined', name: client.name, color }, client.ws);
  }
  broadcastGameList();
}

function handleCreate(client, msg) {
  const { name } = msg;
  const gameId = uuidv4().slice(0, 8);
  const game = new Game(gameId, client.ws);
  games.set(gameId, game);
  const color = game.addPlayer(client.ws, name || 'Host');
  client.gameId = gameId;
  client.role = 'host';
  client.ws.send(JSON.stringify({ type: 'created', gameId, role: 'host', color, game: game.getSummary() }));
  broadcastGameList();
  console.log(`[relay] game created: ${gameId}`);
}

function handleLeave(client) {
  if (client.gameId) {
    const game = games.get(client.gameId);
    if (game) {
      if (client.role === 'player') game.removePlayer(client.ws);
      else if (client.role === 'spectator') game.removeSpectator(client.ws);
      game.broadcast({ type: 'playerLeft', name: client.name, id: client.id });
      if (game.players.size === 0 && game.spectators.size === 0) {
        games.delete(gameId);
        console.log(`[relay] game removed: ${gameId}`);
      }
    }
    client.gameId = null;
    client.role = null;
  }
  broadcastGameList();
}

function handleReady(client, msg) {
  if (client.gameId && client.role === 'player') {
    const game = games.get(client.gameId);
    if (game && game.players.has(client.ws)) {
      game.players.get(client.ws).ready = msg.ready !== false;
      game.broadcast({ type: 'playerReady', name: client.name, ready: game.players.get(client.ws).ready });
    }
  }
}

function handleStart(client, msg) {
  if (client.gameId && client.role === 'host') {
    const game = games.get(client.gameId);
    if (game && !game.started) {
      game.started = true;
      game.broadcast({ type: 'gameStarted', seed: msg.seed || Date.now() });
      console.log(`[relay] game started: ${gameId}`);
    }
  }
}

function handleDisconnect(client) {
  clients.delete(client.ws);
  handleLeave(client);
  console.log(`[relay] client disconnected: ${client.id}`);
}

function sendGameList(ws) {
  const list = [...games.values()].map(g => g.getSummary());
  ws.send(JSON.stringify({ type: 'gameList', games: list }));
}

function broadcastGameList() {
  const list = [...games.values()].map(g => g.getSummary());
  for (const ws of clients.keys()) {
    if (ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify({ type: 'gameList', games: list }));
  }
}

// ---- Heartbeat ----
const interval = setInterval(() => {
  for (const ws of wss.clients) {
    if (ws.isAlive === false) {
      ws.terminate();
      continue;
    }
    ws.isAlive = false;
    ws.ping();
  }
}, HEARTBEAT_INTERVAL);

wss.on('close', () => clearInterval(interval));

httpServer.listen(PORT, () => {
  console.log(`[relay] listening on ws://localhost:${PORT}`);
});

process.on('SIGINT', () => {
  console.log('[relay] shutting down');
  process.exit(0);
});
