; VIA_AC97.866 FM Emulation TSR
; 
; (C) 2025 Eric Voirin (Oerg866)
;
; LICENSE: CC-BY-NC-SA 4.0
;
; FM Emulation TSR Interrupt Handler Data & Assembly Code

    .model small, c
    .586p

vfm_tsrProcessRegistersAndGenerate  PROTO NEAR C, bankedIndex:WORD, data:WORD
vfm_processOPLWriteFromNmi          PROTO NEAR C

; DMA_BUFFER_COUNT            EQU 2
; DMA_SAMPLES_PER_BUFFER      EQU 192
STEREO                      EQU 2

DMATABLEENTRY STRUC
    dd address
    dd countflags
DMATABLEENTRY ENDS


OPLQUEUEENTRY STRUC
    dw bankedIndex
    db data
OPLQUEUEENTRY ENDS

    .data

; General data
EXTERN g_vfm_slaveIrq:              BYTE
EXTERN g_vfm_ioBaseDma:             WORD
EXTERN g_vfm_ioBaseNmi:             WORD
EXTERN g_vfm_oplChip:               PTR

EXTERN g_vfm_fmDmaTable:            PTR DMATABLEENTRY
EXTERN g_vfm_fmDmaBuffers:          PTR WORD
EXTERN g_vfm_fmDmaTablePhysAddress: DWORD

EXTERN g_vfm_oldPciIsr:             PTR PROC
EXTERN g_vfm_oldNmiIsr:             PTR PROC

; Globals
PUBLIC g_DMA_IRQOccured
PUBLIC g_DMA_BufferIndex

; Stack for PCI DMA Interrupt

g_NMI_Backup                dw 4    dup (0)
g_NMI_Stack                 db 256  dup (0)
g_NMI_StackTop              = $

; Stack for PCI DMA Interupt 

g_DMA_Backup                dw 4    dup (0)
g_DMA_Stack                 db 768  dup (0)
g_DMA_StackTop              = $

; NMI IRQ stuff

g_NMI_Success               db 1
g_NMI_Busy                  db 1

; DMA IRQ stuff

g_DMA_OurIRQ                db 0
g_DMA_IRQOccured            db 0

g_DMA_BufferIndex           dw 0

; OPL Register Write Queue

OPL_REG_QUEUE_SIZE          EQU (DMA_SAMPLES_PER_BUFFER-1)
g_OPL_RegQueue              OPLQUEUEENTRY OPL_REG_QUEUE_SIZE dup (<0, 0>)
g_OPL_RegCout               dw 0

; FM SGD Register definitions
SGD_CHANNEL_STATUS_ACTIVE   EQU 80h
SGD_CHANNEL_STATUS_PAUSED   EQU 40h
SGD_CHANNEL_STATUS_QUEUED   EQU 08h
SGD_CHANNEL_STATUS_STOPPED  EQU 04h
SGD_CHANNEL_STATUS_EOL      EQU 02h
SGD_CHANNEL_STATUS_FLAG     EQU 01h


    .code

;vfm_tsrProcessRegistersAndGenerate  PROTO NEAR C, bankedIndex:WORD, data:WORD
;vfm_processOPLWriteFromNmi          PROTO NEAR C
OPL3_Generate2ChResampled           PROTO NEAR C, opl3_chip:PTR WORD, buf:PTR WORD
OPL3_WriteReg                       PROTO NEAR C, opl3_chip:PTR WORD, reg:WORD, data:BYTE

SWAP_STACK  MACRO BACKUP, NEWSTACK
    cli
    mov [BACKUP+0], ax
    mov [BACKUP+2], ss
    mov [BACKUP+4], sp
    mov [BACKUP+6], bp
    mov ax, ds
    mov ss, ax
    mov sp, [NEWSTACK]
    mov bp, [NEWSTACK]
    sti
    ENDM

