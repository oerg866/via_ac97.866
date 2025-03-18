; VIA_AC97.866 FM Emulation TSR
; 
; (C) 2025 Eric Voirin (Oerg866)
;
; LICENSE: CC-BY-NC-SA 4.0
;
; FM Emulation TSR Interrupt Handler Data & Assembly Code

    .model small, c
    .586p

    .data

IFDEF NUKED
; externals from opl3.c
EXTERN exprom: WORD
EXTERN logsinrom: WORD
ENDIF
    .code
__ldiv PROC C _out: PTR DWORD, _a:DWORD, _b:DWORD 
    push edx
    push eax
    push ebx

    mov eax, _a
    cdq
    mov ebx, _b
    idiv ebx

    push si
    mov si, [_out]
    mov [si], eax
    pop si

    pop ebx
    pop eax
    pop edx
    ret
__ldiv ENDP

__lmul PROC C _out: PTR DWORD, _a:DWORD, _b:DWORD 
    push edx
    push eax

    mov eax, _a
    mov edx, _b
    imul edx

    push si
    mov si, [_out]
    mov [si], eax
    pop si

    pop eax
    pop edx
    ret
__lmul ENDP

__ulmul PROC C _out: PTR DWORD, _a:DWORD, _b:DWORD 
    push edx
    push eax

    mov eax, _a
    mov edx, _b
    mul edx

    push si
    mov si, [_out]
    mov [si], eax
    pop si

    pop eax
    pop edx
    ret
__ulmul ENDP

__lshl PROC C _out: PTR DWORD, _a:DWORD, _b:DWORD
    push ecx
    push eax

    mov ecx, _b
    mov eax, _a
    sal eax, cl

    push si
    mov si, [_out]
    mov [si], eax
    pop si

    pop eax
    pop ecx
    ret
__lshl ENDP

__lshr PROC C _out: PTR DWORD, _a:DWORD, _b:DWORD
    push ecx
    push eax

    mov ecx, _b
    mov eax, _a
    sar eax, cl

    push si
    mov si, [_out]
    mov [si], eax
    pop si

    pop eax
    pop ecx
    ret
__lshr ENDP


__llshr PROC C _out: PTR DWORD, _a:DWORD
    push edx
    push ecx
    push eax

    push si

    mov si, [_out]
    mov edx, [si+4]
    mov eax, [si]
    mov ecx, _a
    shrd eax, edx, cl
    sar edx, cl
    test cl, 32
    je _nouppershift
    mov eax, edx
    sar edx, 31
_nouppershift:
    mov [si], eax
    mov [si+4], edx

    pop si

    pop eax
    pop ecx
    pop edx
    ret
__llshr ENDP


__i64add32 PROC C _out: PTR DWORD, _b:DWORD
    push edx
    push ecx
    push ebx
    push eax
    push si

    mov si, [_out]
    mov ecx, [si]
    mov ebx, [si+4]
    mov eax, _b

    cdq

    add eax, ecx
    adc edx, ebx

    mov [si], eax
    mov [si+4], edx
    
    pop si
    pop eax
    pop ebx
    pop ecx
    pop edx
    ret
__i64add32 ENDP

__printHexDigit PROC C _a:BYTE
    ; Prints a 4 bit value in a
    pushad
    cld
    ; Convert a hex digit (0-15) to its ASCII representation and print it
    mov al, _a
    and al, 0Fh
    cmp al, 09h
    jbe noAdj
    add al, 07h  ; Convert to ASCII A-F
noAdj:
    add al, '0' ; Convert to ASCII '0'-'9'
    mov ah, 0Eh
    mov bx, 07h
    int 10h

    popad
    ret
__printHexDigit ENDP

IFDEF DBOPL
; Nothing yet
ELSE
OPL3_ClipSampleFast PROC C sample:DWORD
    push ebx
    mov ebx, sample    
    cmp ebx, 32767
    jle _belowPos
    mov ax, 32767
    pop ebx
    ret

