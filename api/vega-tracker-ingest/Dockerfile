FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1 \
    DEBIAN_FRONTEND=noninteractive

# Install curl (healthcheck) + gnupg/wget for mongo tools repo
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl gnupg wget \
    && wget -qO /tmp/mongo-tools.deb https://fastdl.mongodb.org/tools/db/mongodb-database-tools-debian12-x86_64-100.10.0.deb \
    && apt-get install -y --no-install-recommends /tmp/mongo-tools.deb \
    && rm /tmp/mongo-tools.deb \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY tracker_ingest_api.py .

# Default firmware directory.  Populated at runtime via the volumes: section
# in docker-compose.yml (./firmware:/firmware:ro).  Content is not baked into
# the image so there is no COPY firmware/ here.
RUN mkdir -p /firmware

# Backup directory — host-mounted via docker-compose volumes so backups
# survive container restarts/upgrades.  mongodump writes here.
RUN mkdir -p /backups

EXPOSE 8030

HEALTHCHECK --interval=30s --timeout=5s --retries=3 --start-period=10s \
    CMD curl -fsS http://localhost:8030/health || exit 1

CMD ["uvicorn", "tracker_ingest_api:app", "--host", "0.0.0.0", "--port", "8030"]
