/* Account/cloud browser shell. Saves/configs stay opaque bytes; no invented game profiles or save writers.
   Short access tokens live only in memory. Remembered sign-in belongs to the server's HTTPOnly device cookie. */
(function (root, factory) {
  const api = factory();
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.MC3Account = api;
})(typeof globalThis !== 'undefined' ? globalThis : this, function () {
  'use strict';
  const MAX_FILES = 256, MAX_BYTES = 16 * 1024 * 1024, MAX_SETTINGS = 128 * 1024;
  const ACCOUNT = /^[0-9a-f-]{36}$/i, HEX = /^[0-9a-f]{64}$/;
  const clone = value => JSON.parse(JSON.stringify(value));
  const plain = value => !!value && !Array.isArray(value) && Object.getPrototypeOf(value) === Object.prototype;
  const textBytes = value => new TextEncoder().encode(value).byteLength;
  class AccountError extends Error {
    constructor(message, status = 0) { super(message); this.name = 'AccountError'; this.status = status; }
  }
  function canonicalPath(value) {
    if (typeof value !== 'string' || !value || value.length > 180 || /[\\:\x00-\x1f<>"|?*]/.test(value)) return false;
    const parts = value.split('/');
    if (parts.some(p => !p || p === '.' || p === '..' || /[. ]$/.test(p) || /^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)/i.test(p))) return false;
    if (value === 'settings.toml' || value === 'settings.ini') return true;
    if (parts[0] === 'config') return parts.length >= 2 && parts.length <= 4 && /\.(toml|ini|json|cfg)$/i.test(value);
    return parts[0] === 'hdd' && parts.length >= 2 && parts.length <= 8;
  }
  function importPath(file) {
    const relative = String(file.webkitRelativePath || file.name || '');
    const parts = relative.split('/');
    const known = parts.findIndex(p => /^hdd\d*$/i.test(p) || p === 'config' || p === 'settings.toml' || p === 'settings.ini');
    if (known >= 0) {
      const tail = parts.slice(known); if (/^hdd\d*$/i.test(tail[0])) tail[0] = 'hdd';
      const path = tail.join('/'); if (canonicalPath(path)) return path;
    }
    const name = String(file.name || '');
    const path = /\.(toml|ini|cfg|json)$/i.test(name) ? 'config/' + name : 'hdd/' + name;
    if (!canonicalPath(path)) throw new AccountError('Choose a save/config file with a portable filename.');
    return path;
  }
  function bytesToBase64(bytes) {
    let binary = ''; for (let at = 0; at < bytes.length; at += 32768) binary += String.fromCharCode(...bytes.subarray(at, at + 32768));
    if (typeof btoa === 'function') return btoa(binary);
    return Buffer.from(bytes).toString('base64');
  }
  function base64ToBytes(encoded) {
    if (typeof encoded !== 'string' || encoded.length > Math.ceil(MAX_BYTES / 3) * 4 || encoded.length % 4 || /[^A-Za-z0-9+/=]/.test(encoded))
      throw new AccountError('Cloud file encoding is invalid.');
    const binary = typeof atob === 'function' ? atob(encoded) : Buffer.from(encoded, 'base64').toString('binary');
    const bytes = Uint8Array.from(binary, c => c.charCodeAt(0));
    if (bytesToBase64(bytes) !== encoded) throw new AccountError('Cloud file encoding is not canonical.');
    return bytes;
  }
  async function sha256(bytes, cryptoProvider = globalThis.crypto) {
    if (!cryptoProvider || !cryptoProvider.subtle) throw new AccountError('Verified cloud files require HTTPS or localhost.');
    return Array.from(new Uint8Array(await cryptoProvider.subtle.digest('SHA-256', bytes)), b => b.toString(16).padStart(2, '0')).join('');
  }
  async function encodeFile(path, bytes, cryptoProvider) {
    if (!canonicalPath(path)) throw new AccountError('Unsupported virtual save/config path.');
    bytes = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
    if (bytes.byteLength > MAX_BYTES) throw new AccountError('Cloud files are limited to 16 MiB in total.');
    return { path, encoding: 'base64', data: bytesToBase64(bytes), sha256: await sha256(bytes, cryptoProvider), size: bytes.byteLength };
  }
  async function validateCloud(value, accountId, cryptoProvider) {
    if (!plain(value) || value.schema !== 1 || value.account_id !== accountId || !ACCOUNT.test(accountId) ||
        !Number.isSafeInteger(value.revision) || value.revision < 0 || !Array.isArray(value.files) || value.files.length > MAX_FILES || !plain(value.settings))
      throw new AccountError('The cloud snapshot does not belong to this account or has an unsupported format.');
    if (textBytes(JSON.stringify(value.settings)) > MAX_SETTINGS) throw new AccountError('Cloud settings exceed their limit.');
    const names = new Set(); let total = 0; const files = [];
    for (const file of value.files) {
      if (!plain(file) || !canonicalPath(file.path) || file.encoding !== 'base64' || !HEX.test(file.sha256 || '')) throw new AccountError('Cloud file metadata is invalid.');
      const key = file.path.toLowerCase(); if (names.has(key)) throw new AccountError('Cloud contains duplicate file paths.'); names.add(key);
      const bytes = base64ToBytes(file.data); total += bytes.byteLength;
      if (total > MAX_BYTES || await sha256(bytes, cryptoProvider) !== file.sha256) throw new AccountError('Cloud file integrity or size check failed.');
      files.push({ path: file.path, encoding: 'base64', data: file.data, sha256: file.sha256, size: bytes.byteLength });
    }
    return { schema: 1, account_id: accountId, revision: value.revision, files, settings: clone(value.settings), updated_at: value.updated_at || null };
  }
  function resolveEndpoints(page) {
    const url = new URL(page), secure = url.protocol === 'https:';
    const web = url.protocol === 'http:' || secure;
    const api = web ? url.origin : 'http://localhost:8080';
    const relayDefault = api.replace(/^http/, 'ws');
    const streamDefault = web ? `${secure ? 'wss:' : 'ws:'}//${url.hostname.includes(':') ? '[' + url.hostname.replace(/[\[\]]/g, '') + ']' : url.hostname}:8766` : 'ws://localhost:8766';
    const allowed = (query, fallback) => {
      if (!query) return fallback;
      try {
        const target = new URL(query), host = target.hostname.replace(/[\[\]]/g, '').toLowerCase();
        const loopback = ['localhost', '127.0.0.1', '::1'].includes(host);
        const same = target.host === new URL(relayDefault).host;
        if (!['ws:', 'wss:'].includes(target.protocol) || target.username || target.password || (!same && !loopback) || (secure && !loopback && target.protocol !== 'wss:')) return fallback;
        return target.href;
      } catch (_) { return fallback; }
    };
    return { api, relay: allowed(url.searchParams.get('relay'), relayDefault), stream: allowed(url.searchParams.get('stream'), streamDefault) };
  }
  function scopedKey(origin, accountId, kind = 'preferences') {
    if (!ACCOUNT.test(accountId)) throw new AccountError('Invalid account identity.');
    return `mc3:account:${encodeURIComponent(origin)}:${accountId}:${kind}:v1`;
  }
  function sanitizePreferences(value) {
    value = plain(value) ? value : {};
    return {
      music: value.music !== false,
      sound: value.sound !== false,
      deadzone: Math.max(0.05, Math.min(0.5, Number.isFinite(value.deadzone) ? value.deadzone : 0.15)),
      uiScale: Math.max(0.85, Math.min(1.3, Number.isFinite(value.uiScale) ? value.uiScale : 1)),
      showChat: value.showChat !== false
    };
  }
  class DraftVault {
    constructor(indexedDB) { this.indexedDB = indexedDB; this.memory = new Map(); }
    async save(key, value) {
      this.memory.set(key, clone(value));
      if (!this.indexedDB) return false;
      try {
        const database = await new Promise((resolve, reject) => {
          const request = this.indexedDB.open('mc3-account-drafts-v1', 1);
          request.onupgradeneeded = () => request.result.createObjectStore('drafts');
          request.onsuccess = () => resolve(request.result); request.onerror = () => reject(request.error);
        });
        try { await new Promise((resolve, reject) => {
          const transaction = database.transaction('drafts', 'readwrite'); transaction.objectStore('drafts').put(value, key);
          transaction.oncomplete = resolve; transaction.onerror = () => reject(transaction.error); transaction.onabort = () => reject(transaction.error);
        }); } finally { database.close(); }
        return true;
      } catch (_) { return false; }
    }
    async load(key) {
      if (this.memory.has(key)) return clone(this.memory.get(key));
      if (!this.indexedDB) return null;
      try {
        const database = await new Promise((resolve, reject) => {
          const request = this.indexedDB.open('mc3-account-drafts-v1', 1);
          request.onupgradeneeded = () => request.result.createObjectStore('drafts');
          request.onsuccess = () => resolve(request.result); request.onerror = () => reject(request.error);
        });
        try { return await new Promise((resolve, reject) => {
          const request = database.transaction('drafts').objectStore('drafts').get(key);
          request.onsuccess = () => resolve(request.result || null); request.onerror = () => reject(request.error);
        }); } finally { database.close(); }
      } catch (_) { return null; }
    }
  }
  class Client {
    constructor({ apiBase, fetch: fetcher, storage, crypto, vault, onSession = () => {}, onChange = () => {} }) {
      this.apiBase = apiBase.replace(/\/$/, ''); this.origin = new URL(this.apiBase).origin;
      this.fetch = fetcher; this.storage = storage; this.crypto = crypto || globalThis.crypto;
      this.vault = vault || new DraftVault(globalThis.indexedDB); this.onSession = onSession; this.onChange = onChange;
      this.session = null; this.epoch = 0; this.authSerial = 0; this.refreshing = null; this.cloud = null; this.draft = null; this.conflict = null; this.retained = [];
      this.preferences = sanitizePreferences({}); this.syncTimer = null; this.saving = false; this.settingsAgain = false; this.fullSaveAgain = false; this.dirty = false;
      // Retire the former persisted bearer credentials without reading/reusing them.
      for (const key of ['mc3_token', 'mc3_username', 'mc3_userId']) try { storage && storage.removeItem(key); } catch (_) {}
    }
    key(kind) { return scopedKey(this.origin, this.session.user.id, kind); }
    readPreferences() {
      try { return sanitizePreferences(JSON.parse(this.storage && this.storage.getItem(this.key()))); } catch (_) { return sanitizePreferences({}); }
    }
    applySession(data) {
      if (!plain(data) || typeof data.token !== 'string' || !data.token || data.token.length > 8192 || !plain(data.user) || !ACCOUNT.test(data.user.id) || typeof data.user.username !== 'string')
        throw new AccountError('Sign-in response is invalid.');
      const changed = !this.session || this.session.user.id !== data.user.id;
      if (changed) {
        ++this.epoch; clearTimeout(this.syncTimer); this.cloud = this.draft = this.conflict = null; this.retained = []; this.dirty = false;
        this.saving = this.settingsAgain = this.fullSaveAgain = false; this.lastError = '';
      }
      this.session = { token: data.token, user: { id: data.user.id, username: data.user.username.slice(0, 100) }, remembered: data.remembered === true };
      if (changed) this.preferences = this.readPreferences();
      this.onSession(this.session); this.onChange(); return this.session;
    }
    clearSession() {
      ++this.epoch; ++this.authSerial; clearTimeout(this.syncTimer); this.session = null; this.cloud = this.draft = this.conflict = null;
      this.retained = []; this.preferences = sanitizePreferences({}); this.dirty = false; this.saving = this.settingsAgain = this.fullSaveAgain = false;
      this.onSession(null); this.onChange();
    }
    async response(response, limit = 65536) {
      let body;
      try {
        if (response.body && response.body.getReader) {
          const reader = response.body.getReader(), chunks = []; let size = 0;
          try { for (;;) { const next = await reader.read(); if (next.done) break; size += next.value.byteLength;
            if (size > limit) { await reader.cancel(); throw new Error('Response exceeded limit.'); } chunks.push(next.value); }
          } finally { reader.releaseLock(); }
          const bytes = new Uint8Array(size); let at = 0; for (const chunk of chunks) { bytes.set(chunk, at); at += chunk.byteLength; }
          body = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes));
        } else { body = await response.json(); if (textBytes(JSON.stringify(body)) > limit) throw new Error('Response exceeded limit.'); }
      } catch (_) { throw new AccountError('Server returned an unreadable or oversized response.', response.status); }
      if (!response.ok) throw new AccountError(typeof body.error === 'string' ? body.error.slice(0, 500) : 'Account request failed.', response.status);
      return body;
    }
    async refresh() {
      if (!this.refreshing) this.refreshing = (async () => {
        const serial = this.authSerial, epoch = this.epoch;
        const response = await this.fetch(this.apiBase + '/api/session', { method: 'POST', credentials: 'include', headers: { 'Content-Type': 'application/json' }, body: '{}' });
        if (serial !== this.authSerial || epoch !== this.epoch) return null;
        if (response.status === 401) { this.clearSession(); return null; }
        const result = await this.response(response);
        if (serial !== this.authSerial || epoch !== this.epoch) return null;
        return this.applySession(result);
      })().finally(() => { this.refreshing = null; });
      return this.refreshing;
    }
    async authenticate(mode, username, password, remember, confirmation) {
      if (!['login', 'register'].includes(mode) || !/^[A-Za-z0-9_ -]{3,20}$/.test(username.trim())) throw new AccountError('Use a 3–20 character player name.');
      if (typeof password !== 'string' || !password || password.length > 256 || (mode === 'register' && password.length < 8)) throw new AccountError('New passwords must contain 8–256 characters.');
      if (mode === 'register' && password !== confirmation) throw new AccountError('The password confirmation does not match.');
      const serial = ++this.authSerial;
      const response = await this.fetch(this.apiBase + '/api/' + mode, {
        method: 'POST', credentials: 'include', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username: username.trim(), password, remember: remember === true })
      });
      const result = await this.response(response);
      if (serial !== this.authSerial) throw new AccountError('This sign-in request was superseded.');
      return this.applySession(result); // Password is intentionally never trimmed/stored.
    }
    async logout() {
      try { await this.request('logout', 'POST', {}, false); } finally { this.clearSession(); }
    }
    async request(endpoint, method = 'GET', body = null, retry = true) {
      if (!/^[A-Za-z0-9_/?=&.-]+$/.test(endpoint) || endpoint.includes('..')) throw new AccountError('Invalid API route.');
      const user = this.session && this.session.user.id;
      const headers = {}; if (this.session) headers.Authorization = 'Bearer ' + this.session.token;
      if (body !== null) headers['Content-Type'] = 'application/json';
      const response = await this.fetch(this.apiBase + '/api/' + endpoint, {
        method, credentials: 'include', headers, ...(body !== null ? { body: JSON.stringify(body) } : {})
      });
      if (response.status === 401 && retry && endpoint !== 'session') {
        const recovered = await this.refresh();
        if (!recovered) throw new AccountError('Sign in to continue.', 401);
        if (user && user !== recovered.user.id) throw new AccountError('The account changed. The prior account draft was not sent.', 409);
        return this.request(endpoint, method, body, false);
      }
      return this.response(response, endpoint === 'cloud' ? 24 * 1024 * 1024 : 65536);
    }
    async loadCloud() {
      if (!this.session) throw new AccountError('Sign in to access cloud files.', 401);
      const epoch = this.epoch, id = this.session.user.id;
      const cloud = await validateCloud(await this.request('cloud'), id, this.crypto);
      if (epoch !== this.epoch) return null;
      const saved = await this.vault.load(this.key('draft'));
      if (epoch !== this.epoch) return null;
      this.cloud = cloud;
      if (saved && saved.account_id === id && saved.dirty) {
        try {
          this.draft = await validateCloud(saved.snapshot, id, this.crypto); this.dirty = true;
          if (this.draft.revision !== cloud.revision) this.conflict = { local: clone(this.draft), remote: clone(cloud) };
        } catch (_) { this.draft = clone(cloud); }
      } else if (!this.dirty) this.draft = clone(cloud);
      if (plain(cloud.settings.browser) && !this.dirty) {
        this.preferences = sanitizePreferences(cloud.settings.browser); this.persistPreferences();
      }
      const backup = await this.vault.load(this.key('conflict'));
      if (backup && backup.account_id === id) this.retained = [backup];
      this.onChange(); return clone(cloud);
    }
    persistPreferences() { try { if (this.session && this.storage) this.storage.setItem(this.key(), JSON.stringify(this.preferences)); } catch (_) {} }
    async preserveDraft() {
      if (!this.session || !this.draft) return false;
      return this.vault.save(this.key('draft'), { account_id: this.session.user.id, dirty: this.dirty, snapshot: clone(this.draft) });
    }
    async importFiles(files) {
      if (!this.session || !this.draft) throw new AccountError('Load this account cloud snapshot before importing files.');
      const epoch = this.epoch, merged = new Map(this.draft.files.map(f => [f.path.toLowerCase(), f]));
      const incoming = new Set(); let imported = 0;
      for (const file of files) {
        const path = importPath(file), key = path.toLowerCase();
        if (incoming.has(key)) throw new AccountError('Selected files contain duplicate virtual paths.'); incoming.add(key);
        if (!Number.isSafeInteger(file.size) || file.size < 0 || file.size > MAX_BYTES) throw new AccountError('Selected file exceeds the cloud limit.');
        const encoded = await encodeFile(path, await file.arrayBuffer(), this.crypto);
        if (epoch !== this.epoch) throw new AccountError('Account changed; selected files were not assigned to another account.');
        merged.set(key, encoded); ++imported;
      }
      const next = { ...this.draft, files: Array.from(merged.values()).sort((a,b) => a.path.localeCompare(b.path)) };
      const verified = await validateCloud(next, this.session.user.id, this.crypto);
      if (epoch !== this.epoch) return 0;
      this.draft = verified; this.dirty = true; await this.preserveDraft(); this.onChange(); return imported;
    }
    updatePreferences(patch) {
      this.preferences = sanitizePreferences({ ...this.preferences, ...patch }); this.persistPreferences();
      if (!this.draft) { this.onChange(); return; }
      this.draft.settings = { ...this.draft.settings, browser: clone(this.preferences) };
      this.dirty = true; this.preserveDraft(); clearTimeout(this.syncTimer);
      const epoch = this.epoch;
      this.syncTimer = setTimeout(() => { if (epoch === this.epoch && !this.conflict) this.saveCloud({ settingsOnly: true }).catch(error => { this.lastError = error.message; this.onChange(); }); }, 800);
      this.onChange();
    }
    async saveCloud({ replaceConflict = false, settingsOnly = false } = {}) {
      if (!this.session || !this.draft || !this.cloud) throw new AccountError('No account cloud snapshot is ready.');
      if (this.conflict && !replaceConflict) throw new AccountError('Choose how to keep the two conflicting versions.', 409);
      if (this.saving) { if (settingsOnly) this.settingsAgain = true; else this.fullSaveAgain = true; return null; }
      const epoch = this.epoch, id = this.session.user.id;
      const local = clone(this.draft), base = replaceConflict && this.conflict ? this.conflict.remote.revision : this.cloud.revision;
      // Debouncing a preference must not upload newly imported save files before the player's Save choice.
      if (settingsOnly) local.files = clone(this.cloud.files);
      this.saving = true; this.lastError = ''; this.onChange();
      try {
        if (replaceConflict) await this.retainConflict();
        const result = await this.request('cloud', 'PUT', { base_revision: base, files: local.files, settings: local.settings });
        if (epoch !== this.epoch) return null;
        if (!plain(result) || !Number.isSafeInteger(result.revision) || result.revision !== base + 1) throw new AccountError('Cloud acknowledgement is invalid.');
        this.cloud = { ...local, revision: result.revision, updated_at: result.updated_at || null };
        const unchanged = JSON.stringify(this.draft.files) === JSON.stringify(local.files) && JSON.stringify(this.draft.settings) === JSON.stringify(local.settings);
        if (unchanged) { this.draft = clone(this.cloud); this.dirty = false; }
        else this.draft.revision = result.revision;
        this.conflict = null; await this.preserveDraft(); return result;
      } catch (error) {
        if (epoch === this.epoch && error.status === 409) {
          const remote = await validateCloud(await this.request('cloud'), id, this.crypto);
          if (epoch === this.epoch) { this.conflict = { local: clone(this.draft), remote }; await this.retainConflict(); }
        }
        throw error;
      } finally {
        if (epoch === this.epoch) {
          this.saving = false; this.onChange();
          const full = this.fullSaveAgain, settings = this.settingsAgain;
          this.fullSaveAgain = this.settingsAgain = false;
          if ((full || settings) && this.dirty && !this.conflict) this.saveCloud({ settingsOnly: !full }).catch(error => { this.lastError = error.message; this.onChange(); });
        }
      }
    }
    async retainConflict() {
      if (!this.session || !this.conflict) return true;
      const record = { schema: 1, account_id: this.session.user.id, retained_at: new Date().toISOString(), ...clone(this.conflict) };
      this.retained = [record];
      const persisted = await this.vault.save(this.key('conflict'), record);
      this.conflict.persisted = persisted; return persisted;
    }
    async useCloud() {
      if (!this.conflict) return;
      if (!await this.retainConflict()) throw new AccountError('The local conflict copy is only in memory. Download both versions before using the cloud copy.');
      this.cloud = clone(this.conflict.remote); this.draft = clone(this.cloud); this.conflict = null; this.dirty = false;
      if (plain(this.cloud.settings.browser)) { this.preferences = sanitizePreferences(this.cloud.settings.browser); this.persistPreferences(); }
      await this.preserveDraft(); this.onChange();
    }
    backup() {
      if (!this.session || !this.draft) throw new AccountError('No verified account backup is ready.');
      return { format: 'MC3-Account-Backup', schema: 1, exported_at: new Date().toISOString(), account_id: this.session.user.id,
        snapshot: clone(this.draft), retained_conflicts: clone(this.retained) };
    }
  }

  // DOM adapter is attached below. Core classes remain usable in Node tests without a document/network service.
  function mount(options) {
    const window = options.window || globalThis.window, document = window.document;
    const $ = id => document.getElementById(id);
    const notice = (message, type = 'info') => options.notify && options.notify(String(message), type);
    let authMode = 'login', submitting = false, lastScreen = '', returnScreen = 'mainMenu', padState = new Map(), padRepeat = 0;
    let client;
    const onLoadSave = options.onLoadSave; // (slot, save_data, save_name) -> Promise
    const node = (tag, className, text) => { const el = document.createElement(tag); if (className) el.className = className; if (text !== undefined) el.textContent = String(text); return el; };
    const button = (text, handler, className = 'btn secondary') => { const el = node('button', className, text); el.type = 'button'; el.addEventListener('click', handler); return el; };
    function visibleScreen() { return Array.from(document.querySelectorAll('.screen')).reverse().find(el => !el.classList.contains('hidden') && window.getComputedStyle(el).display !== 'none') || null; }
    function focusables(screen) { return Array.from(screen.querySelectorAll('button:not(:disabled),a[href],input:not(:disabled),select:not(:disabled),textarea:not(:disabled),[tabindex="0"]')).filter(el => !el.hidden && window.getComputedStyle(el).display !== 'none' && el.getClientRects().length); }
    function activate(id) {
      if (options.releaseInput) options.releaseInput();
      options.showScreen(id); padState.clear();
      window.requestAnimationFrame(() => { const screen = $(id); const first = screen && focusables(screen)[0]; if (first) first.focus({ preventScroll: true }); });
    }
    function back() {
      const screen = visibleScreen(); if (!screen) return;
      if (screen.id === 'authScreen') return;
      if (screen.id === 'mainMenu' && options.canResume && options.canResume()) {
        document.querySelectorAll('.screen').forEach(el => el.classList.add('hidden')); if (options.releaseInput) options.releaseInput(); return;
      }
      activate(screen.id === 'settings' ? returnScreen : 'mainMenu');
    }
    function download(name, value, mime = 'application/json') {
      const blob = value instanceof Blob ? value : new Blob([typeof value === 'string' ? value : JSON.stringify(value, null, 2)], { type: mime });
      const url = window.URL.createObjectURL(blob), link = node('a'); link.href = url; link.download = name; document.body.appendChild(link); link.click(); link.remove();
      window.setTimeout(() => window.URL.revokeObjectURL(url), 10000);
    }
    function showError(error) { notice(error && error.message || 'The request could not finish.', 'error'); }
    function footer(parent) {
      const bar = node('div', 'mc3-footer'); bar.setAttribute('aria-hidden', 'true');
      for (const [letter, text, cls] of [['A', 'Accept', 'accept'], ['B', 'Back', 'back']]) {
        const item = node('span'); item.append(node('span', 'mc3-key ' + cls, letter), document.createTextNode(text)); bar.appendChild(item);
      }
      bar.append(node('span', '', 'TAB / D-PAD SELECT')); parent.appendChild(bar);
    }
    function passwordToggle(input, toggle, label) {
      toggle.textContent = 'Show'; toggle.setAttribute('aria-label', 'Show ' + label); toggle.setAttribute('aria-pressed', 'false'); toggle.removeAttribute('onclick');
      toggle.onclick = () => { const shown = input.type === 'password'; input.type = shown ? 'text' : 'password'; toggle.textContent = shown ? 'Hide' : 'Show';
        toggle.setAttribute('aria-label', (shown ? 'Hide ' : 'Show ') + label); toggle.setAttribute('aria-pressed', String(shown)); };
    }
    function buildAuth() {
      $('authUsername').autocomplete = 'username'; $('authUsername').maxLength = 20;
      $('authPassword').maxLength = 256;
      const userLabel = $('authUsername').closest('.input-group').querySelector('label'); userLabel.htmlFor = 'authUsername';
      const passLabel = $('authPassword').closest('.input-group').querySelector('label'); passLabel.htmlFor = 'authPassword';
      passwordToggle($('authPassword'), $('toggleAuthPassword'), 'password');
      const group = node('div', 'input-group'); group.id = 'confirmPasswordGroup'; group.hidden = true;
      const label = node('label', '', 'Confirm password'); label.htmlFor = 'authPasswordConfirm';
      const wrap = node('div', 'input-wrapper'), input = node('input'); input.id = 'authPasswordConfirm'; input.type = 'password'; input.maxLength = 256; input.autocomplete = 'new-password';
      const toggle = button('Show', () => {}, 'toggle-password'); toggle.id = 'toggleConfirmPassword';
      wrap.append(input, toggle); group.append(label, wrap); $('authPassword').closest('.input-group').after(group); passwordToggle(input, toggle, 'password confirmation');
      const remember = node('label', 'remember-row'); const checkbox = node('input'); checkbox.type = 'checkbox'; checkbox.id = 'rememberDevice'; checkbox.checked = true;
      remember.append(checkbox, document.createTextNode('Remember this device for 30 days')); group.after(remember);
      const status = node('p', 'account-error'); status.id = 'authStatus'; status.setAttribute('role', 'alert'); remember.after(status);
      $('authBtn').onclick = () => submitAuth(); $('authToggleBtn').onclick = () => mode(authMode === 'login' ? 'register' : 'login');
      $('authToggleText').replaceChildren(document.createTextNode('Your saves and settings stay with your account.'));
      mode('login'); footer($('authScreen').querySelector('.card'));
    }
    function mode(value) {
      authMode = value; const signup = value === 'register';
      $('authTitle').textContent = signup ? 'Create Account' : 'Player Sign In';
      $('authSubtitle').textContent = signup ? 'Create a player account for your own save files and settings.' : 'Continue with your player account. Your password stays private.';
      $('authBtn').textContent = signup ? 'Create Account' : 'Log In'; $('authToggleBtn').textContent = signup ? 'Back to Log In' : 'Create Account';
      $('confirmPasswordGroup').hidden = !signup; $('authPassword').autocomplete = signup ? 'new-password' : 'current-password';
      $('authPassword').minLength = signup ? 8 : 1; $('authPasswordConfirm').minLength = 8;
      $('authPassword').value = ''; $('authPasswordConfirm').value = ''; $('authStatus').textContent = '';
    }
    async function submitAuth() {
      if (submitting) return; submitting = true; $('authBtn').disabled = true; $('authToggleBtn').disabled = true; $('authStatus').textContent = '';
      try {
        await client.authenticate(authMode, $('authUsername').value, $('authPassword').value, $('rememberDevice').checked, $('authPasswordConfirm').value);
        $('authPassword').value = $('authPasswordConfirm').value = ''; activate('mainMenu'); await loadCloud();
      } catch (error) { $('authStatus').textContent = error.message; }
      finally { submitting = false; $('authBtn').disabled = false; $('authToggleBtn').disabled = false; }
    }
    function buildCloud() {
      $('btnNewSave').textContent = 'Import Save / Config';
      const header = $('savesScreen').querySelector('.saves-header'); header.append(button('Back', back));
      const content = $('savesScreen').querySelector('.saves-content');
      const intro = node('p', 'account-muted', 'Back up original HDD save bytes and settings/config files. This does not create or edit a career. Downloading a backup does not load it into a running game.');
      const toolbar = node('div', 'cloud-toolbar');
      const input = node('input'); input.type = 'file'; input.multiple = true; input.hidden = true; input.id = 'cloudImportFiles';
      const folder = node('input'); folder.type = 'file'; folder.multiple = true; folder.hidden = true; folder.setAttribute('webkitdirectory', ''); folder.id = 'cloudImportFolder';
      $('btnNewSave').onclick = () => input.click();
      const upload = button('Save changes to cloud', () => client.saveCloud().then(() => notice('Cloud backup saved. Original game files were not changed.')).catch(showError), 'btn primary'); upload.id = 'cloudUpload';
      toolbar.append(button('Import Profile Folder', () => folder.click()), upload,
        button('Download Account Backup', () => { try { download('MC3-account-backup.json', client.backup()); notice('Backup downloaded. It has not been loaded into the game.'); } catch (error) { showError(error); } }),
        button('Refresh cloud', () => loadCloud().catch(showError)));
      const status = node('p', 'cloud-status'); status.id = 'cloudStatus'; status.setAttribute('role', 'status'); status.setAttribute('aria-live', 'polite');
      const conflict = node('section', 'cloud-conflict'); conflict.id = 'cloudConflict'; conflict.hidden = true; conflict.setAttribute('aria-label', 'Cloud version conflict');
      conflict.append(node('h3', '', 'Two versions were kept'), node('p', '', 'Another device saved first. Your draft and its cloud version are both retained. Choose which becomes current.'));
      conflict.append(button('Download both versions', () => download('MC3-conflicting-backups.json', client.backup())),
        button('Keep my version in cloud', () => client.saveCloud({ replaceConflict: true }).then(() => notice('Your version is current; the former cloud version remains in server history.')).catch(showError)),
        button('Use cloud version', () => client.useCloud().then(() => notice('Cloud version selected; your prior draft is retained.')).catch(showError)));
      content.prepend(intro, toolbar, status, conflict, input, folder); footer(content);
      const imported = async event => {
        try { const count = await client.importFiles(Array.from(event.target.files || [])); notice(`${count} original file(s) added to this account draft. Choose Save changes to cloud.`); }
        catch (error) { showError(error); } finally { event.target.value = ''; }
      };
      input.addEventListener('change', imported); folder.addEventListener('change', imported);
      $('careerScreen').querySelector('.career-header').append(button('Back', back));
    }
    function renderCloud() {
      if (!$('cloudStatus') || !client) return;
      const draft = client.draft; $('saveList').replaceChildren();
      $('cloudUpload').disabled = !draft || !client.dirty || client.saving || !!client.conflict;
      $('cloudConflict').hidden = !client.conflict;
      $('cloudStatus').textContent = client.saving ? 'Saving this account snapshot...' : client.lastError ? client.lastError : !draft ? 'Cloud snapshot is not loaded.' :
        `Revision ${client.cloud ? client.cloud.revision : draft.revision} · ${draft.files.length} file(s) · ${client.dirty ? 'Local changes awaiting cloud save' : 'Cloud copy verified'}`;
      if (!draft || !draft.files.length) { $('saveList').appendChild(node('p', 'account-muted', 'No original save/config files have been uploaded for this account.')); return; }
      for (const file of draft.files) {
        const row = node('div', 'save-item'), info = node('div', 'save-info');
        info.append(node('div', 'save-name', file.path), node('div', 'save-meta', `${file.size.toLocaleString()} bytes · SHA-256 ${file.sha256.slice(0, 12)}…`));
        const actions = node('div', 'save-actions');
        actions.appendChild(button('Download file', () => {
          try { download(file.path.split('/').pop(), new Blob([base64ToBytes(file.data)], { type: 'application/octet-stream' })); notice('Original file downloaded; game memory/save files were not changed.'); } catch (error) { showError(error); }
        }, 'save-btn load'));
        // Offer to load original hdd/ game saves into a connected native game session.
        if (file.path.startsWith('hdd/') && onLoadSave) {
          const m = file.path.match(/(\d+)/);
          const slot = m ? Number(m[1]) : 0;
          actions.appendChild(button('Load into game', async () => {
            try {
              const ack = await onLoadSave(slot, file.data, file.path);
              notice(ack && ack.ok ? `Game acknowledged save: ${file.path}` : 'Game did not acknowledge the save.', ack && ack.ok ? 'info' : 'error');
            } catch (error) { showError(error); }
          }, 'save-btn load'));
        }
        row.append(info, actions); $('saveList').appendChild(row);
      }
    }
    function renderCareer() {
      $('careerStats').replaceChildren(); $('careerContent').replaceChildren();
      const files = client && client.draft ? client.draft.files.filter(file => file.path.startsWith('hdd/')).length : 0;
      const stat = node('div', 'career-stat'); stat.append(node('div', 'stat-value', files), node('div', 'stat-label', 'Original save files backed up'));
      $('careerStats').appendChild(stat);
      $('careerContent').append(node('h3', '', 'Your career belongs to the game'),
        node('p', '', 'Money, cars, unlocked cities and race progress remain inside your original save files. This browser does not invent or rewrite those fields.'),
        node('p', '', 'Import a profile folder under Cloud Saves to preserve its exact bytes. Download a backup to move it to another installation.'),
        button('Open Cloud Saves', () => { activate('savesScreen'); loadCloud().catch(showError); }, 'btn primary'));
    }
    function buildSettings() {
      const screen = node('div', 'screen hidden'); screen.id = 'settings'; screen.style.display = 'flex';
      const card = node('div', 'settings-card'); card.append(node('h2', '', 'Account Settings'));
      card.append(node('p', 'account-muted', 'Browser controls and interface preferences sync with your account. Import settings.toml and config files under Cloud Saves to preserve all native game settings.'));
      for (const [key, label] of [['music', 'Music preference'], ['sound', 'Sound preference'], ['showChat', 'Show chat']]) {
        const row = node('label', 'remember-row'), input = node('input'); input.type = 'checkbox'; input.id = 'account-pref-' + key;
        input.addEventListener('change', () => client.updatePreferences({ [key]: input.checked })); row.append(input, document.createTextNode(label)); card.appendChild(row);
      }
      for (const [key, label, min, max, step] of [['deadzone', 'Gamepad deadzone', .05, .5, .01], ['uiScale', 'Interface scale', .85, 1.3, .05]]) {
        const row = node('div', 'settings-row'), labelEl = node('label', '', label), input = node('input'), value = node('output', 'val');
        input.type = 'range'; input.id = 'account-pref-' + key; input.min = min; input.max = max; input.step = step; labelEl.htmlFor = input.id; value.id = input.id + '-value';
        input.addEventListener('input', () => client.updatePreferences({ [key]: Number(input.value) })); row.append(labelEl, input, value); card.appendChild(row);
      }
      const status = node('p', 'account-muted'); status.id = 'settingsSyncStatus'; status.setAttribute('role', 'status'); card.appendChild(status);
      if (window.MC3Capabilities && typeof window.MC3Capabilities.mountPanel === 'function') window.MC3Capabilities.mountPanel(card, window);
      const actions = node('div', 'settings-btns'); actions.append(button('Back', back, 'btn secondary'));
      card.appendChild(actions); footer(card); screen.appendChild(card); document.body.appendChild(screen);
      const menu = $('mainMenu').querySelector('.menu-actions'); menu.insertBefore(button('Account Settings', () => toggleSettings(), 'menu-btn secondary'), $('btnLogout'));
      menu.insertBefore(button('Open game view', () => {
        if (options.canResume && options.canResume()) { document.querySelectorAll('.screen').forEach(el => el.classList.add('hidden')); if (options.releaseInput) options.releaseInput(); }
        else notice('Join or create a running native-host session first. Local browser gameplay is not connected.');
      }, 'menu-btn secondary'), $('btnLogout'));
    }
    function applyPreferences() {
      if (!client) return; const prefs = client.preferences;
      for (const key of ['music', 'sound', 'showChat']) if ($('account-pref-' + key)) $('account-pref-' + key).checked = prefs[key];
      for (const key of ['deadzone', 'uiScale']) if ($('account-pref-' + key)) {
        $('account-pref-' + key).value = prefs[key]; $('account-pref-' + key + '-value').textContent = key === 'deadzone' ? `${Math.round(prefs[key] * 100)}%` : `${Math.round(prefs[key] * 100)}%`;
      }
      document.documentElement.style.fontSize = `${16 * prefs.uiScale}px`;
      if ($('chat')) $('chat').style.visibility = prefs.showChat ? '' : 'hidden';
      if ($('settingsSyncStatus')) $('settingsSyncStatus').textContent = !client.cloud ? 'Sign in and load cloud settings to sync. Browser preference is stored only for this account.' :
        client.conflict ? 'Both versions retained. Resolve the conflict under Cloud Saves.' : client.lastError || (client.saving ? 'Syncing account settings...' : client.dirty ? 'Changes retained on this device; cloud sync pending.' : 'Account settings are synchronized.');
      // A future local runtime adapter may consume these exact preferences; no native audio state is claimed.
      window.dispatchEvent(new CustomEvent('mc3:account-preferences', { detail: { accountId: client.session && client.session.user.id, preferences: clone(prefs) } }));
    }
    function toggleSettings() {
      const active = visibleScreen();
      if (active && active.id === 'settings') return back();
      returnScreen = active ? active.id : 'mainMenu'; activate('settings');
    }
    async function loadCloud() {
      try { const result = await client.loadCloud(); renderCloud(); renderCareer(); applyPreferences(); return result; }
      catch (error) { client.lastError = error.message; renderCloud(); applyPreferences(); throw error; }
    }
    function moveFocus(direction) {
      const screen = visibleScreen(); if (!screen) return;
      const elements = focusables(screen); if (!elements.length) return;
      const index = elements.indexOf(document.activeElement), next = elements[(index + direction + elements.length) % elements.length];
      next.focus(); next.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    }
    function pollMenus() {
      const screen = visibleScreen(); if (!screen) { lastScreen = ''; padState.clear(); return false; }
      const pads = window.navigator.getGamepads ? window.navigator.getGamepads() : [];
      if (lastScreen !== screen.id) {
        lastScreen = screen.id; padState.clear();
        for (const pad of pads) if (pad) padState.set(pad.index, pad.buttons.map(button => button.pressed));
        return true; // Held gameplay buttons cannot activate a newly opened menu.
      }
      const now = Date.now();
      for (const pad of pads) {
        if (!pad) continue; const previous = padState.get(pad.index) || [], pressed = pad.buttons.map(button => button.pressed);
        if (pressed[0] && !previous[0]) { const current = document.activeElement; if (current && screen.contains(current) && !['INPUT', 'TEXTAREA', 'SELECT'].includes(current.tagName)) current.click(); else moveFocus(1); }
        if (pressed[1] && !previous[1]) back();
        const next = pressed[13] || pressed[15] || (pad.axes[1] || 0) > .65 || (pad.axes[0] || 0) > .65;
        const prior = pressed[12] || pressed[14] || (pad.axes[1] || 0) < -.65 || (pad.axes[0] || 0) < -.65;
        if ((next || prior) && now >= padRepeat) { moveFocus(next ? 1 : -1); padRepeat = now + 200; }
        if (!next && !prior) padRepeat = 0;
        padState.set(pad.index, pressed);
      }
      return true;
    }
    document.addEventListener('keydown', event => {
      if (event.code === 'F1') { event.preventDefault(); event.stopImmediatePropagation(); toggleSettings(); return; }
      const screen = visibleScreen(); if (!screen) return;
      const entry = event.target.closest && event.target.closest('input,textarea,select,[contenteditable=true]');
      if (event.code === 'Escape') { event.preventDefault(); event.stopImmediatePropagation(); back(); }
      else if (event.code === 'Enter' && screen.id === 'authScreen' && entry) { event.preventDefault(); event.stopImmediatePropagation(); submitAuth(); }
      else if (!entry && ['ArrowDown', 'ArrowRight', 'ArrowUp', 'ArrowLeft'].includes(event.code)) { event.preventDefault(); event.stopImmediatePropagation(); moveFocus(['ArrowDown','ArrowRight'].includes(event.code) ? 1 : -1); }
      else if (!entry && event.code === 'Space') event.stopImmediatePropagation();
      // All screen keyboard input belongs to the browser menu; bubble gameplay listeners never see it.
      else event.stopPropagation();
    }, true);
    buildAuth(); buildCloud(); buildSettings(); footer($('mainMenu').querySelector('.menu-card'));
    let storage = options.storage; if (!storage) try { storage = window.localStorage; } catch (_) {}
    client = new Client({ apiBase: options.apiBase, fetch: options.fetch || window.fetch.bind(window), storage,
      crypto: window.crypto, vault: new DraftVault(window.indexedDB),
      onSession: session => { if (options.onSession) options.onSession(session); if (!session) activate('authScreen'); },
      onChange: () => { renderCloud(); applyPreferences(); } });
    $('btnViewSaves').onclick = () => { activate('savesScreen'); loadCloud().catch(showError); };
    $('btnViewCareer').onclick = () => { activate('careerScreen'); renderCareer(); };
    $('btnLogout').onclick = () => client.logout().then(() => notice('Logged out on this device. Cloud files were kept.')).catch(showError);
    for (const id of ['mainMenu', 'savesScreen', 'careerScreen']) $(id).classList.add('hidden');
    activate('authScreen');
    return { client, request: (...args) => client.request(...args), loadCloud, renderCareer, toggleSettings, pollMenus,
      menuActive: () => !!visibleScreen(),
      async start() { try { if (await client.refresh()) { activate('mainMenu'); await loadCloud(); } } catch (error) { showError(error); } },
      importFiles: files => client.importFiles(files),
      exportBackup: () => client.backup()
    };
  }
  return { Client, DraftVault, AccountError, canonicalPath, importPath, encodeFile, validateCloud, bytesToBase64, base64ToBytes,
    resolveEndpoints, scopedKey, sanitizePreferences, mount, MAX_BYTES, MAX_FILES };
});
