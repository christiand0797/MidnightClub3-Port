'use strict';
// Executes the real World methods with a minimal Durable Object platform stub. No service, deployment or game.
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { webcrypto } = require('node:crypto');
const { validateMessage } = require('../relay/transport_security');
const { Social } = require('../relay/social');
const source = fs.readFileSync(path.join(__dirname, 'src/world.js'), 'utf8')
  .replace(/^import .*;\r?\n/gm, '').replace('export class World', 'class World') + '\nmodule.exports = { World };';
const sandbox = { module: {exports:{}}, DurableObject: class {constructor(ctx, env){this.ctx=ctx;this.env=env;}},
  validateMessage, Social, TextEncoder, crypto:webcrypto, Date, Response, URL, console, JSON, setTimeout, clearTimeout };
vm.runInNewContext(source, sandbox, {filename:'world.js'});
const { World } = sandbox.module.exports;
function socket(info) {
  return { info: structuredClone(info), sent:[], closed:[], bufferedAmount:0,
    deserializeAttachment(){return structuredClone(this.info);}, serializeAttachment(v){this.info=structuredClone(v);},
    send(raw){this.sent.push(JSON.parse(raw));}, close(code,reason){this.closed.push({code,reason});} };
}
async function fixture() {
  const sockets=[], writes=[]; let alarm=null;
  const ctx={getWebSockets:()=>sockets, blockConcurrencyWhile(fn){this.ready=fn();},
    storage:{get:async()=>undefined,put:async row=>writes.push(structuredClone(row)),
      getAlarm:async()=>alarm,setAlarm:async at=>{alarm=at;}}};
  const world=new World(ctx,{}); await ctx.ready;
  const add=(extra={})=>{const ws=socket({ip:'127.0.0.1',at:Date.now(),...extra});sockets.push(ws);return ws;};
  return {world,ctx,sockets,writes,add,get alarm(){return alarm;}};
}
test('cross-origin websocket and global/address quotas reject before admission', async()=>{
  const f=await fixture();
  let r=await f.world.fetch(new Request('https://mc3.test/ws',{headers:{Upgrade:'websocket',Origin:'https://evil.test'}}));
  assert.equal(r.status,403);
  for(let n=0;n<16;n++)f.add();
  r=await f.world.fetch(new Request('https://mc3.test/ws',{headers:{Upgrade:'websocket','CF-Connecting-IP':'127.0.0.1'}}));
  assert.equal(r.status,429);
  for(let n=16;n<500+2000;n++)f.add({ip:String(n)});  // players (MAX_PLAYERS 500) plus the queue (2000)
  r=await f.world.fetch(new Request('https://mc3.test/ws',{headers:{Upgrade:'websocket','CF-Connecting-IP':'new'}}));
  assert.equal(r.status,429);
});
test('UTF-8 size, message and byte budgets reject and survive a new World object',async()=>{
  const f=await fixture(), a=f.add({id:'a',userId:'guest-a',username:'a'});
  await f.world.webSocketMessage(a,'é'.repeat(9000));assert.equal(a.closed[0].code,1009);
  const b=f.add({id:'b',userId:'guest-b',username:'b'});
  assert.equal(f.world.allowed(b,512*1024),true);
  const again=new World(f.ctx,{});await f.ctx.ready;
  assert.equal(again.allowed(b,512*1024),false);
  b.info.budget={tokens:0,bytes:512*1024,at:Date.now()+1000};
  const third=new World(f.ctx,{});await f.ctx.ready;  // a woken object reads the attachment (a live one keeps its copy)
  assert.equal(third.allowed(b,1),false);
});
test('never-authenticated viewers expire on alarm or first late message',async()=>{
  const f=await fixture(), a=f.add({at:Date.now()-16000}),b=f.add({at:Date.now()-1000});
  await f.world.alarm();assert.equal(a.closed[0].code,1008);assert.equal(b.closed.length,0);assert.ok(f.alarm>Date.now());
  const c=f.add({at:Date.now()-16000});await f.world.webSocketMessage(c,JSON.stringify({type:'join',name:'late'}));
  assert.equal(c.closed[0].code,1008);assert.equal(c.info.id,undefined);
});
test('tunnel input and ping do not write storage, room changes do',async()=>{
  const f=await fixture(), a=f.add({id:'a',userId:'guest-a',username:'a',type:'game',gameId:'main'});
  await f.world.webSocketMessage(a,JSON.stringify({type:'ping',ts:1}));
  await f.world.webSocketMessage(a,JSON.stringify({type:'input',data:'AA=='}));
  assert.equal(f.writes.length,0);
  await f.world.webSocketMessage(a,JSON.stringify({type:'enter_room',name:'Friday'}));
  assert.equal(f.writes.length,1);assert.equal(f.writes[0].rooms.length,1);
});
test('room capacity and outgoing queues are bounded',async()=>{
  const f=await fixture(), a=f.add({id:'a',userId:'guest-a',username:'a'});
  for(let n=1;n<1024;n++)f.world.rooms.set('r'+n,{id:'r'+n,name:'room',listed:false});
  await f.world.webSocketMessage(a,JSON.stringify({type:'enter_room',name:'another'}));
  assert.match(a.sent.at(-1).error,/quota/);assert.equal(f.world.rooms.size,1024);  // MAX_ROOMS
  a.bufferedAmount=2*1024*1024;f.world.send(a,{type:'ping'});assert.equal(a.closed.at(-1).code,1009);
  const b=f.add();f.world.send(b,{text:'é'.repeat(70000)});assert.equal(b.closed.at(-1).code,1009);
});
test('close callback replies to the handshake and releases guest activity',async()=>{
  const f=await fixture(),a=f.add({id:'a',userId:'guest-a',username:'a'});
  f.world.social.player(a.info);assert.equal(f.world.social.guests.size,1);
  await f.world.webSocketClose(a,1000);assert.equal(a.closed.at(-1).code,1000);assert.equal(f.world.social.guests.size,0);
  const b=f.add();await f.world.webSocketClose(b,1006);assert.equal(b.closed.at(-1).code,1000);
});
