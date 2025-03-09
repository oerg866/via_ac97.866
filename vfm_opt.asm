; VIA_AC97.866 FM Emulation TSR
; 
; (C) 2025 Eric Voirin (Oerg866)
;
; LICENSE: CC-BY-NC-SA 4.0
;
; FM Emulation TSR Interrupt Handler Data & Assembly Code

    .model small, c
    .586p

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

    END
