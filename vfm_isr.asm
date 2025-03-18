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

; NUM_BUFS            EQU 2
; SAMPS_PER_BUF      EQU 192
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


; Globals
PUBLIC g_DMA_IRQOccured
PUBLIC g_DMA_BufferIndex
PUBLIC g_vfm_oldPciIsr
PUBLIC g_vfm_oldNmiIsr

; NMI IRQ stuff

g_NMI_Success               db 0
g_NMI_Busy                  db 0

; DMA IRQ stuff

g_DMA_OurIRQ                db 0
g_DMA_IRQOccured            db 0

g_DMA_BufferIndex           dw 0

; Our custom stacks
; Stack for PCI DMA Interupt 
g_DMA_Stack                 db 512  dup (0)
g_DMA_StackTop              = $

; Stack for NMI Interrupt
g_NMI_Stack                 db 512  dup (0)
g_NMI_StackTop              = $


IFDEF NUKED 
; OPL Register Write Queue

OPL_REG_QUEUE_SIZE          EQU (SAMPS_PER_BUF-1)
g_OPL_RegQueue              OPLQUEUEENTRY OPL_REG_QUEUE_SIZE dup (<0, 0>)
g_OPL_RegCount               dw 0
ENDIF

; FM SGD Register definitions
SGD_CHANNEL_STATUS_ACTIVE   EQU 080h
SGD_CHANNEL_STATUS_PAUSED   EQU 40h
SGD_CHANNEL_STATUS_QUEUED   EQU 08h
SGD_CHANNEL_STATUS_STOPPED  EQU 04h
SGD_CHANNEL_STATUS_EOL      EQU 02h
SGD_CHANNEL_STATUS_FLAG     EQU 01h

; I/O Offsets
VFM_IO_FM_SGD_STATUS        EQU 20h

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
;   Code segment

    .code

IFDEF DBOPL
Chip_Generate                       PROTO NEAR C, opl3_chip:PTR WORD, buf:PTR WORD, count:DWORD
Chip_WriteReg                       PROTO NEAR C, opl3_chip:PTR WORD, reg:WORD, data:BYTE
ELSE
OPL3_Generate2ChResampled           PROTO NEAR C, opl3_chip:PTR WORD, buf:PTR WORD
OPL3_WriteReg                       PROTO NEAR C, opl3_chip:PTR WORD, reg:WORD, data:BYTE
ENDIF

; Register backups for setting up custom stacks
; These have to be in code segment so we can access it
g_DMA_Backup                dw 5    dup (0AA55h)
g_NMI_Backup                dw 5    dup (0AA55h)

; After swapping back the original segment registers, we can no longer access DS,
; so we copy the chain ISR addresses to the code segment
g_vfm_oldPciIsr             dd 0
g_vfm_oldNmiIsr             dd 0

SWAP_STACK  MACRO BACKUP, NEWSTACK
    cli
    mov cs:[BACKUP+0], ax
    mov cs:[BACKUP+2], ss
    mov cs:[BACKUP+4], sp
    mov cs:[BACKUP+6], ds
    mov ax, SEG NEWSTACK
    mov ds, ax
    mov ss, ax
    lea sp, NEWSTACK
;    lea bp, NEWSTACK
    ENDM

RESTORE_STACK MACRO BACKUP
    mov ax, cs:[BACKUP+0]
    mov ss, cs:[BACKUP+2]
    mov sp, cs:[BACKUP+4]
    mov ds, cs:[BACKUP+6]
    ENDM

