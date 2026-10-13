; Detour for ENGINE::Trace (0x14023d5c0), an empty variadic printf in retail:
;     mov [rsp+10h], rdx / mov [rsp+18h], r8 / mov [rsp+20h], r9 / ret
; With whole-program optimisation its callers rely on that body preserving every volatile
; register (FUN_1411ccb10 keeps loop state in r9/r10/r11 across the call; clobbering them
; crashed the game entering gameplay). This thunk reproduces the spill, saves all volatile
; general and xmm0-5 registers, and calls B2SVR_TraceV(fmt, va_list).

EXTERN B2SVR_TraceV:PROC

.code
B2SVR_TraceThunk PROC
    mov     [rsp+10h], rdx
    mov     [rsp+18h], r8
    mov     [rsp+20h], r9
    push    rax
    push    rcx
    push    rdx
    push    r8
    push    r9
    push    r10
    push    r11
    ; entry rsp = 8 mod 16; after 7 pushes rsp = 0 mod 16. 0x80 keeps that alignment:
    ; 0x20 shadow space + 6 x 16-byte xmm slots.
    sub     rsp, 80h
    movdqu  [rsp+20h], xmm0
    movdqu  [rsp+30h], xmm1
    movdqu  [rsp+40h], xmm2
    movdqu  [rsp+50h], xmm3
    movdqu  [rsp+60h], xmm4
    movdqu  [rsp+70h], xmm5
    ; rcx is still the format string; va_list points at the caller's spilled rdx.
    lea     rdx, [rsp + 80h + 38h + 10h]
    call    B2SVR_TraceV
    movdqu  xmm0, [rsp+20h]
    movdqu  xmm1, [rsp+30h]
    movdqu  xmm2, [rsp+40h]
    movdqu  xmm3, [rsp+50h]
    movdqu  xmm4, [rsp+60h]
    movdqu  xmm5, [rsp+70h]
    add     rsp, 80h
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rdx
    pop     rcx
    pop     rax
    ret
B2SVR_TraceThunk ENDP
END
