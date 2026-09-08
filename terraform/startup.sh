#!/bin/bash

set -euo pipefail

LOG_FILE="/var/log/zedis-startup.log"
exec > >(tee -a "$LOG_FILE") 2>&1

echo "=== Zedis bootstrap: $(date -Is) ==="

# Install Docker.
if ! command -v docker >/dev/null 2>&1; then
    apt-get update
    apt-get install -y docker.io
fi

systemctl enable --now docker

# Configure Docker for Artifact Registry.
gcloud auth configure-docker asia-south1-docker.pkg.dev --quiet

# Install deployment script.
cat > /usr/local/bin/deploy-zedis <<'SCRIPT'
#!/bin/bash

set -euo pipefail

IMAGE="$1"

echo "Deploying ${IMAGE}"

docker pull "${IMAGE}"

docker rm -f zedis 2>/dev/null || true

docker run -d \
    --name zedis \
    --restart unless-stopped \
    -p 16379:16379 \
    "${IMAGE}"

docker ps --filter "name=zedis"
SCRIPT

chmod 755 /usr/local/bin/deploy-zedis

echo "=== Zedis bootstrap complete: $(date -Is) ==="