RESTORE_STACK MACRO BACKUP
    cli
    mov ax, [BACKUP+0]
    mov ss, [BACKUP+2]
    mov sp, [BACKUP+4]
    mov bp, [BACKUP+6]
    sti
    ENDM

vfm_dmaInterruptHandler PROC FAR
    int 3

    SWAP_STACK      g_DMA_Backup, g_DMA_StackTop
    
    ; Actual PCI Interrupt handler code

    ; Read FM SGD Status register
    push bx
    push cx
    push dx
    push eax
    mov dx, [g_vfm_ioBaseDma]
    add dx, 20h
    in al, dx

    ; Is EOL or FLAG set?
    mov g_DMA_OurIRQ, 0
    test al, SGD_CHANNEL_STATUS_FLAG OR SGD_CHANNEL_STATUS_EOL
    jz _skipIrqAck

    ; It's our IRQ! Let's handle it
    mov g_DMA_OurIRQ, 1

    ; Signal to the outside
    mov g_DMA_IRQOccured, 1
    
    ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    ; Next step: update buffer writing position
     
    
    ; Get NMI current pointer table entry address
    add dx, 4
    in eax, dx
    ; Subtract base address and divide by 8 to get the bank index
    sub eax, [g_vfm_fmDmaTablePhysAddress]
    shr eax, 3
    ; Essentially, the bank index to write to is is that index minus 1
    dec al
    ; underflow?
    jns _noNegBufferAdj
    
    ; Adjust
    mov al, DMA_BUFFER_COUNT - 1
   
_noNegBufferAdj:
    ; Update index with final value
    mov g_DMA_BufferIndex, ax

    ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    ; Next step: Process pending OPL register writes
    ; So we don't miss any note-on events we must generate one sample per write

    cli                             ; This shouldn't be interrupted by anything
    mov bx, ax                      ; Calculate index into buffer pointer list
    add bx, bx                      ;
    mov ax, g_vfm_fmDmaBuffers[bx]  ; AX = DMA stream pointer
    mov cx, g_OPL_RegCout  ; CX = OPL Register writes to process
    mov dx, DMA_SAMPLES_PER_BUFFER  ; DX Bytes to write
    ; Do we have register writes to process? If not, skip this step
    or cx, cx
    jz _processRegistersSkip

    ; Current register index
    mov bx, 0

_processRegisters:
    ; Write register to emualted opl chip
    push g_OPL_RegQueue[bx+2]       ; data
    push g_OPL_RegQueue[bx+0]       ; bankedIndex
    push offset g_vfm_oplChip       ; opl3_chip
    call OPL3_WriteReg
    add sp, 6

    ; Generate one sample for this register write
    push ax
    push offset g_vfm_oplChip
    call OPL3_Generate2ChResampled
    add sp, 4

    add ax, 2*2 ; Advance stream pointer
    add bx, 3   ; next register
    dec dx
    dec cx
    jnz _processRegisters

_processRegistersSkip:
 
    ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    ; DONE WITH REGISTER PROCESSING, now just generate the rest of the stream.
    ; Skip stream generation if there is nothing left to generate
    ; (unlikely but you never know how spammy programs are)
    or dx, dx
    jz _generateStreamSkip

    ; LOOP START
_generateStream:
    ; Call OPL emulator
    push ax
    push offset g_vfm_oplChip
    call OPL3_Generate2ChResampled
    add sp, 4

    add ax, 2*2 ; Advance stream pointer
    dec dx      ; Next sample
    jz _generateStream

_generateStreamSkip:
    sti

    ; Generate the rest of the buffer
    mov cx, DMA_SAMPLES_PER_BUFFER

    ; debug output current status
    out 080h, al

    ; Ack the interrupt to clear it, writing FLAG and EOL to clear them
    mov al, SGD_CHANNEL_STATUS_FLAG OR SGD_CHANNEL_STATUS_EOL
    mov dx, g_vfm_ioBaseDma
    out dx, al

