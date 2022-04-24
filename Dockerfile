FROM --platform=linux/amd64 ubuntu:20.04 as builder

## Install build dependencies.
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y cmake clang && \
    apt-get install -y lld

## Add source code to the build stage.
ADD . /libriscv
#ADD fuzzer.sh /
ADD  CMakeLists.txt /libriscv/
#ADD CMakeLists.txt /lib
ADD ./lib/CMakeLists.txt /lib
ADD ./lib/libriscv /lib/libriscv
WORKDIR /libriscv


## TODO: ADD YOUR BUILD INSTRUCTIONS HERE.
RUN /libriscv/fuzz/fuzzer.sh
# Package Stage
FROM --platform=linux/amd64 ubuntu:20.04

## TODO: Change <Path in Builder Stage>
COPY --from=builder /libriscv/build/vmfuzzer32 /
CMD /vmfuzzer32 -fork=1 -handle_fpe=0
