#!/usr/bin/env python3
"""Translate host_functions.json into a Rust module.

This is the Rust counterpart of examples/expert/generate.py. Both read the
exact same kind of JSON - a list of "Name": "C signature" pairs - and both
produce the same two things for the guest:

  1. A callable symbol per host function, whose body is the two-instruction
     stub that traps into the host (a custom instruction carrying the table
     index, then ret).
  2. The `dyncall_table` in .rodata, which the host reads at load time and
     matches against its own registered functions by CRC32 of the signature.

The C generator writes a .h and a .c. This one writes a single .rs, because
Rust can do both jobs in one file: `global_asm!` emits the stubs and the
table, and an `extern "C"` block declares them to the rest of the crate.

On top of that it does something the C generator cannot: because it knows the
whole signature, it also emits a *safe* Rust wrapper per function, grouped
into a module per namespace. `"IO::print": "void sys_print (const char*,
size_t)"` becomes `io::print(text: &str)`, and the raw declaration stays
available as `raw::sys_print` for anyone who wants it.

Usage:
    generate.py -j host_functions.json -o src/host_functions.rs
"""

import json
import re
from argparse import ArgumentParser
from array import array

parser = ArgumentParser()
parser.add_argument("-j", "--json", dest="jsonfile", default="host_functions.json",
                    help="read JSON from FILE", metavar="FILE")
parser.add_argument("-o", "--output", dest="output", required=True,
                    help="write the generated Rust module to FILE", metavar="FILE")
parser.add_argument("-v", "--verbose", action="store_true", dest="verbose",
                    default=False, help="print status messages to stdout")
args = parser.parse_args()

# ---------------------------------------------------------------------------
# CRC32, identical to the C generator and to riscv::crc32 on the host. The
# hash of the single-spaced signature string is what ties the three sides
# together, so this must not drift.
# ---------------------------------------------------------------------------

poly = 0xEDB88320
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
	return (-1 - value) & 0xffffffff

# ---------------------------------------------------------------------------
# C signature parsing
# ---------------------------------------------------------------------------

IDENTIFIER = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*$')

# Types that are spelled as several words, so that the last word of e.g.
# "unsigned long" is not mistaken for a parameter name
TYPE_WORDS = {
	"void", "char", "short", "int", "long", "float", "double",
	"signed", "unsigned", "const", "struct", "size_t", "ssize_t",
	"int8_t", "uint8_t", "int16_t", "uint16_t",
	"int32_t", "uint32_t", "int64_t", "uint64_t",
}

def tokenize(decl):
	"""Split a declaration into words, with every '*' its own token."""
	return decl.replace('*', ' * ').split()

def detach_name(tokens):
	"""Return (type_tokens, name), where name may be None.

	The parameter name, when there is one, is the last token: a plain
	identifier that is not itself a type word.
	"""
	if len(tokens) > 1 and IDENTIFIER.match(tokens[-1]) and tokens[-1] not in TYPE_WORDS:
		return tokens[:-1], tokens[-1]
	return tokens, None

def join_type(tokens):
	"""Put the type tokens back together as "const char*" rather than
	"const char *", which is the spelling the rest of this script expects."""
	out = ""
	for token in tokens:
		if token == '*':
			out += '*'
		else:
			out += (' ' if out and not out.endswith('*') else '') + token
	return out.strip()

def parse_signature(signature):
	"""Parse "int sys_math_add (int a, int b)" into its three parts.

	Returns (return_type, function_name, [(type, name), ...]).
	"""
	front, _, rest = signature.partition('(')
	arglist = rest.rsplit(')', 1)[0].strip()

	front_tokens = tokenize(front)
	if len(front_tokens) < 2 or not IDENTIFIER.match(front_tokens[-1]):
		raise ValueError("Cannot find the function name in: " + signature)
	func_name = front_tokens[-1]
	return_type = join_type(front_tokens[:-1])

	params = []
	if arglist and arglist != "void":
		for param in arglist.split(','):
			tokens = tokenize(param)
			if not tokens:
				raise ValueError("Empty parameter in: " + signature)
			type_tokens, name = detach_name(tokens)
			params.append((join_type(type_tokens), name))

	return return_type, func_name, params