_skipIrqAck:
    pop eax
    pop dx
    pop cx
    pop bx
    ; Restore interrupted program's stack
    RESTORE_STACK   g_DMA_Backup

    ; Did we handle this IRQ? If not, chain to next ISR
    cmp g_DMA_OurIRQ, 1
    jnz _jmpToOldIrq

    ; We handled the ISR, 
_weHandledIRQ:
    cmp g_vfm_slaveIrq, 1
    jz _noSlaveEOI

    ; EOI to slave PIC
    mov al, 20h
    out 0A0h, al

_noSlaveEOI:
    ; EOI to master PIC
    mov al, 20h
    out 20h, al

    ; Done :3
    mov ax, [g_DMA_Backup]
    iret

    ; Jump to other ISR
_jmpToOldIrq:
    jmp far ptr [far ptr g_vfm_oldPciIsr]

vfm_dmaInterruptHandler ENDP

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; NMI / SMI Handler
;
vfm_nmiHandler PROC FAR
    int 3

    ; Double NMI?!?? Not good...
    test g_NMI_Busy, 1
    jnz _nmiIsBusy

    ; Mark NMI as busy, swap stack
    mov g_NMI_Busy, 1
    SWAP_STACK      g_NMI_Backup, g_NMI_StackTop

    push bx
    push dx
    ; Pre-set to failure
    mov g_NMI_Success, 0

    ; Fetch NMI Status byte
    mov dx, g_vfm_ioBaseNmi
    in al, dx
    and al, 3
    dec al

    ; AL = bank number, move it to high byte for OPL3 emulator api
    mov ah, al

    ; Bank index != 0 or 1 -> invalid or not our NMI, get out
    cmp al, 2
    jge _nmiDone

    ; It's our write to handle, so let's do that.
    add dx, 2   ; Index port
    in al, dx

    ; BX = bank index + register index
    mov bx, ax

    ; Get Data
    dec dx
    in al, dx

    ; !! Add register to write queue !!
    cli

    ; Is the write queue full?
    cmp [g_OPL_RegCout], OPL_REG_QUEUE_SIZE
    ; if not, proceed
    jl _addRegWriteToQueue
    
    ; Write queue full, flag failure and get out
    mov al, 0FEh
    out 080h, al
    sti
    jmp _nmiDone    

    ; Write queue not full, proceed
_addRegWriteToQueue:
    ; Struct is 3 bytes, so multiply the index by 3
    mov dx, bx              ; We need bx register, so move the bankedIndex to dx
    mov bx, [g_OPL_RegCout] ; bx = count * 3
    add bx, bx
    add bx, [g_OPL_RegCout]

    mov word ptr g_OPL_RegQueue[bx + 0], dx  ; bankedIndex
    mov byte ptr g_OPL_RegQueue[bx + 1], al  ; data

    ; Increment counter
    inc [g_OPL_RegCout]
    sti

    ; All done and successful!

    mov g_NMI_Success, 1

_nmiDone:
    pop dx
    pop bx

    ; Restore stack and unflag busy
    RESTORE_STACK   g_NMI_Backup
    mov g_NMI_Busy, 0

    ; Did we handle it successfully? if not, call the previous NMI handler
    test g_NMI_Success, 1
    jz _callOldNmi

    iret

    ; TSR Signature
    DW 0866h
    DW 0866h
    DW 0AC97h
    DW 0AC97h

    ; NMI is busy; flag this as a warning to the POST debug port
_nmiIsBusy:
    mov [g_NMI_Backup], ax
    mov al, 0DEh
    out 080h, al
    mov ax, [g_NMI_Backup]

_callOldNmi:
    ; NMI is either busy or wasn't for us, jmp to previous NMI handler
    jmp far ptr [far ptr g_vfm_oldNmiIsr]

vfm_nmiHandler ENDP

vfm_nmiHandlerEnd PROC FAR
vfm_nmiHandlerEnd ENDP
    END