_belowPos:
    cmp ebx, -32768
    jge _aboveNeg
    mov ax, -32768
    pop ebx
    ret

_aboveNeg:    
    mov ax, bx
    pop ebx
    ret
OPL3_ClipSampleFast ENDP

OPL3_EnvelopeCalcExpFast PROC C level:DWORD
    push ebx
    push cx
    mov ebx, level
    cmp ebx, 00001fffh
    jbe  _noLevelAdj
    mov ebx, 00001fffh
_noLevelAdj:
    mov cx, bx  ; BX = level clipped
    and bx, 00ffh
    mov bx, exprom[bx]
    add bx, bx  ; BX = exprom[level & 0xff] << 1

    shr cx, 8   ; CX = level >> 8
    shr bx, cl  ; BX = (exprom[level & 0xff] << 1) >> (level >> 8)
    mov ax, bx  ; ret val
    pop cx
    pop ebx 
    ret
OPL3_EnvelopeCalcExpFast ENDP


; Macro for OPL3_EnvelopeCalcExp(out + envelope << 3), uses bx and cx, result in ax
OPL3_EnvelopeCalcExpOutPlusEnvShift3 MACRO
    mov bx, envelope
    shl bx, 3
    add bx, ax

    ;;;;;;;;; EnvelopeCalcExp inline
    cmp bx, 01fffh
    jbe __no1fff
    mov bx, 01fffh
__no1fff:
    mov cl, bh ; BX = level clipped
    and bx, 00ffh
    add bx, bx
    mov ax, exprom[bx]
    add ax, ax  ; BX = exprom[level & 0xff] << 1
    shr ax, cl  ; BX = (exprom[level & 0xff] << 1) >> (level >> 8)
    ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    ENDM


OPL3_EnvelopeCalcSin0Fast PROC C phase:WORD, envelope:WORD
    xor dx, dx
    mov bx, phase
    test bx, 0200h
    jz _sin0no200
    mov dx, 0ffffh
_sin0no200:
    test bx, 0100h
    jz _sin0no100
    xor bl, 0ffh
_sin0no100:
    sub bh, bh
    add bx, bx
    mov ax, logsinrom[bx]

    ; ax = OPL3_EnvelopeCalcExp(out + envelope << 3)
    OPL3_EnvelopeCalcExpOutPlusEnvShift3

    ; envelopeCalcExp result ^ neg
    xor ax, dx
    ret    

OPL3_EnvelopeCalcSin0Fast ENDP





OPL3_EnvelopeCalcSin1Fast PROC C phase:WORD, envelope:WORD
    mov bx, phase
    test bx, 0200h          ;if (phase & 0x200) out = 0x1000
    jz _sin1no200
    mov ax, 01000h
    jmp _sin1cont
_sin1no200:                 ; else
    test bx, 0100h
    jz _sin1no100
    xor bl, 0ffh
_sin1no100:
    sub bh, bh
    add bx, bx
    mov ax, logsinrom[bx]

_sin1cont:

    ; ax = OPL3_EnvelopeCalcExp(out + envelope << 3)
    OPL3_EnvelopeCalcExpOutPlusEnvShift3

    ret    
OPL3_EnvelopeCalcSin1Fast ENDP


OPL3_EnvelopeCalcSin2Fast PROC C phase:WORD, envelope:WORD
    mov bx, phase
    test bx, 0100h
    jz _sin2no100
    xor bl, 0ffh
_sin2no100:
    sub bh, bh
    add bx, bx
    mov ax, logsinrom[bx]

    ; ax = OPL3_EnvelopeCalcExp(out + envelope << 3)
    OPL3_EnvelopeCalcExpOutPlusEnvShift3
    ret    
OPL3_EnvelopeCalcSin2Fast ENDP


OPL3_EnvelopeCalcSin3Fast PROC C phase:WORD, envelope:WORD
    mov bx, phase
    test bx, 0100h
    jz _sin3no100
    mov ax, 01000h
    jmp _sin3cont
