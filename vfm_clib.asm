
; VIA_AC97.866 FM Emulation TSR
; 
; (C) 2025 Eric Voirin (Oerg866)
;
; LICENSE: CC-BY-NC-SA 4.0
;
; MS C-Library replacement functions and startup code for non-debug builds
;

IFNDEF DEBUG

    .model small, c
    .586p
    .stack 300h

    .code

; void (__cdecl __far far *__cdecl _dos_getvect(unsigned int))()
_dos_getvect    proc C _vec: WORD
    mov     ax, _vec
    mov     ah, 35h
    int     21h             ; DOS - 2+ - GET INTERRUPT VECTOR
                            ; AL = interrupt number
                            ; Return: ES:BX = value of interrupt vector
    mov     dx, es
    mov     ax, bx
    ret
_dos_getvect    endp

; void __cdecl _dos_setvect(unsigned int, void (__cdecl __far far *)())
_dos_setvect    proc C _vec: WORD, _funcptr: DWORD
    mov     ax, _vec
    push    ds
    lds     dx, _funcptr
    mov     ah, 25h
    int     21h             ; DOS - SET INTERRUPT VECTOR
                            ; AL = interrupt number
                            ; DS:DX = new vector to be used for specified interrupt
    pop     ds
    xor     ax, ax
    ret
_dos_setvect    endp

; void __cdecl _dos_keep(unsigned int, unsigned int)
_dos_keep       proc C _rc:WORD, _size: WORD
    mov     ax, _rc
    mov     dx, _size
    mov     ah, 31h
    int     21h             ; DOS - DOS 2+ - TERMINATE BUT STAY RESIDENT
_dos_keep      endp         ; AL = exit code, DX = program size, in paragraphs

; Signed Long Multiplication
; dx:ax = _a * _b
_aNlmul         proc C a:DWORD, b:DWORD
    mov     eax, a          ; Load first operand into EAX
    mul     b               ; Multiply: (EDX:EAX) = EAX * B
    mov     edx, eax
    shr     edx, 16
    and     eax, 0ffffh
    ret                     ; Return (DX:AX layout)
_aNlmul         endp

; Unsigned Long Multiplication
; dx:ax = _a * _b
_aNulmul        proc C a:DWORD, b:DWORD
    mov     eax, a          ; Load first operand into EAX
    mul     b               ; Multiply: (EDX:EAX) = EAX * B
    mov     edx, eax
    shr     edx, 16
    and     eax, 0ffffh
    ret                     ; Return (DX:AX layout)
_aNulmul endp

; Signed Long Shift Left
; dx:ax <<= cl
_aNlshl         proc near
    shl     edx, 16
    mov     dx, ax
    sal     edx, cl
    mov     ax, dx
    shr     edx, 16
    ret
_aNlshl         endp

; Unsigned Long Shift Right
; dx:ax >>= cl
_aNulshr        proc near
    shl     edx, 16
    mov     dx, ax
    shr     edx, cl
    mov     ax, dx
    shr     edx, 16
    ret
_aNulshr        endp

; Unsigned Long Division
; uint32_t aNuldiv(uint32_t _a, uint32_t b)
; dx:ax = _a / _b
_aNuldiv        proc C _a:DWORD, _b:DWORD
    push    ebx
    mov     eax, _a
    mov     ebx, _b
    xor     edx, edx

    ; Perform 32-bit division: EDX:EAX / EBX
    div     ebx
    ; EAX = quotient, EDX = remainder

    ; Return the result in DX:AX
    mov     edx, eax
    shr     edx, 16     ; High word
    and     eax, 0ffffh ; Low word
    
    pop     ebx
    ret
_aNuldiv       endp

; Unsigned Long Remainder
; uint32_t aNulrem(uint32_t _a, uint32_t b)
; dx:ax = remainder of _a / _b
_aNulrem       proc C _a:DWORD, _b:DWORD
    push    ebx 
    mov     eax, _a
    mov     ebx, _b
    xor     edx, edx

    ; Perform 32-bit division: EDX:EAX / EBX
    div     ebx
    ; EAX = quotient, EDX = remainder

    ; Return the remainder in DX:AX
    mov     eax, edx
    shr     edx, 16     ; High word
    and     eax, 0ffffh ; Low word
    
    pop     ebx
    ret
