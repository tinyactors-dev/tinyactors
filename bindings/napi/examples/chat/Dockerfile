FROM node:22-bookworm
WORKDIR /app
RUN apt-get update \
    && apt-get install -y --no-install-recommends build-essential \
       python3 pkg-config libnode-dev \
    && rm -rf /var/lib/apt/lists/*
COPY bindings/napi/examples/chat/package*.json bindings/napi/examples/chat/
RUN npm ci --prefix bindings/napi/examples/chat
COPY . .
RUN npm run build --prefix bindings/napi
EXPOSE 8080
CMD ["node", "bindings/napi/examples/realtime.js"]
