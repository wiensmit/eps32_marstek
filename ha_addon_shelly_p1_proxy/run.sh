#!/usr/bin/with-contenv bashio

export SENSOR_ENTITY="$(bashio::config 'sensor_entity')"
export LISTEN_PORT="$(bashio::config 'listen_port')"
export CACHE_TTL_MS="$(bashio::config 'cache_ttl_ms')"
export STALE_FALLBACK_MS="$(bashio::config 'stale_fallback_ms')"
export LOG_REQUESTS="$(bashio::config 'log_requests')"

bashio::log.info "Serving ${SENSOR_ENTITY} on :${LISTEN_PORT} (cache ${CACHE_TTL_MS}ms)"
exec python3 /app/server.py
