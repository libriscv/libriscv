FROM --platform=linux/amd64 ubuntu:20.04 as builder

## Install build dependencies.
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y cmake clang && \
    apt-get install -y lld

## Add source code to the build stage.
COPY fuzz/ ./fuzz
COPY lib/ ./lib
WORKDIR /fuzz


## TODO: ADD YOUR BUILD INSTRUCTIONS HERE.
RUN ./fuzzer.sh

## TODO: Change <Path in Builder Stage>
CMD ./build/vmfuzzer32 -fork=1 -handle_fpe=0
