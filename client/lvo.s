| libogles3.a: calls through ogles3.library's vectors (GCC, stack ABI).
	.text
	.even
	.extern	_OGLES3Base

| const struct OGLES3Dispatch *OGLES3GetDispatchTableLVO(void)   -30
	.globl	_OGLES3GetDispatchTableLVO
_OGLES3GetDispatchTableLVO:
	move.l	a6,-(sp)
	move.l	_OGLES3Base,a6
	move.l	a6,d0
	beq.s	1f
	jsr	-30(a6)
1:	move.l	(sp)+,a6
	rts

| ULONG OGLES3PrivateLogFlushLVO(void)   -36, private
	.globl	_OGLES3PrivateLogFlushLVO
_OGLES3PrivateLogFlushLVO:
	move.l	a6,-(sp)
	move.l	_OGLES3Base,a6
	move.l	a6,d0
	beq.s	1f
	jsr	-36(a6)
1:	move.l	(sp)+,a6
	rts