# ---------------------------------------------------------------------------
# C type -> Rust type
# ---------------------------------------------------------------------------

PRIMITIVES = {
	"void": "()",
	"char": "c_char",
	"signed char": "i8",
	"unsigned char": "u8",
	"short": "i16",
	"unsigned short": "u16",
	"int": "i32",
	"unsigned": "u32",
	"unsigned int": "u32",
	"long": "i64",
	"unsigned long": "u64",
	"long long": "i64",
	"unsigned long long": "u64",
	"float": "f32",
	"double": "f64",
	"size_t": "usize",
	"ssize_t": "isize",
	"int8_t": "i8",
	"uint8_t": "u8",
	"int16_t": "i16",
	"uint16_t": "u16",
	"int32_t": "i32",
	"uint32_t": "u32",
	"int64_t": "i64",
	"uint64_t": "u64",
	"bool": "bool",
}

class Translator:
	"""Turns C types into Rust types, using the "rust": {"types": {...}}
	section of the JSON for everything that is not a C primitive."""

	def __init__(self, custom):
		self.custom = custom

	def is_custom(self, ctype):
		return ctype.strip().removeprefix("struct ").strip() in self.custom

	def translate(self, ctype):
		ctype = ctype.strip()
		if ctype.endswith('*'):
			inner = ctype[:-1].strip()
			const = inner.startswith("const ") or inner == "const"
			if const:
				inner = inner[len("const"):].strip()
			if inner in ("void", ""):
				pointee = "c_void"
			else:
				pointee = self.translate(inner)
			return ("*const " if const else "*mut ") + pointee

		if ctype.startswith("const "):
			ctype = ctype[len("const "):].strip()
		bare = ctype.removeprefix("struct ").strip()

		if bare in self.custom:
			return self.custom[bare]
		if ctype in PRIMITIVES:
			return PRIMITIVES[ctype]
		raise ValueError(
			"No Rust type for C type '%s'. Add it to the \"rust\": {\"types\"} "
			"section of the JSON." % ctype)

# ---------------------------------------------------------------------------
# Rust naming
# ---------------------------------------------------------------------------

RUST_KEYWORDS = {
	"as", "break", "const", "continue", "crate", "dyn", "else", "enum",
	"extern", "false", "fn", "for", "if", "impl", "in", "let", "loop",
	"match", "mod", "move", "mut", "pub", "ref", "return", "self", "Self",
	"static", "struct", "super", "trait", "true", "type", "unsafe", "use",
	"where", "while", "async", "await", "abstract", "become", "box", "do",
	"final", "macro", "override", "priv", "try", "typeof", "unsized",
	"virtual", "yield",
}

def snake_case(name):
	name = re.sub(r'(.)([A-Z][a-z]+)', r'\1_\2', name)
	name = re.sub(r'([a-z0-9])([A-Z])', r'\1_\2', name)
	return name.lower()

def rust_ident(name):
	"""A Rust identifier, escaped when it collides with a keyword."""
	name = snake_case(name)
	return ("r#" + name) if name in RUST_KEYWORDS else name

def split_key(key):
	"""'Math::add' -> ('math', 'add'); 'get_time' -> (None, 'get_time')."""
	if "::" in key:
		namespace, _, func = key.rpartition("::")
		return rust_ident(namespace.replace("::", "_")), rust_ident(func)
	return None, rust_ident(key)

# ---------------------------------------------------------------------------
# Safe wrappers
#
# The wrapper is generated from the parameter list alone, using four rules,
# in order. Anything a rule does not cover is passed straight through, and a
# raw pointer that survives that far makes the whole wrapper `unsafe fn`.
#
#   1. (const char*, size_t)   ->  &str, passed as pointer and length
#   2. const char*             ->  &str, copied into a CString for the call
#   3. T* / const T*  (T being a type named in the JSON's "rust" section)
#                              ->  &mut T / &T
#   4. everything else         ->  unchanged
# ---------------------------------------------------------------------------

