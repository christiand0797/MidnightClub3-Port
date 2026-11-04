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
const { installAccountRoutes, atomicJson } = require('./account_service');
const { TransportSecurity, validateRoomRequest, originAllowed } = require('./transport_security');

// ---------- Configuration ----------
const PORT = parseInt(process.env.RELAY_PORT, 10) || 8080;
const DATA_DIR = process.env.DATA_DIR || path.join(__dirname, 'data');
fs.mkdirSync(DATA_DIR, { recursive: true });
const secretFile = path.join(DATA_DIR, '.session-secret');
if (!process.env.JWT_SECRET && !fs.existsSync(secretFile))
  fs.writeFileSync(secretFile, crypto.randomBytes(48).toString('hex'), { flag: 'wx', mode: 0o600 });
const JWT_SECRET = process.env.JWT_SECRET || fs.readFileSync(secretFile, 'utf8').trim();
if (JWT_SECRET.length < 32) throw new Error('JWT_SECRET must contain at least 32 characters.');
const CORS_ORIGIN = process.env.CORS_ORIGIN || '';
const MAX_PLAYERS = parseInt(process.env.MAX_PLAYERS, 10) || 8;
const GAME_TIMEOUT = parseInt(process.env.GAME_TIMEOUT, 10) || 3600;
const MAX_SAVE_BYTES = 16 * 1024 * 1024;  // matches account_service.js cloud file cap
if (!Number.isInteger(MAX_PLAYERS) || MAX_PLAYERS < 1 || MAX_PLAYERS > 32) throw new Error('MAX_PLAYERS must be between 1 and 32.');
const security = new TransportSecurity({ maxPlayers: MAX_PLAYERS });
const allowedOrigin = req => originAllowed(req.headers.origin, req.headers.host, CORS_ORIGIN, process.env.NODE_ENV !== 'production');

fs.mkdirSync(DATA_DIR, { recursive: true });

// ---------- Simple JSON file database ----------
const DB_FILE = path.join(DATA_DIR, 'mc3_relay.json');

function loadDB() {
  try {
    const raw = fs.readFileSync(DB_FILE, 'utf8');
    return JSON.parse(raw);
  } catch (e) {
    if (e.code !== 'ENOENT') throw e; // Preserve an unreadable database instead of erasing accounts.
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
  atomicJson(DB_FILE, db);
}

let db = loadDB();
for (const table of ['users','games','game_players','cloud_saves','career_progress','car_setups','match_history'])
  if (!Array.isArray(db[table])) db[table] = [];

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
    { expiresIn: '30m' }
  );
}

function verifyToken(token) {
  try {
    return jwt.verify(token, JWT_SECRET, { algorithms: ['HS256'] });
  } catch (e) {
    return null;
  }
}

// ---------- Express App ----------
const app = express();
app.disable('x-powered-by');
app.set('trust proxy', false); // Rate limits use the direct peer, never an untrusted forwarded address.
app.use((req, res, next) => {
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('X-Frame-Options', 'DENY');
  res.setHeader('Referrer-Policy', 'no-referrer');
  if (!allowedOrigin(req)) return res.status(403).json({ error: 'Origin is not allowed.' });
  next();
});
app.use(cors((req, callback) => callback(null, { origin: !!req.headers.origin && allowedOrigin(req), credentials: true })));

// Rate limiting
const apiLimiter = rateLimit({
  windowMs: 15 * 60 * 1000,
  max: 100,
  message: { error: 'Too many requests, please try again later.' }
});
app.use('/api/', apiLimiter);
app.use('/api/cloud', express.json({ limit: '24mb' }));
app.use(express.json({ limit: '10kb' }));
app.use((error, req, res, next) => {
  if (error.type === 'entity.too.large') return res.status(413).json({ error: 'Request body exceeds its limit.' });
  if (error instanceof SyntaxError) return res.status(400).json({ error: 'Malformed JSON request.' });
  next(error);
});
app.use('/api/', (req, res, next) => {
  if (['POST', 'PUT', 'PATCH'].includes(req.method) && (!req.body || typeof req.body !== 'object' || Array.isArray(req.body)))
    return res.status(400).json({ error: 'A JSON object is required.' });
  next();
});
installAccountRoutes(app, { db, dataDirectory: DATA_DIR, persist: saveDB, makeToken, verifyToken });
app.use(express.static(path.join(__dirname, '../browser'), { index: 'index.html' }));

