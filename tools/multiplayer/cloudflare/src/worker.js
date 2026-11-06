// MC3 online world, Cloudflare edition: the HTTP front. Everything stateful lives in the World Durable Object.
export { World } from './world.js';
export { Room } from './room.js';

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const world = () => env.WORLD.get(env.WORLD.idFromName('mc3-world-2'));
    if (url.pathname === '/healthz') {
      return Response.json({ service: 'mc3-relay', protocol: 1, status: 'ok', host: 'cloudflare' });
    }
    // a room's game traffic, on the room's own object (room.js); the World issued the ticket
    if (url.pathname.startsWith('/ws/room/')) {
      if (request.headers.get('Upgrade') !== 'websocket') return new Response('WebSocket expected', { status: 426 });
      const id = decodeURIComponent(url.pathname.slice('/ws/room/'.length));
      if (!id || id.length > 64) return new Response('Bad room', { status: 400 });
      return env.ROOM.get(env.ROOM.idFromName(id)).fetch(request);
    }
    if (url.pathname === '/ws') {
      if (request.headers.get('Upgrade') !== 'websocket') return new Response('WebSocket expected', { status: 426 });
      return world().fetch(request);
    }
    // Lobby listing, accounts (register/login/session/logout/profile) and cloud saves all live in the World.
    if (url.pathname.startsWith('/api/')) return world().fetch(request);
    return env.ASSETS ? env.ASSETS.fetch(request) : new Response('Not found', { status: 404 });
  }
};
