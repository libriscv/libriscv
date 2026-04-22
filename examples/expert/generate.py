import binascii
import json
from array import array

from argparse import ArgumentParser
parser = ArgumentParser()
parser.add_argument("-j", "--json", dest="jsonfile", default="dyncalls.json",
                    help="read JSON from FILE", metavar="FILE")
parser.add_argument("-o", "--output", dest="output", required=True,
                    help="write generated prototypes to FILE", metavar="FILE")
parser.add_argument("-v", "--verbose",
                    action="store_true", dest="verbose", default=False,
                    help="print status messages to stdout")
parser.add_argument('--dyncall', dest='dyncall', action='store', default=504,
                   help='set the dyncall system call number')

args = parser.parse_args()

# Use system call number 504 (instead of custom instruction)
use_syscall = False
# this is the plain CRC32 polynomial
poly = 0xEDB88320
# we need to be able to calculate CRC32 using any poly
table = array('L')
for byte in range(256):
    crc = 0
    for bit in range(8):
        if (byte ^ crc) & 1:
            crc = (crc >> 1) ^ poly
        else:
            crc >>= 1
        byte >>= 1
    table.append(crc)

def crc32(string):
    value = 0xffffffff
    for ch in string:
        value = table[(ord(ch) ^ value) & 0xff] ^ (value >> 8)

    return -1 - value

def is_type(string):
	keywords = ["unsigned", "char", "short", "int", "long", "float", "double", "size_t", "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t"]
	conventions = ("_callback", "_t")
	return ("*" in string) or string in keywords or string.endswith(conventions)

def find_arguments(string):
	sfront = string.split('(', 1)
	retval = [sfront[0].split(' ')[0]]
	strargs = sfront[1].split(')')[0]
	# [retval, arg0, arg1, '']
	fargs = retval + strargs.split(", ")
	# Remove parameter names
	for (idx, arg) in enumerate(fargs):
		symbols = arg.split(" ")
		if len(symbols) > 1:
			last = symbols[-1]
			if not is_type(last):
				symbols.pop()
				fargs[idx] = " ".join(symbols)
	# Remove empty argument lists
	if fargs[-1] == "":
		fargs.pop()
	return fargs

# load JSON
j = {}
with open(args.jsonfile) as f:
	j = json.load(f)

# List of client-side only dyncalls
client_side = []
if "clientside" in j:
	for key in j["clientside"]:
		client_side.append(key)

# List of server-side only dyncalls
server_side = []
if "serverside" in j:
	for key in j["serverside"]:
		server_side.append(key)

# List of initialization-only dyncalls
initialization = []
if "initialization" in j:
	for key in j["initialization"]:
		initialization.append(key)

header = """
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

""";

# iterate typedefs first
for key in j:
	if key == "typedef":
		for typedef in j[key]:
			# Replace \" with "
			typedef = typedef.replace('\\"', '"')
			if "#define" in typedef:
				header += typedef + "\n"
			else:
				header += typedef + ";\n"
header += "\n"

source = '__asm__(".section .text\\n");\n\n'
dyncallindex = 0

dyncall = ""

# create dyncall prototypes and assembly
for key in j:
	if key == "typedef" or key == "clientside" or key == "serverside" or key == "initialization":
		continue
	else:
		asmdef  = " ".join(j[key].split())
		asmname = asmdef.split(' ')[1]

		fargs = find_arguments(asmdef)

		crcval = crc32(asmdef) & 0xffffffff
		crc = '%08x' % crcval

		header += "// " + key + ": 0x" + crc + "\n"
		header += "extern " + asmdef + ";\n"

		if args.verbose:
			print("Dynamic call: " + key + ", hash 0x" + crc)

		# Each dynamic call has a table index where the name and hash is stored
		dyncall += '  .long 0x' + crc + '\\n\\\n'
		dyncall += '  .long ' + str(0) + '\\n\\\n'
		dyncall += '  .long ' + asmname + '_str\\n\\\n'

		# Flags (one byte each for client-side, server-side, initialization, and padding)
		is_client_side = key in client_side
		is_server_side = key in server_side
		is_initialization = key in initialization
		dyncall += '  .byte ' + str(int(is_initialization)) + '\\n\\\n'
		dyncall += '  .byte ' + str(int(is_client_side)) + '\\n\\\n'
		dyncall += '  .byte ' + str(int(is_server_side)) + '\\n\\\n'
		dyncall += '  .byte 0\\n\\\n'

		# These dynamic calls use the table indexed variant
		# Each dynamic call has a table index where the name and hash is stored
		# and at run-time this value is lazily resolved
		source += '__asm__("\\n\\\n'
		source += '.global ' + asmname + '\\n\\\n'
		source += '.func ' + asmname + '\\n\\\n'
		source += asmname + ':\\n\\\n'
		if use_syscall:
			source += '  li t0, ' + str(dyncallindex) + '\\n\\\n'
			source += '  li a7, ' + str(args.dyncall) + '\\n\\\n'
			source += '  ecall\\n\\\n'
		else:
			source += '  .insn i 0b1011011, 0, x0, x0, ' + str(dyncallindex) + '\\n\\\n'
		source += '  ret\\n\\\n'
		source += '.endfunc\\n\\\n'
		source += '.pushsection .rodata\\n\\\n'
		source += asmname + '_str:\\n\\\n'
		source += '.asciz \\\"' + key + '\\\"\\n\\\n'
		source += '.popsection\\n\\\n'
		source += '");\n\n'

		dyncallindex += 1

header += """
#ifdef __cplusplus
}
#endif
"""

dyncall_header =  '__asm__("\\n\\\n'
dyncall_header += '.pushsection .rodata\\n\\\n'
dyncall_header += '.align 8\\n\\\n'
dyncall_header += '.global dyncall_table\\n\\\n'
dyncall_header += 'dyncall_table:\\n\\\n'
dyncall_header += '  .long ' + str(dyncallindex) + '\\n\\\n'

dyncall = dyncall_header + dyncall
dyncall += '.popsection\\n\\\n'
dyncall += '");\n\n'

source += dyncall

if (args.verbose):
	print("* There are " + str(dyncallindex) + " dynamic calls")

with open(args.output + ".h", "w") as hdrfile:
	hdrfile.write(header)
with open(args.output + ".c", "w") as srcfile:
	srcfile.write(source)