vfm_dmaInterruptHandler PROC FAR
;    int 3

    SWAP_STACK      g_DMA_Backup, g_DMA_StackTop
    
    ; Actual PCI Interrupt handler code

    ; Read FM SGD Status register
    pushad
    mov dx, [g_vfm_ioBaseDma]
    add dx, VFM_IO_FM_SGD_STATUS
    in al, dx

    ; Is EOL or FLAG set?
    mov g_DMA_OurIRQ, 0
    test al, SGD_CHANNEL_STATUS_FLAG OR SGD_CHANNEL_STATUS_EOL
    jz _skipIrqAck

    ; It's our IRQ! Let's handle it
    mov [g_DMA_OurIRQ], 1

    ; Signal to the outside
    mov [g_DMA_IRQOccured], 1
    
    ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    ; Next step: update buffer writing position
     
    
    ; Get NMI current pointer table entry address
    add dx, 4
    in eax, dx
    ; Subtract base address and divide by 8 to get the bank index
    sub eax, [g_vfm_fmDmaTablePhysAddress]
    shr eax, 3
    dec eax ; It's always pointing to the *next* table address so -1

    ; Essentially, the bank index to write to is is that index minus 1
    dec al
    ; underflow?
    jns _noNegBufferAdj
    
    ; Adjust
    mov al, NUM_BUFS - 1
   
_noNegBufferAdj:
    ; Update index with final value
    mov [g_DMA_BufferIndex], ax
    
    ; debug output current status
    out 080h, al

    ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    ; Next step: Process pending OPL register writes
    ; So we don't miss any note-on events we must generate one sample per write
   
    mov di, offset g_vfm_fmDmaBuffers   ; DI = DMA buffer pointer array
    add di, ax                          ; Calculate pointer to our current index's buffer pointer (sorry for the confusion)
    add di, ax
    mov di, [di]                        ; DI = Write Pointer to DMA buffer


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; NUKED-OPL GENERATION - Process register queue, generate one sample per register, then generate the rest of the stream
;
IFDEF NUKED

    mov cx, word ptr [g_OPL_RegCount]   ; CX = OPL Register writes to process

    ; Do we have register writes to process? If not, skip this step
    or cx, cx
    jz _processRegistersSkip

    ; Current register ptr
    mov si, offset g_OPL_RegQueue
_processRegisters:

    ; Write register to emualted opl chip
    push cx

    push word ptr [si+2]       ; data
    push word ptr [si+0]       ; bankedIndex
    push offset g_vfm_oplChip  ; opl3_chip
    call OPL3_WriteReg
    add sp, 6

    push di
    push offset g_vfm_oplChip
    call OPL3_Generate2ChResampled
    add sp, 4

    add di, 2*2 ; Advance stream pointer
    add si, 3   ; next register
    pop cx
    dec cx
    jnz _processRegisters

_processRegistersSkip:
    mov cx, SAMPS_PER_BUF      ; DX = Samples to write after processing registers
    sub cx, word ptr [g_OPL_RegCount]   ; Subtract register count
    
    jns _noError
; Etoooo bleh???
    int 3

_noError:

    mov word ptr [g_OPL_RegCount], 0    ; Reset register count for next iteration

    ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    ; DONE WITH REGISTER PROCESSING, now just generate the rest of the stream.
    ; Skip stream generation if there is nothing left to generate
    ; (unlikely but you never know how spammy programs are)
    or cx, cx
    jz _generateStreamSkip

    ; LOOP START
