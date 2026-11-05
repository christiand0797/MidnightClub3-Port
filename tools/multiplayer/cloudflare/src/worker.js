// MC3 online world, Cloudflare edition: the HTTP front. Everything stateful lives in the World Durable Object.
export { World } from './world.js';

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const world = () => env.WORLD.get(env.WORLD.idFromName('mc3-world-2'));
    if (url.pathname === '/healthz') {
      return Response.json({ service: 'mc3-relay', protocol: 1, status: 'ok', host: 'cloudflare' });
    }
    if (url.pathname === '/ws') {
      if (request.headers.get('Upgrade') !== 'websocket') return new Response('WebSocket expected', { status: 426 });
      return world().fetch(request);
    }
    if (url.pathname === '/api/games' && request.method === 'GET') return world().fetch(request);
    if (url.pathname.startsWith('/api/')) {
      return Response.json({ error: 'Accounts and cloud saves are not available on this server yet.' }, { status: 501 });
    }
    return env.ASSETS ? env.ASSETS.fetch(request) : new Response('Not found', { status: 404 });
  }
};
