.section .text

.global _start
_start:
	li sp, -8
	sd a7, 0(sp)

	li a7, 2
	la a0, hello_world
	scall

	li a7, 1
	li a0, 0
	scall

hello_world:
	.type hello_world, @object
	.string "Hello World!"
