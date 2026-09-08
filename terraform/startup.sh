#!/bin/bash

set -euo pipefail

LOG_FILE="/var/log/zedis-startup.log"
exec > >(tee -a "$LOG_FILE") 2>&1

echo "=== Zedis startup: $(date -Is) ==="

# Install Docker if it isn't already installed.
if ! command -v docker >/dev/null 2>&1; then
    echo "Installing Docker..."
    apt-get update
    apt-get install -y docker.io
    systemctl enable docker
fi

# Make sure Docker is running.
systemctl start docker

# Configure Docker to use gcloud's Artifact Registry credential helper.
gcloud auth configure-docker asia-south1-docker.pkg.dev --quiet

# Remove an existing Zedis container if one exists.
docker rm -f zedis 2>/dev/null || true

# Pull the requested image.
docker pull "${zedis_image}"

# Start Zedis.
docker run -d \
    --name zedis \
    --restart unless-stopped \
    -p 16379:16379 \
    "${zedis_image}"

echo "=== Zedis started successfully: $(date -Is) ==="
docker ps --filter "name=zedis"
