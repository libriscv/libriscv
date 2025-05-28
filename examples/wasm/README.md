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

It will run a basic LuaJIT program as written in main.cpp. Have fun!

## Example output

```sh
LuaJIT WebAssembly Example
[luajit] Hello, WebAssembly!
[luajit] The 500th fibonacci number is 1.394232245617e+104

Runtime: 340us Insn/s: 387.3mi/s  Result: 42.000000
```

For one-shots disabling the JIT will result in faster execution!
