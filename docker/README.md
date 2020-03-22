RISC-V binary building containers
==========================

## Building the docker images

GCC full Linux userspace environment:
```
docker build -t linux-rv32gc . -f linux.Dockerfile
```
Supports threads.


Newlib limited userspace environment:
```
docker build -t newlib-rv32gc . -f newlib.Dockerfile
```

Note that you may have to run as root to build these.


## Starting the docker containers

Check the `docker_start.sh` script. It will start each of the containers, whichever one you have built will be started.
