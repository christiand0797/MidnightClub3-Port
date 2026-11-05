"""Signed-in game tunnels: race points and rankings go to the player's account instead of a guest name.

Sign in once (`proxy.py --sign-in`, or the launcher's -SignIn). The password is sent to the server once and never
stored; what is kept is the server's 30-day device credential (the same `mc3_device` value a browser keeps in an
HttpOnly cookie), in this Windows user's local app data, one entry per server. Each time the tunnel connects it
trades that credential for a 30-minute access token (POST /api/session) and signs the WebSocket in with it.
`--sign-out` revokes the device on the server and deletes the entry.
"""

import json
import os
import re
import urllib.error
import urllib.request
from pathlib import Path

ACCOUNT_FILE = Path(os.environ.get('LOCALAPPDATA') or Path.home()) / 'MidnightClub3-Port' / 'multiplayer' / 'accounts.json'
COOKIE = re.compile(r'(?:^|[;,]\s*)mc3_device=([A-Za-z0-9_-]{43})(?:;|$)')
TOKEN = re.compile(r'^[A-Za-z0-9_.-]{1,4096}$')


class AccountError(Exception):
    pass


def http_base(relay_url):
    """wss://host/ws -> https://host (the account API lives on the relay's own origin)."""
    m = re.match(r'^(wss?)://([A-Za-z0-9.-]+(?::\d{1,5})?)(?:/.*)?$', relay_url or '')
    if not m:
        raise AccountError('Relay URL must look like wss://host/ws.')
    scheme, host = m.groups()
    if scheme == 'ws' and not re.match(r'^(127\.0\.0\.1|localhost)(:\d+)?$', host):
        raise AccountError('Accounts need wss:// (HTTPS) unless the relay is on this PC.')
    return ('https://' if scheme == 'wss' else 'http://') + host


def _post(base, path, body=None, device=None, timeout=15, method='POST', bearer=None):
    data = json.dumps(body if body is not None else {}).encode()
    headers = {'Content-Type': 'application/json', 'Origin': base, 'User-Agent': 'mc3-tunnel/1'}
    if device:
        headers['Cookie'] = 'mc3_device=' + device
    if bearer:
        headers['Authorization'] = 'Bearer ' + bearer
    req = urllib.request.Request(base + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read(1 << 16) or b'{}'), r.headers.get_all('Set-Cookie') or []
    except urllib.error.HTTPError as e:
        try:
            payload = json.loads(e.read(1 << 16) or b'{}')
        except ValueError:
            payload = {}
        return e.code, payload, []


def _load():
    try:
        data = json.loads(ACCOUNT_FILE.read_text(encoding='utf-8'))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def _save(data):
    ACCOUNT_FILE.parent.mkdir(parents=True, exist_ok=True)
    tmp = ACCOUNT_FILE.with_suffix('.tmp')
    tmp.write_text(json.dumps(data, indent=1), encoding='utf-8')
    os.replace(tmp, ACCOUNT_FILE)


def saved(base):
    """The saved {username, device} for this server, or None."""
    entry = _load().get(base)
    if isinstance(entry, dict) and isinstance(entry.get('device'), str) and COOKIE.search('mc3_device=' + entry['device']):
        return entry
    return None


def sign_in(base, username, password, register=False):
    status, body, cookies = _post(base, '/api/register' if register else '/api/login',
                                  {'username': username, 'password': password, 'remember': True})
    if status not in (200, 201):
        raise AccountError(body.get('error') or f'Sign-in failed (HTTP {status}).')
    device = next((m.group(1) for c in cookies for m in [COOKIE.search(c)] if m), None)
    if not device:
        raise AccountError('The server did not issue a device sign-in.')
    name = body.get('user', {}).get('username') or username
    data = _load()
    data[base] = {'username': name, 'device': device}
    _save(data)
    return name


def sign_out(base):
    entry = saved(base)
    data = _load()
    data.pop(base, None)
    _save(data)
    if entry:
        _post(base, '/api/logout', device=entry['device'])
    return entry['username'] if entry else None


def access_token(base):
    """A fresh access token for the saved device, or None if not signed in / the sign-in expired or was revoked."""
    entry = saved(base)
    if not entry:
        return None
    status, body, _ = _post(base, '/api/session', device=entry['device'])
    if status == 401:
        return None
    if status != 200 or not TOKEN.match(str(body.get('token', ''))):
        raise AccountError(body.get('error') or f'Account server answered HTTP {status}.')
    return body['token']


def delete_account(base, password):
    """Permanently delete the signed-in account (rankings, club membership, cloud saves); needs its password."""
    token = access_token(base)
    if not token:
        raise AccountError('Sign in first.')
    status, body, _ = _post(base, '/api/account', {'password': password}, method='DELETE', bearer=token)
    if status != 200:
        raise AccountError(body.get('error') or f'Delete failed (HTTP {status}).')
    data = _load()
    data.pop(base, None)
    _save(data)
    return body.get('username')
