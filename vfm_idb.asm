; VIA_AC97.866 FM Emulation TSR
; 
; (C) 2025 Eric Voirin (Oerg866)
;
; LICENSE: CC-BY-NC-SA 4.0
;
; FM Emulation TSR Interrupt Handler Data & Assembly Code
;
; DOSBox OPL Core version

    INCLUDE vfm_icmn.asm

Chip_Generate   PROTO NEAR C, opl3_chip:PTR WORD, buf:PTR WORD, count:WORD
Chip_WriteReg   PROTO NEAR C, opl3_chip:PTR WORD, reg:WORD, data:BYTE

vfm_dmaInterruptHandler PROC FAR
    int 3

    SWAP_STACK      g_DMA_Backup, g_DMA_StackTop
    
    ; Actual PCI Interrupt handler code

    ; Read FM SGD Status register
    pushad
    push es

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
;M    out 080h, al

    ;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
    ; Next step: Process pending OPL register writes
    ; So we don't miss any note-on events we must generate one sample per write
   
    mov di, offset g_vfm_fmDmaBuffers   ; DI = DMA buffer pointer array
    add di, ax                          ; Calculate pointer to our current index's buffer pointer (sorry for the confusion)
    add di, ax
    mov di, [di]                        ; DI = Write Pointer to DMA buffer

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
    call Chip_WriteReg
    add sp, 6

    push 1
    push di
    push offset g_vfm_oplChip
    call Chip_Generate  
    add sp, 6

    pop cx

    add di, 2*2 ; Advance stream pointer
    add si, 3   ; next register
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

;_generateStream:
    ; Call OPL emulator, c doesnt save the regs here :(

    push cx
    push di
    push offset g_vfm_oplChip
    call Chip_Generate  
    add sp, 6

_generateStreamSkip:

    ; Ack the interrupt to clear it, writing FLAG and EOL to clear them
    mov al, SGD_CHANNEL_STATUS_FLAG OR SGD_CHANNEL_STATUS_EOL
    mov dx, word ptr [g_vfm_ioBaseDma]
    add dx, VFM_IO_FM_SGD_STATUS
    out dx, al

_skipIrqAck:
    pop es
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
; We have a queue which gets processed in the DMA interrupt, so we add the register there
;
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