// Auth routes are supplied exclusively by account_service (salted scrypt + legacy migration).
function authMiddleware(req, res, next) {
  const header = req.headers['authorization'];
  if (typeof header !== 'string' || header.length > 8192 || !header.startsWith('Bearer ')) {
    return res.status(401).json({ error: 'Authentication required' });
  }
  const token = header.slice(7);
  const payload = verifyToken(token);
  const user = payload && typeof payload === 'object' && typeof payload.id === 'string' && findById('users', payload.id);
  if (!user) {
    return res.status(401).json({ error: 'Invalid or expired token' });
  }
  req.user = { id: user.id, username: user.username };
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
  const checked = validateRoomRequest({ name, maxPlayers: max_players, region }, MAX_PLAYERS);
  if (!checked.ok) return res.status(400).json({ error: checked.reason });
  if (!canCreateGame(req.user.id)) return res.status(429).json({ error: 'Room quota exceeded.' });

  const id = uuidv4();
  const game = {
    id,
    name: checked.room.name,
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
  if (!Number.isInteger(game.max_players) || game.max_players < 1 || game.max_players > MAX_PLAYERS || players.length >= game.max_players) {
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
  const playerIdx = db.game_players.findIndex(p => p.game_id === gameId && p.user_id === req.user.id);
  if (playerIdx === -1) {
    return res.json({ ok: true });
  }
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
    newHost.role = 'host';
    const newHostUser = findBy('users', 'id', newHost.user_id);
    updateById('games', gameId, { host_id: newHost.user_id, host_username: newHostUser ? newHostUser.username : newHost.username, player_count: remaining.length });
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

function validateSaveData(save_data) {
  // save_data is caller-supplied opaque JSON, but we sanity-check its shape so a
  // corrupted payload cannot bloat the database or be misread by the native game.
  if (!save_data || typeof save_data !== 'object' || Array.isArray(save_data)) return 'save_data must be a JSON object';
  const serialized = JSON.stringify(save_data);
  if (Buffer.byteLength(serialized) > MAX_SAVE_BYTES) return 'save_data exceeds the 16 MiB save cap';
  const saveMoney = save_data.money;
  if (saveMoney !== undefined && (!Number.isSafeInteger(saveMoney) || saveMoney < 0)) return 'save_data.money must be a non-negative integer';
  if (saveMoney !== undefined && save_data.playtime !== undefined &&
      (!Number.isSafeInteger(save_data.playtime) || save_data.playtime < 0)) return 'save_data.playtime must be a non-negative integer';
  return null;
}

app.post('/api/saves', authMiddleware, (req, res) => {
  const { slot, save_name, save_data, thumbnail, playtime, money, rating } = req.body;
  if (slot === undefined || save_data === undefined) {
    return res.status(400).json({ error: 'slot and save_data required' });
  }

  const slotNum = typeof slot === 'string' && /^\d{1,2}$/.test(slot) ? Number(slot) : slot;
  const saveErr = validateSaveData(save_data);
  if (!Number.isInteger(slotNum) || slotNum < 1 || slotNum > 16 ||
      (save_name !== undefined && (typeof save_name !== 'string' || save_name.length > 80)) ||
      (thumbnail !== undefined && thumbnail !== null && (typeof thumbnail !== 'string' || thumbnail.length > 8192)) ||
      [playtime, money, rating].some(value => value !== undefined && (!Number.isSafeInteger(value) || value < 0)) ||
      saveErr)
    return res.status(400).json({ error: saveErr || 'Invalid save slot or metadata.' });
  if (money !== undefined && save_data.money !== undefined && save_data.money !== money) {
    return res.status(400).json({ error: 'save_data.money does not match top-level money metadata.' });
  }
  const existing = filterBy('cloud_saves', 'user_id', req.user.id).find(s => s.slot === slotNum);
  const nowTs = now();
  try {
    const payload = {
      save_name: save_name || 'Untitled',
      save_data: JSON.stringify(save_data),
      thumbnail: thumbnail || null,
      playtime: playtime || 0,
      money: money || 0,
      rating: rating || 0,
      updated_at: nowTs,
      native_ack: false,
    };
    if (existing) {
      Object.assign(existing, payload);
      saveDB(db);
      res.json({ success: true, slot: slotNum, message: 'Save updated', native_ack: existing.native_ack });
    } else {
      const id = uuidv4();
      push('cloud_saves', {
        id,
        user_id: req.user.id,
        slot: slotNum,
        ...payload,
        created_at: nowTs,
        native_ack: false,
      });
      saveDB(db);
      res.json({ success: true, id, slot: slotNum, message: 'Save created', native_ack: false });
    }
  } catch (e) {
    res.status(500).json({ error: 'Failed to save game' });
  }
});

// Native save-import acknowledgment. The native game (or its stream proxy) calls
// this after it has successfully imported/loaded a cloud save, so HTTP 200 on
// /api/saves no longer means "game loaded the save" � only native_ack == true does.
app.post('/api/saves/:slot/ack', authMiddleware, (req, res) => {
  const { slot } = req.params;
  const slotNum = typeof slot === 'string' && /^\d{1,2}$/.test(slot) ? Number(slot) : slot;
  if (!Number.isInteger(slotNum) || slotNum < 1 || slotNum > 16) {
    return res.status(400).json({ error: 'Invalid save slot.' });
  }
  const save = db.cloud_saves.find(s => s.user_id === req.user.id && s.slot === slotNum);
  if (!save) {
    return res.status(404).json({ error: 'Save not found.' });
  }
  save.native_ack = true;
  save.native_acked_at = now();
  saveDB(db);
  res.json({ success: true, slot: slotNum, native_ack: true, native_acked_at: save.native_acked_at });
});

app.delete('/api/saves/:slot', authMiddleware, (req, res) => {
  const { slot } = req.params;
  const idx = db.cloud_saves.findIndex(s => s.user_id === req.user.id && s.slot === parseInt(slot));
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
  let cities = career.cities_unlocked || [];
  if (typeof cities === 'string') { try { cities = JSON.parse(cities); } catch (_) { cities = []; } }
  res.json({ career: { ...career, cities_unlocked: Array.isArray(cities) ? cities : [] } });
});

app.post('/api/career', authMiddleware, (req, res) => {
  const { total_races, races_won, total_money_earned, current_money, cars_owned, cities_unlocked, difficulty, last_played } = req.body;
  if ([total_races, races_won, total_money_earned, current_money, cars_owned, last_played].some(value => value !== undefined && (!Number.isSafeInteger(value) || value < 0)) ||
      (difficulty !== undefined && !['easy', 'medium', 'hard', 'custom'].includes(difficulty)) ||
      (cities_unlocked !== undefined && (!Array.isArray(cities_unlocked) || cities_unlocked.length > 16 || cities_unlocked.some(city => typeof city !== 'string' || !/^[A-Za-z0-9 _-]{1,50}$/.test(city)))))
    return res.status(400).json({ error: 'Invalid career metadata.' });
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
  res.json({ setups: setups.map(setup => {
    let data = setup.setup_data || {};
    if (typeof data === 'string') { try { data = JSON.parse(data); } catch (_) { data = {}; } }
    return { ...setup, setup_data: data };
  }) });
});

app.post('/api/car-setups', authMiddleware, (req, res) => {
  const { car_name, setup_data, is_equipped } = req.body;
  if (typeof car_name !== 'string' || !car_name.trim() || car_name.length > 80 || !setup_data || typeof setup_data !== 'object' || Array.isArray(setup_data) ||
      (is_equipped !== undefined && typeof is_equipped !== 'boolean')) {
    return res.status(400).json({ error: 'car_name and setup_data required' });
  }
  if (filterBy('car_setups', 'user_id', req.user.id).length >= 64) return res.status(429).json({ error: 'Car setup quota exceeded.' });
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
server.headersTimeout = 15000;
server.requestTimeout = 30000;
server.maxHeadersCount = 64;

// WebSocket server for real-time relay
const wss = new WebSocketServer({ server, path: '/ws', ...security.wsOptions, verifyClient: info => allowedOrigin(info.req) });

// Connected clients: ws -> { id, type ('game' | 'browser'), userId, gameId, role, username, color }
const clients = new Map();

// Games in memory (synced with DB)
const activeGames = new Map(); // gameId -> { id, name, hostId, players: Map(wsId -> client), spectators: Set(wsId) }
function playerCount(game) { return new Set([...game.players.values()].map(player => player.id)).size; }
function canCreateGame(userId) {
  const all = new Map(db.games.filter(game => ['waiting', 'playing'].includes(game.status)).map(game => [game.id, game.host_id]));
  for (const game of activeGames.values()) all.set(game.id, game.hostId);
  return all.size < security.limits.maxRooms && [...all.values()].filter(owner => owner === userId).length < security.limits.maxRoomsPerUser;
}

function broadcastToGame(gameId, message, excludeWs = null) {
  const game = activeGames.get(gameId);
  if (!game) return;
  const targets = new Set([...game.players.keys(), ...game.spectators.keys()]);
  if (excludeWs) targets.delete(excludeWs);
  const data = typeof message === 'string' ? JSON.parse(message) : message;
  for (const ws of targets) {
    security.send(ws, data);
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
      player_count: playerCount(game),
      region: game.region
    });
  }
  const msg = { type: 'gameList', games };
  for (const ws of clients.keys()) {
    security.send(ws, msg);
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
  security.removeRoom(gameId);
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
  security.leaveRoom(ws);
  const game = activeGames.get(client.gameId);
  if (!game) { client.gameId = null; client.role = null; client.color = null; return; }

  const wasHost = game.hostId === client.userId;
  game.players.delete(ws);
  game.spectators.delete(ws);

  // Update DB
  const userStillPresent = [...game.players.values()].some(player => player.id === client.userId);
  if (!userStillPresent) db['game_players'] = db['game_players'].filter(p => !(p.game_id === client.gameId && p.user_id === client.userId));
  saveDB(db);

  if (game.players.size === 0 && game.spectators.size === 0) {
    removeGame(client.gameId);
    db['games'] = db['games'].filter(g => g.id !== client.gameId);
    db['game_players'] = db['game_players'].filter(p => p.game_id !== client.gameId);
    saveDB(db);
  } else {
    const gameObj = findBy('games', 'id', client.gameId);
    if (gameObj) {
      gameObj.player_count = playerCount(game);
      updateById('games', client.gameId, { player_count: playerCount(game) });
    }

    if (wasHost && !userStillPresent && game.players.size > 0) {
      const [newHostWs] = game.players.keys();
      const newHost = game.players.get(newHostWs);
      game.hostId = newHost.id;
      game.hostUsername = newHost.username;
      // Update game_players role
      const npIdx = db.game_players.findIndex(p => p.game_id === client.gameId && p.user_id === newHost.id);
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
      updateById('games', client.gameId, { player_count: playerCount(game) });
    }

    broadcastToGame(client.gameId, {
      type: 'player_left',
      username: client.username,
      id: client.id,
      playerCount: playerCount(game)
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

  security.attach(ws, req, data => {
    try {
      switch (data.type) {
        case 'auth': {
          const token = data.token;
          const payload = verifyToken(token);
          if (!payload || typeof payload !== 'object' || typeof payload.id !== 'string' || typeof payload.username !== 'string' || !findById('users', payload.id)) {
            security.send(ws, { type: 'error', error: 'Invalid token' });
            security.reject(ws, { code: 1008, reason: 'Invalid token' });
            return;
          }
          const authenticated = security.markAuthenticated(ws, payload.id);
          if (!authenticated.ok) { security.reject(ws, authenticated); return; }
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
          security.send(ws, { type: 'auth_ok', user: { id: payload.id, username: payload.username } });
          break;
        }

        case 'join': {
          // Guest attachment (no token). Local adapters (game/stream proxies) identify
          // themselves as 'game' clients with a display name; browsers may also use it.
          if (client) { security.send(ws, { type: 'error', error: 'Already authenticated' }); return; }
          const name = data.name || 'Guest';
          const guestId = 'guest-' + clientId;
          const authenticated = security.markAuthenticated(ws, guestId);
          if (!authenticated.ok) { security.reject(ws, authenticated); return; }
          client = {
            id: clientId,
            type: data.clientType || 'browser',
            userId: guestId,
            username: name,
            gameId: null,
            role: null,
            color: null
          };
          clients.set(ws, client);
          security.send(ws, { type: 'auth_ok', user: { id: guestId, username: name } });
          break;
        }

        case 'create_game': {
          if (!client) return;
          if (client.gameId || !canCreateGame(client.userId)) { security.send(ws, { type: 'error', error: 'Leave the current room or wait for room quota.' }); return; }
          const id = uuidv4().slice(0, 8);
          const reserved = security.createRoom(ws, id, data);
          if (!reserved.ok) { security.send(ws, { type: 'error', error: reserved.reason }); return; }
          const color = assignColor();
          const game = createGame(id, data.name, client.userId, client.username, data.maxPlayers || MAX_PLAYERS, data.region || 'global');
          client.gameId = id;
          client.role = 'host';
          client.color = color;
          game.players.set(ws, { id: client.userId, username: client.username, role: 'host', color, ready: false });
          security.send(ws, {
            type: 'game_created',
            game: { id, name: data.name, hostId: client.userId, hostUsername: client.username, maxPlayers: game.maxPlayers, playerCount: 1, region: game.region },
            role: 'host',
            color
          });
          sendGameListUpdate();
          break;
        }

        case 'join_game': {
          if (!client) return;
          if (client.gameId) { security.send(ws, { type: 'error', error: 'Leave the current room first.' }); return; }
          const game = activeGames.get(data.gameId);
          if (!game || game.status !== 'waiting') {
            security.send(ws, { type: 'error', error: 'Game not found or not waiting' });
            return;
          }
          const alreadyPresent = [...game.players.values()].some(player => player.id === client.userId);
          if (!alreadyPresent && playerCount(game) >= game.maxPlayers) {
            security.send(ws, { type: 'error', error: 'Game is full' });
            return;
          }
          const joined = security.joinRoom(ws, data.gameId);
          if (!joined.ok) { security.send(ws, { type: 'error', error: joined.reason }); return; }
          const color = assignColor();
          client.gameId = data.gameId;
          client.role = 'player';
          client.color = color;
          game.players.set(ws, { id: client.userId, username: client.username, role: 'player', color, ready: false });
          const gObj = findBy('games', 'id', data.gameId);
          if (gObj) {
            gObj.player_count = playerCount(game);
            updateById('games', data.gameId, { player_count: playerCount(game) });
          }
          if (!db.game_players.some(player => player.game_id === data.gameId && player.user_id === client.userId)) push('game_players', {
            game_id: data.gameId,
            user_id: client.userId,
            username: client.username,
            role: 'player',
            joined_at: now()
          });
          security.send(ws, {
            type: 'game_joined',
            game: {
              id: game.id,
              name: game.name,
              hostId: game.hostId,
              hostUsername: game.hostUsername,
              maxPlayers: game.maxPlayers,
              playerCount: playerCount(game),
              region: game.region
            },
            role: 'player',
            color
          });
          broadcastToGame(data.gameId, {
            type: 'player_joined',
            username: client.username,
            color,
            playerCount: playerCount(game)
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
          if (!game || game.status !== 'waiting' || game.hostId !== client.userId) return;
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
          const msg = {
            type: 'input',
            from: client.username,
            fromId: client.id, // relay-assigned, unlike the self-chosen name: proxies route by this
            data: data.data,
            ts: Date.now()
          };
          // `to` (a relay client id) sends a datagram to one player in the room; otherwise everyone gets it.
          const to = typeof data.to === 'string' ? data.to : null;
          for (const gws of game.players.keys()) {
            if (gws === ws) continue; // never echo a peer's own datagram back to it
            const peer = clients.get(gws);
            if (to !== null && peer?.id !== to) continue;
            if (gws.readyState === 1 && peer?.type === 'game') {
              security.send(gws, msg);
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
          security.send(ws, { type: 'pong', ts: data.ts });
          break;
        case 'list':
          sendGameListUpdate();
          break;
      }
    } catch (e) {
      console.error('WS message handler failed.');
      security.reject(ws, { code: 1011, reason: 'Message handler failed' });
    }
  });

  ws.on('close', () => {
    try { if (client && client.gameId) leaveGame(client, ws); }
    catch (_) { console.error('WS room cleanup failed.'); }
    finally { clients.delete(ws); }
  });

  ws.on('error', (e) => {
    console.error('WS error:', e.message);
  });
});

// Heartbeat
const interval = setInterval(() => security.tick(), 5000);

wss.on('close', () => clearInterval(interval));

// Express catches synchronous route errors; return bounded JSON instead of leaking a stack or input.
app.use((error, req, res, next) => {
  if (res.headersSent) return next(error);
  console.error('HTTP request failed.');
  res.status(500).json({ error: 'Request could not be completed.' });
});

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
