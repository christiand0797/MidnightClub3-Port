# MC3 Multiplayer Relay — Dockerfile
FROM node:24-alpine

WORKDIR /app/relay

# Install dependencies
COPY relay/package.json relay/package-lock.json ./
RUN npm ci --omit=dev --ignore-scripts && npm cache clean --force

# Copy application code
COPY --chown=node:node relay/server.js relay/account_service.js relay/transport_security.js relay/social.js ./
COPY --chown=node:node browser/ ../browser/

# Create data directory
RUN mkdir -p /data && chown node:node /data
ENV NODE_ENV=production DATA_DIR=/data RELAY_PORT=8080 RELAY_HOST=0.0.0.0
USER node

# Expose ports
EXPOSE 8080

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
  CMD node -e "const r=require('http').get('http://127.0.0.1:8080/healthz',s=>{s.resume();process.exit(s.statusCode===200?0:1)});r.setTimeout(3000,()=>r.destroy());r.on('error',()=>process.exit(1));"

CMD ["node", "server.js"]
