#!/usr/bin/python3
import sys
import subprocess
import os

project_base = sys.argv[1]
project_dir  = sys.argv[2]
method       = sys.argv[3]
# relative local filesystem paths
os.chdir(project_base + "/" + project_dir)
python_codefile = "code.cpp"
python_status   = "status.txt"
# docker volume paths
dc_codefile = project_dir + "/" + python_codefile
dc_binary   = project_dir + "/binary"

sanitized = ""
with open(python_codefile) as fp:
	for line in fp:
		if "include" in line:
			if ("/" in line) or (".." in line):
				fo = open(python_status, "w")
				fo.write("Invalid characters in statement")
				fo.close()
				sys.exit(666)
		sanitized += str(line)
print(sanitized)

# overwrite with sanitized text
fo = open(python_codefile, "w")
fo.write(sanitized)
fo.close()

# docker outside & inside shared folder
local_dir = project_base
dc_shared = "/usr/outside"

dc_extra = []
if method == "linux":
	dc_instance = "linux-rv32gc"
	dc_gnucpp = "riscv32-unknown-linux-gnu-g++"
	dc_extra = ["-pthread"]
else:
	dc_instance = "newlib-rv32gc"
	dc_gnucpp = "riscv32-unknown-elf-g++"

# compile the code
cmd = ["docker", "exec", dc_instance,
		dc_gnucpp, "-march=rv32gc", "-mabi=ilp32", "-static"] + dc_extra + [
		"-std=c++17", "-O2", "-fstack-protector", dc_codefile, "-o", dc_binary,
		"-ffunction-sections", "-fdata-sections", "-Wl,-gc-sections", "-Wl,-s"]
print(cmd)

result = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
stdout, stderr = result.communicate()
returncode = result.returncode

#print(stdout)
#print(stderr)

fo = open(python_status, "w")
fo.write(stderr.decode("utf-8"))
fo.close()

exit(returncode)
