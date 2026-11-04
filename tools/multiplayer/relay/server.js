// MC3 Multiplayer Relay Server (Public, 24/7)
// nfsu2.xyz style: central matchmaking + game proxy relay + browser spectator
//
// This server runs on a public VPS and handles:
//   - User registration/login (JWT auth)
//   - Public game lobby & matchmaking
//   - WebSocket relay between game proxies and browser clients
//   - Input relay (browser → game) and stream relay (game → browser)
//   - Persistent player profiles, cloud saves, career progress, car setups
//
// Environment variables (see .env.example):
//   RELAY_PORT        HTTP/WebSocket port (default 8080)
//   JWT_SECRET        Secret for signing JWT tokens
//   DATA_DIR          Path to data directory (default ./data)
//   CORS_ORIGIN       Allowed CORS origin (default * for dev)
//   MAX_PLAYERS       Max players per game (default 8)
//   GAME_TIMEOUT      Seconds before idle game is removed (default 3600)

require('dotenv').config();
const express = require('express');
const http = require('http');
const crypto = require('crypto');
const { WebSocketServer } = require('ws');
const { v4: uuidv4 } = require('uuid');
const jwt = require('jsonwebtoken');
const cors = require('cors');
const rateLimit = require('express-rate-limit');
const path = require('path');
const fs = require('fs');

// ---------- Configuration ----------
const PORT = parseInt(process.env.RELAY_PORT, 10) || 8080;
const JWT_SECRET = process.env.JWT_SECRET || 'change-me-in-production';
const DATA_DIR = process.env.DATA_DIR || path.join(__dirname, 'data');
const CORS_ORIGIN = process.env.CORS_ORIGIN || '*';
const MAX_PLAYERS = parseInt(process.env.MAX_PLAYERS, 10) || 8;
const GAME_TIMEOUT = parseInt(process.env.GAME_TIMEOUT, 10) || 3600;

fs.mkdirSync(DATA_DIR, { recursive: true });

// ---------- Simple JSON file database ----------
const DB_FILE = path.join(DATA_DIR, 'mc3_relay.json');

function loadDB() {
  try {
    const raw = fs.readFileSync(DB_FILE, 'utf8');
    return JSON.parse(raw);
  } catch (e) {
    return {
      users: [],
      games: [],
      game_players: [],
      cloud_saves: [],
      career_progress: [],
      car_setups: [],
      match_history: []
    };
  }
}

function saveDB(db) {
  fs.writeFileSync(DB_FILE, JSON.stringify(db, null, 2));
}

let db = loadDB();

// Helper: find by id
function findById(table, id) {
  return db[table].find(r => r.id === id);
}
function findBy(table, key, value) {
  return db[table].find(r => r[key] === value);
}
function filterBy(table, key, value) {
  return db[table].filter(r => r[key] === value);
}
function push(table, record) {
  db[table].push(record);
  saveDB(db);
  return record;
}
function updateById(table, id, updates) {
  const idx = db[table].findIndex(r => r.id === id);
  if (idx === -1) return false;
  db[table][idx] = { ...db[table][idx], ...updates };
  saveDB(db);
  return true;
}
function removeById(table, id) {
  const idx = db[table].findIndex(r => r.id === id);
  if (idx === -1) return false;
  db[table].splice(idx, 1);
  saveDB(db);
  return true;
}

// ---------- Hel ----------
function now() { return Math.floor(Date.now() / 1000); }

function makeToken(user) {
  return jwt.sign(
    { id: user.id, username: user.username },
    JWT_SECRET,
    { expiresIn: '7d' }
  );
}

function verifyToken(token) {
  try {
    return jwt.verify(token, JWT_SECRET);
  } catch (e) {
    return null;
  }
}

// ---------- Express App ----------
const app = express();
app.use(cors({ origin: CORS_ORIGIN }));
app.use(express.json({ limit: '10kb' }));

// Rate limiting
const apiLimiter = rateLimit({
  windowMs: 15 * 60 * 1000,
  max: 100,
  message: { error: 'Too many requests, please try again later.' }
});
app.use('/api/', apiLimiter);