_aNulrem       endp

; Yeah i don't understand this sh*t so this is just the original code disassembled...
; int __cdecl memcmp(const void *, const void *, size_t)
_fmemcmp       proc far C _ptr1: DWORD, _ptr2: DWORD, _size:WORD
    xor     ax, ax
    mov     cx, _size
    jcxz    short loc_1584F
    push    ds
    push    di
    push    si
    lds     si, _ptr1
    les     di, _ptr2
    assume  es:nothing
loc_15809:                              ; CODE XREF: _memcmp+46↓j
                                        ; _memcmp+4F↓j
    mov     ax, cx
    dec     ax
    mov     dx, di
    not     dx
    sub     ax, dx
    sbb     bx, bx
    and     ax, bx
    add     ax, dx
    mov     dx, si
    not     dx
    sub     ax, dx
    sbb     bx, bx
    and     ax, bx
    add     ax, dx
    inc     ax
    xchg    ax, cx
    sub     ax, cx
    repe cmpsb
    jnz     short loc_15847
    xchg    ax, cx
    jcxz    short loc_1584C
    or      si, si
    jnz     short loc_1583A
    mov     ax, ds
    add     ax, 1000h
    mov     ds, ax
    assume  ds:nothing

loc_1583A:                              ; CODE XREF: _memcmp+3B↑j
    or      di, di
    jnz     short loc_15809
    mov     ax, es
    add     ax, 1000h
    mov     es, ax
    assume  es:nothing
    jmp     short loc_15809
; ---------------------------------------------------------------------------

loc_15847:                              ; CODE XREF: _memcmp+34↑j
    sbb     ax, ax
    sbb     ax, 0FFFFh

loc_1584C:                              ; CODE XREF: _memcmp+37↑j
    pop     si
    pop     di
    pop     ds
loc_1584F:                              ; CODE XREF: _memcmp+8↑j
    ret
_fmemcmp        endp

kbhit           proc C
    mov     ax, 00B00h
    int     21h
    xor     ah, ah
    ret
kbhit           endp

    .data

str_CmdLine db 32 dup (0)                   ; temporary storage for the command line arguments
str_TooLong db 'Command Line too long!$'    ; Error message for command line length
    .code

; Startup Continuation in C

vfm_main    PROTO NEAR C, args:PTR BYTE

; -----------------------------------------------------------
; FM TSR Startup code wrapper
.STARTUP
; Get command line ------------------------------------------
    ; Get PSP segment into es
    mov     ah, 051h
    int     21h
    mov     es, bx

    ; Command line is at psp + 0x80
    mov     bx, 080h

    ; Get length of command line
    movzx   cx,byte ptr es:[bx]    ; size of parameter string

    inc     bx
_skipWhiteSpaceLoop:
    cmp     byte ptr es:[bx], ' ' ; Skip probable initial space 
    jne     _whiteSpaceEnd
    inc     bx
    dec     cx
    ; If we have no more args left, oops. 
    jz      _whiteSpaceEnd
    jmp     _skipWhiteSpaceLoop

_whiteSpaceEnd:

    or      cx, cx
    jz      _noArgs

    cmp     cx, 31
    jle     _lenOK

    mov     ah, 09h
    lea     dx, str_TooLong
    int     21h
    mov     ax, -1
    jmp     _exit

_lenOK:

; Copy the command line string into our null terminated buffer
    lea     si, ds:str_CmdLine

_getArgStringLoop:
    ; Copy character, can't use movsb here because es:bx is the source :D
    mov     dl, byte ptr es:[bx]
    mov     byte ptr [si], dl
    inc     si
    inc     bx
    loop    _getArgStringLoop

_noArgs:

    ; Call C main function with args as parameter
    push    offset str_CmdLine
    call    vfm_main
    add     sp, 2

_exit:
    ; Exit with return code in al
    mov     ah, 4Ch
    int     21h

ENDIF

    END                 ; End of program