LENGTH_TYPES = {"size_t", "unsigned long", "uint64_t", "unsigned"}

class Wrapper:
	"""The signature and body of one safe wrapper function."""

	def __init__(self, params, translator):
		self.args = []      # "name: &str"
		self.forward = []   # what to pass to the raw function
		self.pre = []       # statements before the call
		self.unsafe = False
		self._build(params, translator)

	def _build(self, params, translator):
		index = 0
		unnamed = 0
		while index < len(params):
			ctype, name = params[index]
			if name is None:
				name = "arg%d" % unnamed
				unnamed += 1
			name = rust_ident(name)
			rtype = translator.translate(ctype)

			# Rule 1: a string and its length, as a single &str
			if ctype == "const char*" and index + 1 < len(params) \
					and params[index + 1][0] in LENGTH_TYPES:
				self.args.append("%s: &str" % name)
				self.forward.append("%s.as_ptr() as *const c_char" % name)
				self.forward.append("%s.len()" % name)
				index += 2
				continue

			# Rule 2: a lone C string, zero-terminated for the call
			if ctype == "const char*":
				self.args.append("%s: &str" % name)
				self.pre.append(
					'let %s = CString::new(%s).expect("%s must not contain a NUL byte");'
					% (name, name, name))
				self.forward.append("%s.as_ptr()" % name)
				index += 1
				continue

			# Rule 3: a pointer to a type the JSON named, as a reference
			if rtype.startswith(("*mut ", "*const ")) \
					and translator.is_custom(ctype.rstrip('*').strip().removeprefix("const ").strip()):
				mutable = rtype.startswith("*mut ")
				pointee = rtype.split(' ', 1)[1]
				self.args.append("%s: &%s%s" % (name, "mut " if mutable else "", pointee))
				self.forward.append("%s as %s" % (name, rtype))
				index += 1
				continue

			# Rule 4: as-is. A raw pointer left over is the caller's problem,
			# which is what makes this wrapper unsafe.
			if rtype.startswith(("*mut ", "*const ")):
				self.unsafe = True
			self.args.append("%s: %s" % (name, rtype))
			self.forward.append(name)
			index += 1

# ---------------------------------------------------------------------------
# Read the JSON
# ---------------------------------------------------------------------------

with open(args.jsonfile) as f:
	j = json.load(f)

RESERVED_KEYS = {"typedef", "rust", "clientside", "serverside", "initialization"}

rust_section = j.get("rust", {})
translator = Translator(rust_section.get("types", {}))
prelude = rust_section.get("prelude", [])

client_side    = set(j.get("clientside", []))
server_side    = set(j.get("serverside", []))
initialization = set(j.get("initialization", []))

functions = [(key, value) for key, value in j.items() if key not in RESERVED_KEYS]

# ---------------------------------------------------------------------------
# Generate
# ---------------------------------------------------------------------------

header = """// Generated from %s by generate.py. Do not edit.
//
// Every host function below is a two-instruction stub: a custom RISC-V
// instruction carrying this function's index in `dyncall_table`, then ret.
// The emulator does not know the opcode, traps, and calls the host function
// the index selects. The host matches its own implementations against the
// table by the CRC32 of the signature, at load time, so a guest built against
// an API the host does not provide fails during initialization rather than
// somewhere in the middle of a call.
//
// This file is meant to be include!()d at the root of a crate that allows
// dead_code, unused_imports and non_camel_case_types - see lib.rs.

use core::arch::global_asm;
use core::ffi::{c_char, c_void};
use std::ffi::CString;
""" % args.jsonfile

if prelude:
	header += "\n" + "\n".join(prelude) + "\n"