// ---------- Auth Routes ----------
app.post('/api/register', (req, res) => {
  const { username, password, email } = req.body;
  if (!username || !password) {
    return res.status(400).json({ error: 'Username and password required' });
  }
  if (username.length < 3 || username.length > 20) {
    return res.status(400).json({ error: 'Username must be 3-20 characters' });
  }
  if (password.length < 6) {
    return res.status(400).json({ error: 'Password must be at least 6 characters' });
  }

  const existing = findBy('users', 'username', username);
  if (existing) {
    return res.status(409).json({ error: 'Username already taken' });
  }

  const id = uuidv4();
  const passwordHash = crypto.pbkdf2Sync(password, 'mc3-salt', 100000, 64, 'sha512').toString('hex');
  const user = {
    id,
    username,
    password_hash: passwordHash,
    email: email || null,
    created_at: now(),
    last_seen: now(),
    games_played: 0,
    rating: 1000
  };
  push('users', user);
  const token = makeToken({ id, username });
  res.json({ token, user: { id, username, email: email || null } });
});

app.post('/api/login', (req, res) => {
  const { username, password } = req.body;
  if (!username || !password) {
    return res.status(400).json({ error: 'Username and password required' });
  }

  const user = findBy('users', 'username', username);
  if (!user) {
    return res.status(401).json({ error: 'Invalid credentials' });
  }
  const passwordHash = crypto.pbkdf2Sync(password, 'mc3-salt', 100000, 64, 'sha512').toString('hex');
  if (user.password_hash !== passwordHash) {
    return res.status(401).json({ error: 'Invalid credentials' });
  }

  user.last_seen = now();
  saveDB(db);
  const token = makeToken({ id: user.id, username: user.username });
  res.json({
    token,
    user: { id: user.id, username: user.username, email: user.email || null, rating: user.rating }
  });
});

function authMiddleware(req, res, next) {
  const header = req.headers['authorization'];
  if (!header || !header.startsWith('Bearer ')) {
    return res.status(401).json({ error: 'Authentication required' });
  }
  const token = header.slice(7);
  const payload = verifyToken(token);
  if (!payload) {
    return res.status(401).json({ error: 'Invalid or expired token' });
  }
  req.user = payload;
  next();
}

// ---------- Game Routes ----------
app.get('/api/games', (req, res) => {
  const status = req.query.status || 'waiting';
  const games = filterBy('games', 'status', status).map(g => {
    const players = filterBy('game_players', 'game_id', g.id);
    return { ...g, player_count: players.length };
  }).sort((a, b) => b.created_at - a.created_at).slice(0, 100);
  res.json({ games });
});

app.post('/api/games', authMiddleware, (req, res) => {
  const { name, max_players = MAX_PLAYERS, region = 'global' } = req.body;
  if (!name || name.length < 1 || name.length > 50) {
    return res.status(400).json({ error: 'Invalid game name' });
  }

  const id = uuidv4();
  const game = {
    id,
    name,
    host_id: req.user.id,
    host_username: req.user.username,
    status: 'waiting',
    max_players,
    player_count: 1,
    created_at: now(),
    started_at: null,
    finished_at: null,
    region
  };
  push('games', game);
  push('game_players', {
    game_id: id,
    user_id: req.user.id,
    username: req.user.username,
    role: 'host',
    joined_at: now()
  });
  res.json({ game });
});

app.post('/api/games/:gameId/join', authMiddleware, (req, res) => {
  const { gameId } = req.params;
  const game = findBy('games', 'id', gameId);
  if (!game) {
    return res.status(404).json({ error: 'Game not found' });
  }
  if (game.status !== 'waiting') {
    return res.status(400).json({ error: 'Game already started or finished' });
  }

  const players = filterBy('game_players', 'game_id', gameId);
  if (players.length >= game.max_players) {
    return res.status(400).json({ error: 'Game is full' });
  }

  const existing = filterBy('game_players', 'game_id', gameId).find(p => p.user_id === req.user.id);
  if (existing) {
    return res.json({ game: { ...game, player_count: players.length } });
  }

  push('game_players', {
    game_id: gameId,
    user_id: req.user.id,
    username: req.user.username,
    role: 'player',
    joined_at: now()
  });
  game.player_count = players.length + 1;
  updateById('games', gameId, { player_count: game.player_count });

  res.json({ game: { ...game, player_count: game.player_count } });
});

