在 Linux 上部署 SearXNG，标准做法是 Docker Compose。核心步骤和我们在 Windows 上排掉的坑都适用，我按最小可用到生产化给你一份可直接复制的流程。

**1. 创建目录和 Compose 文件**

```bash
mkdir -p ~/searxng && cd ~/searxng
```

新建 `docker-compose.yml`：

```yaml
services:
  searxng:
    image: searxng/searxng:latest
    container_name: searxng
    restart: unless-stopped
    ports:
      - "127.0.0.1:8080:8080"   # 只绑本机；需要外网访问时再改
    volumes:
      - ./searxng:/etc/searxng  # 配置持久化
    cap_drop:
      - ALL
    cap_add:
      - CHOWN
      - SETGID
      - SETUID
    read_only: true
    tmpfs:
      - /tmp
```

**2. 首次启动生成配置**

```bash
docker compose up -d
sleep 5
docker compose logs searxng
```

首次启动会自动生成 `~/searxng/searxng/settings.yml`。然后停掉再改：

```bash
docker compose down
```

**3. 启用 JSON API**

编辑 `searxng/settings.yml`，在文件里加入：

```yaml
use_default_settings: true

server:
  secret_key: "<用下面命令生成>"
  limiter: false          # 本机/内网用可以关；公开实例请保持 true
  image_proxy: true

search:
  formats:
    - html
    - json              # 没有这行，agent 请求会 403
```

生成密钥：

```bash
openssl rand -hex 32
# 或
python3 -c "import secrets; print(secrets.token_hex(32))"
```

**4. 启动并验证**

```bash
docker compose up -d
curl "http://127.0.0.1:8080/search?q=GNSS+radio+occultation&format=json"
```

返回带 `results` 的 JSON 就算成功。

**5. 国内服务器注意**

Docker Hub 直连经常超时，两种办法任选：

```bash
# 方式一：直接用加速源镜像名
image: docker.m.daocloud.io/searxng/searxng:latest

# 方式二：配置 daemon 镜像加速
sudo tee /etc/docker/daemon.json <<'EOF'
{"registry-mirrors": ["https://docker.m.daocloud.io"]}
EOF
sudo systemctl restart docker
```

如果服务器在国内，Google/Bing/DuckDuckGo 这些上游大概率超时，可以在 `settings.yml` 里加引擎覆盖，只保留可直连的引擎：

```yaml
engines:
  - name: google
    disabled: true
  - name: google cse
    disabled: true
  - name: duckduckgo
    disabled: true
  - name: startpage
    disabled: true
  - name: wikipedia
    disabled: true
  - name: bing
    disabled: true
  - name: baidu
    disabled: false
    weight: 100
  - name: yandex
    disabled: false
    weight: 60
  - name: arxiv
    disabled: false
  - name: crossref
    disabled: false
  - name: openalex
    disabled: false
```

然后 `docker compose restart searxng`。

**6. 公开部署时**

- 不要直接暴露 `8080`，前面加 Nginx/Caddy 做 HTTPS，并转发真实 IP：

```nginx
location / {
    proxy_pass http://127.0.0.1:8080;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $remote_addr;
    proxy_set_header X-Real-IP $remote_addr;
}
```

- 把 `limiter` 重新设为 `true`，并在 `settings.yml` 的 `server.trusted_proxies` 里写上反代 IP，否则限流会把所有用户当成一个 IP。
- agent 接口最好再套一层 API key（Nginx `auth_request` 或 FastAPI 都行），SearXNG 本身没有用户鉴权。

**常用维护命令**

```bash
docker compose logs -f searxng   # 看日志
docker compose restart searxng   # 改完配置重启
docker compose pull && docker compose up -d   # 升级
```

如果你是想部署到一台具体服务器上（国内还是国外、要不要 HTTPS/API key），告诉我环境，我可以把完整的 Compose + Nginx + 鉴权配置直接生成给你。