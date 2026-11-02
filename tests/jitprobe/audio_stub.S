// cellAudio imported with only some of its functions, laid out exactly like PSL1GHT's stub
// objects (libaudio.a imports all 16). Built by `make AUDIOSTUB=init` (cellAudioInit alone) or
// `AUDIOSTUB=used` (the 7 functions ps3gba calls), to find what keeps the import from loading
// on PS3HEN (docs/ps3-homebrew-notes.md).

	// One imported function: its ID, its slot the loader fills, its descriptor and call stub.
	.macro stub name, fnid
	.section .rodata.sceFNID,"a"
	.align 2
	.globl \name\()_fnid
\name\()_fnid:
	.long \fnid
	.section .data.sceFStub.cellAudio,"aw"
	.align 2
	.globl \name\()_stub
\name\()_stub:
	.long __\name
	.section .opd,"aw"
	.align 3
	.globl \name
	.type \name, @function
\name:
	.quad __\name, .TOC.@tocbase, 0
	.size \name, 24
	.section .sceStub.text,"ax"
	.align 3
	.globl __\name
__\name:
	mflr 0
	std 0, 16(1)
	std 2, 40(1)
	stdu 1, -128(1)
	lis 12, \name\()_stub@ha
	lwz 12, \name\()_stub@l(12)
	lwz 0, 0(12)
	lwz 2, 4(12)
	mtctr 0
	bctrl
	addi 1, 1, 128
	ld 2, 40(1)
	ld 0, 16(1)
	mtlr 0
	blr
	nop
	.endm

	.section .rodata.sceResident,"a"
	.align 2
	.long 0
.Lname:
	.asciz "cellAudio"

	.section .rodata.sceFNID,"a"
	.align 2
	.globl cellAudio
cellAudio:

	.section .data.sceFStub.cellAudio,"aw"
	.align 2
.Lstubs:

	// size, version 1, attributes 9, then counts the PSL1GHT sprxlinker fills in
	.section .lib.stub,"aw"
	.byte 0x2c, 0
	.short 1, 9, 0, 0, 0
	.long 0
	.long .Lname, cellAudio, .Lstubs, 0, 0, 0, 0

	stub audioInit, 0x0b168f92
#ifdef AUDIOSTUB_USED
	stub audioQuit, 0xca5ac370
	stub audioPortOpen, 0xcd7bc431
	stub audioPortStart, 0x89be28f2
	stub audioPortStop, 0x5b1e2c73
	stub audioPortClose, 0x4129fe2d
	stub audioGetPortConfig, 0x74a66af0
#endif
