#!/usr/bin/python3
import sys
import subprocess

project  = sys.argv[1]
codefile = project + "/code.cpp"
statusfile = project + "/status.txt"
binaryfile  = sys.argv[2]

sanitized = ""
with open(codefile) as fp:
	for line in fp:
		if "#include" in line:
			if ("/" in line) or (".." in line):
				fo = open(statusfile, "w")
				fo.write("Invalid characters in statement")
				fo.close()
				sys.exit(666)
		sanitized += str(line)
#print(sanitized)

# overwrite with sanitized text
fo = open(codefile, "w")
fo.write(sanitized)
fo.close()

# compile the code
cmd = ["riscv32-unknown-elf-g++", "-march=rv32imc", "-mabi=ilp32",
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
