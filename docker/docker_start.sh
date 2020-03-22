#!/usr/bin/env bash
DC_SHARED="/usr/outside"
PROG_BASE="/tmp/programs"
DC_IMAGE=$1

mkdir -p $PROG_BASE
ARGS="--volume $PROG_BASE:$DC_SHARED --user 1000:1000"

#docker stop
docker run $ARGS -t --name linux-rv32gc -d linux-rv32gc
docker run $ARGS -t --name newlib-rv32gc -d newlib-rv32gc