app.post('/api/games/:gameId/leave', authMiddleware, (req, res) => {
  const { gameId } = req.params;
  const game = findBy('games', 'id', gameId);
  if (!game) {
    return res.json({ ok: true });
  }

  // Remove player
  const playerIdx = filterBy('game_players', 'game_id', gameId).findIndex(p => p.user_id === req.user.id);
  if (playerIdx === -1) {
    return res.json({ ok: true });
  }
  const leaving = filterBy('game_players', 'game_id', gameId)[playerIdx];
  db['game_players'].splice(playerIdx, 1);
  saveDB(db);

  const remaining = filterBy('game_players', 'game_id', gameId);

  if (remaining.length === 0) {
    // Delete game
    db['games'] = db['games'].filter(g => g.id !== gameId);
    db['game_players'] = db['game_players'].filter(p => p.game_id !== gameId);
    saveDB(db);
  } else if (game.host_id === req.user.id) {
    // Transfer host
    const newHost = remaining[0];
    updateById('game_players', newHost.id, { role: 'host' });
    // Actually newHost.id is game_players id, need user_id
    const newHostUser = findBy('users', 'id', newHost.user_id);
    updateById('games', gameId, { host_id: newHost.user_id, host_username: newHostUser.username });
    // Broadcast host transfer handled in WS
  } else {
    game.player_count = remaining.length;
    updateById('games', gameId, { player_count: game.player_count });
  }

  res.json({ ok: true });
});

app.get('/api/games/:gameId', (req, res) => {
  const { gameId } = req.params;
  const game = findBy('games', 'id', gameId);
  if (!game) {
    return res.status(404).json({ error: 'Game not found' });
  }
  const players = filterBy('game_players', 'game_id', gameId);
  res.json({ game: { ...game, players }, player_count: players.length });
});

app.get('/api/profile', authMiddleware, (req, res) => {
  const user = findBy('users', 'id', req.user.id);
  if (!user) {
    return res.status(404).json({ error: 'User not found' });
  }
  res.json({ user: { id: user.id, username: user.username, email: user.email, created_at: user.created_at, last_seen: user.last_seen, games_played: user.games_played, rating: user.rating } });
});

// ---------- Cloud Saves ----------
app.get('/api/saves', authMiddleware, (req, res) => {
  const saves = filterBy('cloud_saves', 'user_id', req.user.id)
    .sort((a, b) => b.updated_at - a.updated_at)
    .map(s => ({ id: s.id, slot: s.slot, save_name: s.save_name, playtime: s.playtime, money: s.money, rating: s.rating, created_at: s.created_at, updated_at: s.updated_at }));
  res.json({ saves });
});

app.get('/api/saves/:slot', authMiddleware, (req, res) => {
  const { slot } = req.params;
  const save = filterBy('cloud_saves', 'user_id', req.user.id).find(s => s.slot === parseInt(slot));
  if (!save) {
    return res.status(404).json({ error: 'Save not found' });
  }
  res.json({ save });
});

app.post('/api/saves', authMiddleware, (req, res) => {
  const { slot, save_name, save_data, thumbnail, playtime, money, rating } = req.body;
  if (slot === undefined || save_data === undefined) {
    return res.status(400).json({ error: 'slot and save_data required' });
  }

  const slotNum = parseInt(slot);
  const existing = filterBy('cloud_saves', 'user_id', req.user.id).find(s => s.slot === slotNum);
  const nowTs = now();
  try {
    if (existing) {
      Object.assign(existing, {
        save_name: save_name || 'Untitled',
        save_data: JSON.stringify(save_data),
        thumbnail: thumbnail || null,
        playtime: playtime || 0,
        money: money || 0,
        rating: rating || 0,
        updated_at: nowTs
      });
      saveDB(db);
      res.json({ success: true, slot: slotNum, message: 'Save updated' });
    } else {
      const id = uuidv4();
      push('cloud_saves', {
        id,
        user_id: req.user.id,
        slot: slotNum,
        save_name: save_name || 'Untitled',
        save_data: JSON.stringify(save_data),
        thumbnail: thumbnail || null,
        playtime: playtime || 0,
        money: money || 0,
        rating: rating || 0,
        created_at: nowTs,
        updated_at: nowTs
      });
      res.json({ success: true, id, slot: slotNum, message: 'Save created' });
    }
  } catch (e) {
    res.status(500).json({ error: 'Failed to save game' });
  }
});

