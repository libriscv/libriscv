# WebAssembly example

1. Activate emsdk:
```sh
./emsdk activate latest
source "$PWD/emsdk_env.sh"
```

2. Build the example:
```sh
./build.sh
```

3. Run using `emrun`:
```sh
emrun .build/wasm_example.html
```

It will calculate a fibonacci sequence and print it. The fib program is embedded in main.cpp.
