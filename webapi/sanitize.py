#!/usr/bin/python3
import sys
import subprocess
import os

project    = sys.argv[1]
method     = sys.argv[2]
os.chdir(project)
statusfile = "status.txt"
codefile   = "code.cpp"
binaryfile = "binary"

sanitized = ""
with open(codefile) as fp:
	for line in fp:
		if "include" in line:
			if ("/" in line) or (".." in line):
				fo = open(statusfile, "w")
				fo.write("Invalid characters in statement")
				fo.close()
				sys.exit(666)
		sanitized += str(line)
print(sanitized)

# overwrite with sanitized text
fo = open(codefile, "w")
fo.write(sanitized)
fo.close()

local_dir = os.getcwd()
# docker shared folder
dc_shared = "/usr/outside"

dc_extra = []
if method == "linux":
	dc_image = "linux-rv32gc"
	dc_gnucpp = "riscv32-unknown-linux-gnu-g++"
	dc_extra = ["-pthread"]
else:
	dc_image = "newlib-rv32gc"
	dc_gnucpp = "riscv32-unknown-elf-g++"

# compile the code
cmd = ["docker", "run", "--volume", local_dir + ":" + dc_shared,
		"--user", "1000:1000", dc_image,
		dc_gnucpp, "-march=rv32gc", "-mabi=ilp32", "-static"] + dc_extra + [
		"-std=c++17", "-O2", "-fstack-protector", codefile, "-o", binaryfile,
		"-ffunction-sections", "-fdata-sections", "-Wl,-gc-sections", "-Wl,-s"]
print(cmd)

result = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
stdout, stderr = result.communicate()
returncode = result.returncode

#print(stdout)
#print(stderr)

fo = open(statusfile, "w")
fo.write(stderr.decode("utf-8"))
fo.close()

exit(returncode)