app.delete('/api/saves/:slot', authMiddleware, (req, res) => {
  const { slot } = req.params;
  const idx = filterBy('cloud_saves', 'user_id', req.user.id).findIndex(s => s.slot === parseInt(slot));
  if (idx === -1) {
    return res.status(404).json({ error: 'Save not found' });
  }
  db['cloud_saves'].splice(idx, 1);
  saveDB(db);
  res.json({ success: true });
});

// ---------- Career Progress ----------
app.get('/api/career', authMiddleware, (req, res) => {
  let career = findBy('career_progress', 'user_id', req.user.id);
  if (!career) {
    const id = uuidv4();
    career = {
      id,
      user_id: req.user.id,
      total_races: 0,
      races_won: 0,
      total_money_earned: 0,
      current_money: 0,
      cars_owned: 0,
      cities_unlocked: [],
      difficulty: 'medium',
      last_played: now()
    };
    push('career_progress', career);
  }
  career.cities_unlocked = JSON.parse(career.cities_unlocked || '[]');
  res.json({ career });
});

app.post('/api/career', authMiddleware, (req, res) => {
  const { total_races, races_won, total_money_earned, current_money, cars_owned, cities_unlocked, difficulty, last_played } = req.body;
  let career = findBy('career_progress', 'user_id', req.user.id);
  if (!career) {
    const id = uuidv4();
    career = { id, user_id: req.user.id, total_races: 0, races_won: 0, total_money_earned: 0, current_money: 0, cars_owned: 0, cities_unlocked: [], difficulty: 'medium', last_played: now() };
    push('career_progress', career);
  }
  if (total_races !== undefined) career.total_races = total_races;
  if (races_won !== undefined) career.races_won = races_won;
  if (total_money_earned !== undefined) career.total_money_earned = total_money_earned;
  if (current_money !== undefined) career.current_money = current_money;
  if (cars_owned !== undefined) career.cars_owned = cars_owned;
  if (cities_unlocked !== undefined) career.cities_unlocked = JSON.stringify(cities_unlocked);
  if (difficulty !== undefined) career.difficulty = difficulty;
  if (last_played !== undefined) career.last_played = last_played;
  saveDB(db);
  res.json({ success: true });
});

// ---------- Car Setups ----------
app.get('/api/car-setups', authMiddleware, (req, res) => {
  const setups = filterBy('car_setups', 'user_id', req.user.id).sort((a, b) => b.updated_at - a.updated_at);
  for (const s of setups) {
    s.setup_data = JSON.parse(s.setup_data || '{}');
  }
  res.json({ setups });
});

app.post('/api/car-setups', authMiddleware, (req, res) => {
  const { car_name, setup_data, is_equipped } = req.body;
  if (!car_name || !setup_data) {
    return res.status(400).json({ error: 'car_name and setup_data required' });
  }
  const id = uuidv4();
  const nowTs = now();
  try {
    push('car_setups', {
      id,
      user_id: req.user.id,
      car_name,
      setup_data: JSON.stringify(setup_data),
      is_equipped: is_equipped ? 1 : 0,
      created_at: nowTs,
      updated_at: nowTs
    });
    res.json({ success: true, id });
  } catch (e) {
    res.status(500).json({ error: 'Failed to save car setup' });
  }
});

// ---------- HTTP Server + WebSocket ----------
const server = http.createServer(app);

// WebSocket server for real-time relay
const wss = new WebSocketServer({ server, path: '/ws' });

// Connected clients: ws -> { id, type ('game' | 'browser'), userId, gameId, role, username, color }
const clients = new Map();

// Games in memory (synced with DB)
const activeGames = new Map(); // gameId -> { id, name, hostId, players: Map(wsId -> client), spectators: Set(wsId) }

function broadcastToGame(gameId, message, excludeWs = null) {
  const game = activeGames.get(gameId);
  if (!game) return;
  const targets = new Set([...game.players.keys(), ...game.spectators.keys()]);
  if (excludeWs) targets.delete(excludeWs);
  const data = typeof message === 'string' ? message : JSON.stringify(message);
  for (const ws of targets) {
    if (ws.readyState === 1) ws.send(data);
  }
}

function sendGameListUpdate() {
  const games = [];
  for (const [id, game] of activeGames) {
    games.push({
      id: game.id,
      name: game.name,
      host_username: game.hostUsername,
      status: game.status,
      max_players: game.maxPlayers,
      player_count: game.players.size,
      region: game.region
    });
  }
  const msg = JSON.stringify({ type: 'gameList', games });
  for (const ws of clients.keys()) {
    if (ws.readyState === 1) ws.send(msg);
  }
}