# The extern declarations, the assembly stubs, and one dyncall_table entry
# per function, all built in a single pass so that the index they share stays
# in step.
externs = []
stubs = []
entries = []
modules = {}    # namespace (or None) -> [rendered function]

for index, (key, signature) in enumerate(functions):
	signature = " ".join(signature.split())
	return_ctype, symbol, params = parse_signature(signature)
	crc = crc32(signature)

	# --- the raw declaration ---
	rust_params = []
	for pos, (ctype, name) in enumerate(params):
		pname = rust_ident(name) if name else ("arg%d" % pos)
		rust_params.append("%s: %s" % (pname, translator.translate(ctype)))
	rust_return = translator.translate(return_ctype)
	returns = "" if rust_return == "()" else " -> " + rust_return

	externs.append("\t\t// %s: 0x%08x" % (key, crc))
	externs.append("\t\tpub fn %s(%s)%s;" % (symbol, ", ".join(rust_params), returns))

	# --- the stub, and the name the host reads back out of the table ---
	stubs.append('''// %s -> %s, dyncall index %d
global_asm!(r#"
.pushsection .text
.global %s
.type %s, @function
%s:
  .insn i 0b1011011, 0, x0, x0, %d
  ret
.size %s, . - %s
.popsection
.pushsection .rodata
%s_str:
.asciz "%s"
.popsection
"#);''' % (key, symbol, index, symbol, symbol, symbol, index,
		symbol, symbol, symbol, key))

	# --- the table entry the host resolves against ---
	entries.append('''  .long 0x%08x
  .long 0
  .long %s_str
  .byte %d
  .byte %d
  .byte %d
  .byte 0''' % (crc, symbol,
		int(key in initialization), int(key in client_side), int(key in server_side)))

	# --- the safe wrapper ---
	namespace, func_name = split_key(key)
	wrapper = Wrapper(params, translator)

	body = "\n".join("\t\t" + line for line in wrapper.pre)
	if body:
		body += "\n"
	body += "\t\tunsafe { raw::%s(%s) }" % (symbol, ", ".join(wrapper.forward))

	modules.setdefault(namespace, []).append(
		'''\t/// `%s`
\t///
\t/// Host function `%s`, dyncall index %d, signature hash `0x%08x`.
\t#[inline]
\tpub %sfn %s(%s)%s {
%s
\t}''' % (signature, key, index, crc,
		"unsafe " if wrapper.unsafe else "", func_name,
		", ".join(wrapper.args), returns, body))

	if args.verbose:
		print("Host function: %-20s %-28s index %d, hash 0x%08x"
			% (key, symbol, index, crc))

table = '''// The table the host reads at load time: an entry count, then one 16-byte
// { hash, reserved, name address, flags } descriptor per host function.
global_asm!(r#"
.pushsection .rodata
.align 8
.global dyncall_table
dyncall_table:
  .long %d
%s
.popsection
"#);''' % (len(functions), "\n".join(entries))

# --- assemble the file ---

out = [header]

out.append('''/// The host functions exactly as they are declared in the JSON. Prefer the
/// safe wrappers in the modules below; these are here for the cases the
/// wrappers do not cover, such as passing a callback and its captured data.
pub mod raw {
\tuse super::*;

\textern "C" {
%s
\t}
}''' % "\n".join(externs))

out.append("\n\n".join(stubs))
out.append(table)

for namespace in sorted(modules, key=lambda ns: (ns is None, ns or "")):
	rendered = "\n\n".join(modules[namespace])
	if namespace is None:
		# Functions whose JSON key had no "Namespace::" go to the top level,
		# with the module indentation taken back out
		out.append("\n\n".join(
			"\n".join(line[1:] if line.startswith('\t') else line
				for line in func.split("\n"))
			for func in modules[namespace]))
	else:
		out.append('''pub mod %s {
\tuse super::*;

%s
}''' % (namespace, rendered))

with open(args.output, "w") as f:
	f.write("\n\n".join(out) + "\n")

if args.verbose:
	print("* %d host functions written to %s" % (len(functions), args.output))
