# SPDX-License-Identifier: GPL-2.0
# LiteKVM 自部署跳网中继 —— 零依赖 Node 镜像（无需 npm install）
FROM node:22-alpine

LABEL org.opencontainers.image.title="litekvm-relay" \
      org.opencontainers.image.description="LiteKVM self-hosted cross-network relay (token-authenticated, content-agnostic TCP forwarder)" \
      org.opencontainers.image.licenses="GPL-2.0" \
      org.opencontainers.image.version="0.1.0"

ENV NODE_ENV=production \
    RELAY_PORT=25902 \
    RELAY_HEALTH_PORT=8080 \
    RELAY_AUTH_MODE=static \
    RELAY_ROOM_MAX_PEERS=2 \
    RELAY_LOG_LEVEL=info

WORKDIR /app
COPY --chown=node:node package.json server.js ./
COPY --chown=node:node tools ./tools

# 以非 root 运行（alpine 镜像自带 node 用户）
USER node

EXPOSE 25902 8080

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
  CMD node -e "require('http').get({host:'127.0.0.1',port:process.env.RELAY_HEALTH_PORT||8080,path:'/health'},r=>process.exit(r.statusCode===200?0:1)).on('error',()=>process.exit(1))"

CMD ["node", "server.js"]
