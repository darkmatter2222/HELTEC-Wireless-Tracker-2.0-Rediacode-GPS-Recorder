# --- build stage ---
FROM node:20-alpine AS build
WORKDIR /app
COPY package.json ./
RUN npm install --no-audit --no-fund
COPY index.html ./
COPY vite.config.js ./
COPY src ./src
COPY public ./public
RUN npm run build

# --- runtime stage ---
FROM nginx:1.27-alpine
RUN apk add --no-cache openssl
COPY --from=build /app/dist /usr/share/nginx/html
COPY nginx.conf /etc/nginx/conf.d/default.conf
COPY docker-entrypoint.d/10-config.sh /docker-entrypoint.d/10-config.sh
# Strip Windows CRLF line endings — Docker scp from a Windows host can deliver
# the script with \r\n which makes the kernel try to exec "/bin/sh\r" and the
# container crash-loops with "10-config.sh: not found".
RUN sed -i 's/\r$//' /docker-entrypoint.d/10-config.sh && chmod +x /docker-entrypoint.d/10-config.sh
EXPOSE 80
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
  CMD wget -qO- http://127.0.0.1/ >/dev/null 2>&1 || exit 1