function createGame(id, name, hostId, hostUsername, maxPlayers, region) {
  const game = {
    id,
    name,
    hostId,
    hostUsername,
    status: 'waiting',
    maxPlayers,
    region,
    players: new Map(),
    spectators: new Set(),
    createdAt: now()
  };
  activeGames.set(id, game);
  return game;
}

function removeGame(gameId) {
  activeGames.delete(gameId);
}

function assignColor() {
  const colors = ['#ff4444', '#44ff44', '#4444ff', '#ffff44', '#ff44ff', '#44ffff', '#ffaa44', '#aa44ff'];
  let used = new Set();
  for (const game of activeGames.values()) {
    for (const p of game.players.values()) {
      used.add(p.color);
    }
  }
  for (const c of colors) {
    if (!used.has(c)) return c;
  }
  return '#888888';
}

function leaveGame(client, ws) {
  const game = activeGames.get(client.gameId);
  if (!game) return;

  const wasHost = game.hostId === client.userId;
  game.players.delete(ws);
  game.spectators.delete(ws);

  // Update DB
  db['game_players'] = db['game_players'].filter(p => !(p.game_id === client.gameId && p.user_id === client.user.id));
  saveDB(db);

  if (game.players.size === 0 && game.spectators.size === 0) {
    removeGame(client.gameId);
    db['games'] = db['games'].filter(g => g.id !== client.gameId);
    db['game_players'] = db['game_players'].filter(p => p.game_id !== client.gameId);
    saveDB(db);
  } else {
    const gameObj = findBy('games', 'id', client.gameId);
    if (gameObj) {
      gameObj.player_count = game.players.size;
      updateById('games', client.gameId, { player_count: game.players.size });
    }

    if (wasHost && game.players.size > 0) {
      const [newHostWs] = game.players.keys();
      const newHost = game.players.get(newHostWs);
      game.hostId = newHost.id;
      game.hostUsername = newHost.username;
      // Update game_players role
      const npIdx = filterBy('game_players', 'game_id', client.gameId).findIndex(p => p.user_id === newHost.id);
      if (npIdx !== -1) {
        db['game_players'][npIdx].role = 'host';
        saveDB(db);
      }
      updateById('games', client.gameId, { host_id: newHost.id, host_username: newHost.username });
      broadcastToGame(client.gameId, {
        type: 'host_transferred',
        newHost: newHost.username
      });
    } else if (gameObj) {
      updateById('games', client.gameId, { player_count: game.players.size });
    }

    broadcastToGame(client.gameId, {
      type: 'player_left',
      username: client.username,
      playerCount: game.players.size
    });
  }

  client.gameId = null;
  client.role = null;
  client.color = null;
  sendGameListUpdate();
}

