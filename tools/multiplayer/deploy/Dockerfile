# MC3 online world: the relay (Main Lobby, rooms, car clubs, chat, rankings) as one self-contained image.
# Build context: tools/multiplayer. Players run the game on their own PCs; this server only carries lobby traffic.
FROM node:24-alpine

WORKDIR /app/relay
COPY relay/package*.json ./
RUN if [ -f package-lock.json ]; then npm ci --omit=dev --ignore-scripts; else npm install --omit=dev --ignore-scripts; fi \
 && npm cache clean --force

# every relay module (server, accounts, transport limits, social), and the account page it serves
COPY --chown=node:node relay/*.js ./
COPY --chown=node:node browser/ ../browser/
RUN rm -f ./*.test.js ../browser/*.test.js && mkdir -p /data && chown node:node /data

ENV NODE_ENV=production DATA_DIR=/data RELAY_PORT=8080 RELAY_HOST=0.0.0.0
USER node
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=10s --start-period=10s --retries=3 \
  CMD node -e "require('http').get('http://127.0.0.1:8080/healthz',s=>{s.resume();process.exit(s.statusCode===200?0:1)}).on('error',()=>process.exit(1)).setTimeout(3000,function(){this.destroy()})"
CMD ["node", "server.js"]
