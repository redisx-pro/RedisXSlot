# https://vsupalov.com/docker-arg-env-variable-guide/
# docker build arg to all FROM
ARG A_REDIS_IMG_TAG=latest

# dockerhub img https://hub.docker.com/_/redis 
# https://github.com/docker-library/redis
FROM redis:${A_REDIS_IMG_TAG}

# docker img meta
LABEL redisxslot.image.authors="weedge"

# docker build arg after each FROM
ARG A_REDISXSLOT_ARGS="1024 0 async"
ARG A_REDIS_SERVER_PORT=6379

# container env
ENV E_REDISXSLOT_URL=https://github.com/weedge/redisxslot.git
ENV E_REDISXSLOT_ARGS=${A_REDISXSLOT_ARGS}
ENV E_REDIS_SERVER_PORT=${A_REDIS_SERVER_PORT}

# build prepare layer, use arg/env
RUN set -eux; \
    \
    apt-get update; \
    apt-get install -y --no-install-recommends \
    ca-certificates \
    wget \
    git \
    dpkg-dev \
    gcc \
    libc6-dev \
    libssl-dev \
    make \
    ; \
    rm -rf /var/lib/apt/lists/*; \
    \
    wget -O redis.tar.gz "$REDIS_DOWNLOAD_URL"; \
    echo "$REDIS_DOWNLOAD_SHA *redis.tar.gz" | sha256sum -c -; \
    mkdir -p /usr/src/redis; \
    tar -xzf redis.tar.gz -C /usr/src/redis --strip-components=1; \
    mkdir -p /usr/local/etc/redis; \
    cp /usr/src/redis/redis.conf /usr/local/etc/redis/; \
    rm redis.tar.gz; \
    \
    git clone ${E_REDISXSLOT_URL} /usr/src/redisxslot; \
    make -C /usr/src/redisxslot RM_INCLUDE_DIR=/usr/src/redis/src BUILD_TYPE=Release; \
    mv /usr/src/redisxslot/redisxslot.so /usr/local/lib/redisxslot_module.so; \
    \
    rm -rf /usr/src/redisxslot; \
    rm -r /usr/src/redis; \
    echo "Gen redisxslot module Done\n"

# Custom cache invalidation
ARG CACHEBUST=1

# build config layer, use arg/env
RUN sed -i "1i loadmodule /usr/local/lib/redisxslot_module.so ${E_REDISXSLOT_ARGS}" /usr/local/etc/redis/redis.conf; \
    chmod 644 /usr/local/etc/redis/redis.conf; \
    sed -i 's/^bind 127.0.0.1/#bind 127.0.0.1/g' /usr/local/etc/redis/redis.conf; \
    sed -i 's/^protected-mode yes/protected-mode no/g' /usr/local/etc/redis/redis.conf; \
    sed -i "s/^port 6379/port ${E_REDIS_SERVER_PORT}/g" /usr/local/etc/redis/redis.conf

# docker run container runtime, use env
CMD [ "redis-server", "/usr/local/etc/redis/redis.conf" ]