_generateStream:
    ; Call OPL emulator, c doesnt save the regs here :(

    push cx

    push di
    push offset g_vfm_oplChip
    call OPL3_Generate2ChResampled
    add sp, 4

    pop cx

    add di, 2*2 ; Advance stream pointer
    dec cx      ; Next sample
    jnz _generateStream

;    int 3

_generateStreamSkip:

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; Not NUKED but DBOPL - just generate everything in 1 go
;
ELSE 

    ; No idea how to push a DWORD in MASM 6.11 .... push dword doesn't work nor does push large. wtf?
    db 066h, 068h
    dd SAMPS_PER_BUF
    push di
    push offset g_vfm_oplChip
    call Chip_Generate
    add sp, 8

ENDIF

    ; Ack the interrupt to clear it, writing FLAG and EOL to clear them
    mov al, SGD_CHANNEL_STATUS_FLAG OR SGD_CHANNEL_STATUS_EOL
    mov dx, word ptr [g_vfm_ioBaseDma]
    add dx, VFM_IO_FM_SGD_STATUS
    out dx, al

_skipIrqAck:
    popad

    ; Did we handle this IRQ? If not, chain to next ISR
    cmp byte ptr [g_DMA_OurIRQ], 1
    jnz _jmpToOldIrq

    ; We handled the ISR, 
_weHandledIRQ:
    cmp byte ptr [g_vfm_slaveIrq], 1
    jnz _noSlaveEOI

    ; EOI to slave PIC
    mov al, 20h
    out 0A0h, al

_noSlaveEOI:
    ; EOI to master PIC
    mov al, 20h
    out 20h, al

    ; Done :3
    ; Restore interrupted program's stack
    RESTORE_STACK   g_DMA_Backup
    iret

    ; Jump to other ISR
_jmpToOldIrq:
    ; Restore interrupted program's stack
    RESTORE_STACK   g_DMA_Backup
    jmp cs:[g_vfm_oldPciIsr]

vfm_dmaInterruptHandler ENDP

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; NMI / SMI Handler
;
vfm_nmiHandler PROC FAR
    int 3

    SWAP_STACK      g_NMI_Backup, g_NMI_StackTop

    ; Double NMI?!?? Not good...
    test byte ptr [g_NMI_Busy], 1
    jnz _nmiIsBusy

    ; Mark NMI as busy, swap stack
    mov byte ptr [g_NMI_Busy], 1

    push bx
    push dx
    ; Pre-set to failure
    mov byte ptr [g_NMI_Success], 0

    ; Fetch NMI Status byte
    mov dx, word ptr [g_vfm_ioBaseNmi]
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

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; NUKED-OPL - We have a queue which gets processed in the DMA interrupt, so we add the register there
;

IFDEF NUKED

    ; Is the write queue full?
    cmp word ptr [g_OPL_RegCount], (OPL_REG_QUEUE_SIZE-2)
    ; if not, proceed
    jl _addRegWriteToQueue
    
    ; Write queue full, flag failure and get out
    mov al, 0FEh
    out 080h, al
    jmp _nmiDone

    ; Write queue not full, proceed
_addRegWriteToQueue:
    ; Struct is 3 bytes, so multiply the index by 3
    mov dx, bx              ; We need bx register, so move the bankedIndex to dx
    mov bx, word ptr [g_OPL_RegCount] ; bx = count * 3
    add bx, bx
    add bx, word ptr [g_OPL_RegCount]

    mov word ptr g_OPL_RegQueue[bx + 0], dx  ; bankedIndex
    mov byte ptr g_OPL_RegQueue[bx + 2], al  ; data

    ; Increment counter
    inc word ptr [g_OPL_RegCount]

    ; All done and successful!

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;
; DBOPL - We can just write registers whenever... Speeeeeeeeeeedddddddddddd!!!
;
ELSE
    sub ah, ah                  ; upper byte is dirty from before
    push ax                     ; data
    push bx                     ; bankedIndex
    push offset g_vfm_oplChip   ; opl3_chip
    call Chip_WriteReg
    add sp, 6
ENDIF

    mov byte ptr [g_NMI_Success], 1

_nmiDone:
    pop dx
    pop bx

    mov byte ptr [g_NMI_Busy], 0

    ; Did we handle it successfully? if not, call the previous NMI handler
    test byte ptr [g_NMI_Success], 1
    jz _callOldNmi

    ; Restore stack
    RESTORE_STACK   g_NMI_Backup
    iret

    ; TSR Signature
    DW 0866h
    DW 0866h
    DW 0AC97h
    DW 0AC97h

    ; NMI is busy; flag this as a warning to the POST debug port
_nmiIsBusy:
;    mov [g_NMI_Backup], ax
;    mov al, 0DEh
;    out 080h, al
;    mov ax, [g_NMI_Backup]

_callOldNmi:
    ; NMI is either busy or wasn't for us, jmp to previous NMI handler
    ; Restore stack
    RESTORE_STACK   g_NMI_Backup
    jmp cs:[g_vfm_oldNmiIsr]

vfm_nmiHandler ENDP

vfm_nmiHandlerEnd PROC FAR
vfm_nmiHandlerEnd ENDP
    END