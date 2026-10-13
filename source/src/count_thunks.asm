; EXP-031 task 3/4 discovery: call-counting detours that preserve every register.
; Thunk i: stores rcx (this) in B2SVR_CountThis[i], increments B2SVR_CountCalls[i], then jumps to
; the MinHook trampoline in B2SVR_CountOrig[i]. No argument register is touched (float args safe).

PUBLIC B2SVR_CountThis
PUBLIC B2SVR_CountCalls
PUBLIC B2SVR_CountOrig

.data
ALIGN 8
B2SVR_CountThis  QWORD 8 DUP(0)
B2SVR_CountCalls QWORD 8 DUP(0)
B2SVR_CountOrig  QWORD 8 DUP(0)

.code
COUNT_THUNK MACRO idx
B2SVR_CountThunk&idx& PROC
    mov     qword ptr [B2SVR_CountThis + idx*8], rcx
    lock inc qword ptr [B2SVR_CountCalls + idx*8]
    jmp     qword ptr [B2SVR_CountOrig + idx*8]
B2SVR_CountThunk&idx& ENDP
ENDM

COUNT_THUNK 0
COUNT_THUNK 1
COUNT_THUNK 2
COUNT_THUNK 3
COUNT_THUNK 4
COUNT_THUNK 5
COUNT_THUNK 6
COUNT_THUNK 7

END
