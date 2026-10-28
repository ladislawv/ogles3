; libogles2 for vbcc: calls through ogles3.library's vectors (vasm, Motorola
; syntax). Same functions as lvo.s, which is the GCC version.
	section	code,code

	xref	_OGLES3Base

; const struct OGLES3Dispatch *OGLES3GetDispatchTableLVO(void)   -30
	xdef	_OGLES3GetDispatchTableLVO
_OGLES3GetDispatchTableLVO:
	move.l	a6,-(sp)
	move.l	_OGLES3Base,a6
	move.l	a6,d0
	beq.s	.none
	jsr	-30(a6)
.none:	move.l	(sp)+,a6
	rts

; ULONG OGLES3PrivateLogFlushLVO(void)   -36, private
	xdef	_OGLES3PrivateLogFlushLVO
_OGLES3PrivateLogFlushLVO:
	move.l	a6,-(sp)
	move.l	_OGLES3Base,a6
	move.l	a6,d0
	beq.s	.none
	jsr	-36(a6)
.none:	move.l	(sp)+,a6
	rts