// ---------- WebSocket Handling ----------
wss.on('connection', (ws, req) => {
  const clientId = uuidv4();
  let client = null;

  ws.isAlive = true;
  ws.on('pong', () => { ws.isAlive = true; });

  ws.on('message', (raw) => {
    try {
      const data = typeof raw === 'string' ? JSON.parse(raw) : null;
      if (!data) return;

      switch (data.type) {
        case 'auth': {
          const token = data.token;
          const payload = verifyToken(token);
          if (!payload) {
            ws.send(JSON.stringify({ type: 'error', error: 'Invalid token' }));
            ws.close();
            return;
          }
          client = {
            id: clientId,
            type: data.clientType || 'browser',
            userId: payload.id,
            username: payload.username,
            gameId: null,
            role: null,
            color: null
          };
          clients.set(ws, client);
          ws.send(JSON.stringify({ type: 'auth_ok', user: { id: payload.id, username: payload.username } }));
          break;
        }

        case 'create_game': {
          if (!client) return;
          const id = uuidv4().slice(0, 8);
          const color = assignColor();
          const game = createGame(id, data.name, client.userId, client.username, data.maxPlayers || MAX_PLAYERS, data.region || 'global');
          client.gameId = id;
          client.role = 'host';
          client.color = color;
          game.players.set(ws, { id: client.userId, username: client.username, role: 'host', color, ready: false });
          ws.send(JSON.stringify({
            type: 'game_created',
            game: { id, name: data.name, hostId: client.userId, hostUsername: client.username, maxPlayers: game.maxPlayers, playerCount: 1, region: game.region },
            role: 'host',
            color
          }));
          sendGameListUpdate();
          break;
        }

        case 'join_game': {
          if (!client) return;
          const game = activeGames.get(data.gameId);
          if (!game || game.status !== 'waiting') {
            ws.send(JSON.stringify({ type: 'error', error: 'Game not found or not waiting' }));
            return;
          }
          if (game.players.size >= game.maxPlayers) {
            ws.send(JSON.stringify({ type: 'error', error: 'Game is full' }));
            return;
          }
          const color = assignColor();
          client.gameId = data.gameId;
          client.role = 'player';
          client.color = color;
          game.players.set(ws, { id: client.userId, username: client.username, role: 'player', color, ready: false });
          const gObj = findBy('games', 'id', data.gameId);
          if (gObj) {
            gObj.player_count = game.players.size;
            updateById('games', data.gameId, { player_count: game.players.size });
          }
          push('game_players', {
            game_id: data.gameId,
            user_id: client.userId,
            username: client.username,
            role: 'player',
            joined_at: now()
          });
          ws.send(JSON.stringify({
            type: 'game_joined',
            game: {
              id: game.id,
              name: game.name,
              hostId: game.hostId,
              hostUsername: game.hostUsername,
              maxPlayers: game.maxPlayers,
              playerCount: game.players.size,
              region: game.region
            },
            role: 'player',
            color
          }));
          broadcastToGame(data.gameId, {
            type: 'player_joined',
            username: client.username,
            color,
            playerCount: game.players.size
          });
          sendGameListUpdate();
          break;
        }

        case 'leave_game': {
          if (!client || !client.gameId) return;
          leaveGame(client, ws);
          break;
        }

        case 'set_ready': {
          if (!client || !client.gameId) return;
          const game = activeGames.get(client.gameId);
          if (!game) return;
          const p = game.players.get(ws);
          if (p) {
            p.ready = data.ready !== false;
            broadcastToGame(client.gameId, {
              type: 'player_ready',
              username: client.username,
              ready: p.ready
            });
          }
          break;
        }

        case 'start_game': {
          if (!client || !client.gameId) return;
          const game = activeGames.get(client.gameId);
          if (!game || game.hostId !== client.userId) return;
          game.status = 'playing';
          updateById('games', client.gameId, { status: 'playing', started_at: now() });
          broadcastToGame(client.gameId, {
            type: 'game_started',
            seed: data.seed || now()
          });
          sendGameListUpdate();
          break;
        }

        case 'input': {
          if (!client || !client.gameId) return;
          const game = activeGames.get(client.gameId);
          if (!game) return;
          const msg = JSON.stringify({
            type: 'input',
            from: client.username,
            data: data.data,
            ts: Date.now()
          });
          for (const gws of game.players.keys()) {
            if (gws.readyState === 1 && clients.get(gws)?.type === 'game') {
              gws.send(msg);
            }
          }
          break;
        }

        case 'chat': {
          if (!client || !client.gameId) return;
          broadcastToGame(client.gameId, {
            type: 'chat',
            from: client.username,
            text: data.text,
            ts: Date.now()
          });
          break;
        }

        case 'ping':
          ws.send(JSON.stringify({ type: 'pong', ts: data.ts }));
          break;
      }
    } catch (e) {
      console.error('WS message error:', e.message);
    }
  });

  ws.on('close', () => {
    if (client && client.gameId) {
      leaveGame(client, ws);
    }
    clients.delete(ws);
  });

  ws.on('error', (e) => {
    console.error('WS error:', e.message);
  });
});

// Heartbeat
const interval = setInterval(() => {
  for (const ws of wss.clients) {
    if (ws.isAlive === false) return ws.terminate();
    ws.isAlive = false;
    ws.ping();
  }
}, 30000);

wss.on('close', () => clearInterval(interval));

// ---------- Start ----------
server.listen(PORT, () => {
  console.log(`[relay] HTTP+WS listening on port ${PORT}`);
  console.log(`[relay] Data directory: ${DATA_DIR}`);
  console.log(`[relay] JWT_SECRET: ${JWT_SECRET === 'change-me-in-production' ? 'WARNING: using default!' : 'set'}`);
});

// Graceful shutdown
process.on('SIGTERM', () => {
  console.log('[relay] Shutting down...');
  wss.close();
  server.close();
  saveDB(db);
  process.exit(0);
});