_sin3no100:
    sub bh, bh
    add bx, bx
    mov ax, logsinrom[bx]
_sin3cont:
    ; ax = OPL3_EnvelopeCalcExp(out + envelope << 3)
    OPL3_EnvelopeCalcExpOutPlusEnvShift3
    ret    
OPL3_EnvelopeCalcSin3Fast ENDP


OPL3_EnvelopeCalcSin4Fast PROC C phase:WORD, envelope:WORD
    xor dx, dx
    mov bx, phase
    ; if phase & 0x100 neg = 0xffff
    test bx, 0100h
    jz _sin4no100
    mov dx, 0ffffh
_sin4no100:

    ; if phase & 0x200 out = 0x1000
    test bx, 0200h
    jz _sin4no200
    mov ax, 01000h
    jmp _sin4cont

_sin4no200:

    ; else if phase & 0x80 out = logsinrom[(phase^0xff<<1)&0xff]
    test bx, 080h
    jz _sin4no80

    xor bl, 0ffh
    shl bl, 1
    sub bh, bh
    add bx, bx
    mov ax, logsinrom[bx]
    jmp _sin4cont

_sin4no80:
    ; else out = logsinrom[phase<<1]
    shl bx, 2
    sub bh, bh
    mov ax, logsinrom[bx]
_sin4cont:
    ; ax = OPL3_EnvelopeCalcExp(out + envelope << 3)
    OPL3_EnvelopeCalcExpOutPlusEnvShift3

    ; envelopeCalcExp result ^ neg
    xor ax, dx
    ret    
OPL3_EnvelopeCalcSin4Fast ENDP


OPL3_EnvelopeCalcSin5Fast PROC C phase:WORD, envelope:WORD
    mov bx, phase
    ; if phase & 0x200 out = 0x1000
    test bx, 0200h
    jz _sin5no200
    mov ax, 01000h
    jmp _sin5cont

_sin5no200:

    ; else if phase & 0x80 out = logsinrom[(phase^0xff<<1)&0xff]
    test bx, 080h
    jz _sin5no80

    xor bl, 0ffh
    shl bl, 1
    sub bh, bh
    add bx, bx
    mov ax, logsinrom[bx]
    jmp _sin5cont

_sin5no80:
    ; else out = logsinrom[phase<<1]
    shl bx, 2
    sub bh, bh
    mov ax, logsinrom[bx]
_sin5cont:
    ; ax = OPL3_EnvelopeCalcExp(out + envelope << 3)
    OPL3_EnvelopeCalcExpOutPlusEnvShift3
    ret
OPL3_EnvelopeCalcSin5Fast ENDP



OPL3_EnvelopeCalcSin6Fast PROC C phase:WORD, envelope:WORD
    mov dx, 0
    ; if phase & 0x200 neg = 0xffff
    mov bx, phase
    test bx, 0200h
    jz _sin6no200
    mov dx, 0ffffh
_sin6no200:
    mov ax, 0
    ; ax = OPL3_EnvelopeCalcExp(envelope << 3) <-- note no out this time
    OPL3_EnvelopeCalcExpOutPlusEnvShift3

    ; envelopeCalcExp result ^ neg
    xor ax, dx
    ret    
OPL3_EnvelopeCalcSin6Fast ENDP


OPL3_EnvelopeCalcSin7Fast PROC C phase:WORD, envelope:WORD
    mov dx, 0
    ; if phase & 0x200 neg = 0xffff, phase = phase & 1ff ^ 1ff
    mov ax, phase
    and ax, 03ffh
    test ax, 0200h
    jz _sin7no200
    mov dx, 0ffffh
    and ax, 01ffh
    xor ax, 01ffh
_sin7no200:
    shl ax, 3
    ; ax = OPL3_EnvelopeCalcExp(out + envelope << 3)
    OPL3_EnvelopeCalcExpOutPlusEnvShift3

    ; envelopeCalcExp result ^ neg
    xor ax, dx
    ret    
OPL3_EnvelopeCalcSin7Fast ENDP

ENDIF

    END