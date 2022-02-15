## WebAPI

Compile and run RISC-V on a website

It's not very secure because it's not being run in a docker container anymore. I am not an expert on containers, even though I have been using this service inside a container before. Regardless, it should not be too hard to execute into a container instead of directly compiling files. Have a look at `sanitize.py` for the compiler arguments.

### Usage

1. Install Varnish
2. Run varnishd -a :8080 -f $PWD/cache.vcl -F
	- If you need a custom working folder just add -n /tmp/varnishd
3. Build webapi
	- mkdir -p build && pushd build
	- cmake ..
	- make -j6
	- popd
4. Start the WebAPI server
	- ./build/webapi
5. Go to http://localhost:8080
	- Type some code and press Compile & Run
	- The program will be run in the background

### Benchmarking

While the benchmarking feature is kinda useless right now, it is possible to make it run the delineated code many times with some minor effort. At any rate, benchmarks are delinated using two EBREAK instructions.

Perhaps a better way would have been a system call that took a function pointer as argument, and then run that function X number of times to measure performance. But, I wanted something simple and here we are.
