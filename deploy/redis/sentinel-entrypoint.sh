#!/bin/sh
# Sentinel переписывает свой конфиг во время работы, поэтому шаблон копируется
# в рабочий каталог, а имя хоста и пароль подставляются при старте.
set -eu
CONFIG=/data/sentinel.conf
sed -e "s/__ANNOUNCE_HOST__/${ANNOUNCE_HOST}/" \
    -e "s/__REDIS_PASSWORD__/${REDIS_PASSWORD}/" \
    /etc/redis/sentinel.conf.template > "$CONFIG"
exec redis-sentinel "$CONFIG"
