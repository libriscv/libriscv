## RISC-V D00M Emulation

Copy shareware doom1.wad into the doom root directory and then build and run:

```sh
./build.sh
```

All binary translation modes and related experimental options are supported for this program.

```sh
cd build
cmake .. -DRISCV_BINARY_TRANSLATION=1
make -j4
```


Requires CMake, SDL2:

```sh
sudo apt install cmake libsdl2-dev
```
