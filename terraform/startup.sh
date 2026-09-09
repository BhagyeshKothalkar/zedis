#!/bin/bash

set -euo pipefail

LOG_FILE="/var/log/zedis-startup.log"
exec > >(tee -a "$LOG_FILE") 2>&1

echo "=== Zedis bootstrap: $(date -Is) ==="

# Install Docker, Docker Compose v2, and deployment validation tools.
if ! command -v docker >/dev/null 2>&1 || \
   ! docker compose version >/dev/null 2>&1 || \
   ! command -v curl >/dev/null 2>&1 || \
   ! command -v jq >/dev/null 2>&1; then
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y \
        docker.io docker-compose-v2 curl jq
fi

systemctl enable --now docker

# The repository's Compose and monitoring files are transferred here by CI.
# Keep the directory layout stable so relative Compose bind mounts resolve on
# every deployment.
install -d -m 0755 \
    /opt/zedis/monitoring/textfile \
    /opt/zedis/monitoring/grafana/provisioning \
    /opt/zedis/monitoring/grafana/dashboards

# Configure Docker for Artifact Registry.
gcloud auth configure-docker asia-south1-docker.pkg.dev --quiet

# Install deployment script.
cat > /usr/local/bin/deploy-zedis <<'SCRIPT'
#!/bin/bash

set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: deploy-zedis IMAGE" >&2
    exit 2
fi

IMAGE="$1"
DEPLOY_DIR="/opt/zedis"
COMPOSE_FILE="${DEPLOY_DIR}/docker-compose.yml"
ENV_FILE="${DEPLOY_DIR}/.env"

if [ ! -f "${COMPOSE_FILE}" ] || [ ! -d "${DEPLOY_DIR}/monitoring" ]; then
    echo "deployment files are missing from ${DEPLOY_DIR}" >&2
    exit 1
fi

printf 'ZEDIS_IMAGE=%s\n' "${IMAGE}" > "${ENV_FILE}"
chmod 600 "${ENV_FILE}"

compose() {
    docker compose \
        --project-directory "${DEPLOY_DIR}" \
        --env-file "${ENV_FILE}" \
        --file "${COMPOSE_FILE}" \
        "$@"
}

echo "Deploying ${IMAGE}"

compose config --quiet
compose pull zedis
compose up -d --no-build

required_services=(zedis node-exporter prometheus grafana)
for service in "${required_services[@]}"; do
    if ! compose ps --services --filter status=running | grep -Fxq "${service}"; then
        echo "required service is not running: ${service}" >&2
        compose ps
        exit 1
    fi
done

echo "Waiting for the monitoring path to become queryable"
for attempt in $(seq 1 30); do
    if [ -s "${DEPLOY_DIR}/monitoring/textfile/zedis.prom" ] && \
       timeout 5 bash -c '</dev/tcp/127.0.0.1/16379' 2>/dev/null && \
       curl --fail --silent http://127.0.0.1:9100/metrics |
           grep -q '^zedis_up ' && \
       curl --fail --silent --get \
           --data-urlencode 'query=up{job="node-exporter"}' \
           http://127.0.0.1:9090/api/v1/query |
           jq -e '.status == "success" and any(.data.result[]?; .value[1] == "1")' >/dev/null && \
       curl --fail --silent --get \
           --data-urlencode 'query=zedis_up' \
           http://127.0.0.1:9090/api/v1/query |
           jq -e '.status == "success" and (.data.result | length) > 0' >/dev/null && \
       curl --fail --silent http://127.0.0.1:3000/api/health |
           jq -e '.database == "ok"' >/dev/null && \
       curl --fail --silent --get \
           --data-urlencode 'query=zedis_up' \
           http://127.0.0.1:3000/api/datasources/proxy/uid/prometheus/api/v1/query |
           jq -e '.status == "success" and (.data.result | length) > 0' >/dev/null; then
        break
    fi
    if [ "${attempt}" -eq 30 ]; then
        echo "monitoring validation failed" >&2
        compose ps
        exit 1
    fi
    sleep 2
done

echo "Zedis and monitoring stack are healthy"
SCRIPT

chmod 755 /usr/local/bin/deploy-zedis

echo "=== Zedis bootstrap complete: $(date -Is) ==="
