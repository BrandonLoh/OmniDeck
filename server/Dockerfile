# OmniDeck 服务端镜像
# 构建上下文 = server/ 目录（群晖 Container Manager 直接指向该目录即可）
FROM node:20-bookworm-slim

# better-sqlite3 无预编译包时需要从源码构建
RUN apt-get update && apt-get install -y --no-install-recommends python3 make g++ \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY package.json ./
RUN npm install --omit=dev
COPY src ./src
COPY public ./public

ENV NODE_ENV=production PORT=8080 TZ=Asia/Shanghai
EXPOSE 8080
CMD ["node", "src/index.js"]
