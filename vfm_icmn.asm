    .model small, c
    .586p

    .data

STEREO                      EQU 2

DMATABLEENTRY STRUC
    dd address
    dd countflags
DMATABLEENTRY ENDS

OPLQUEUEENTRY STRUC
    dw bankedIndex
    db data
OPLQUEUEENTRY ENDS

SWAP_STACK  MACRO BACKUP, NEWSTACK
    cli
    cld
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

; OPL Register Write Queue
OPL_REG_QUEUE_SIZE          EQU (SAMPS_PER_BUF-1)
g_OPL_RegQueue              OPLQUEUEENTRY OPL_REG_QUEUE_SIZE dup (<0, 0>)
g_OPL_RegCount               dw 0

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

; Register backups for setting up custom stacks
; These have to be in code segment so we can access it
g_DMA_Backup                dw 5    dup (0AA55h)
g_NMI_Backup                dw 5    dup (0AA55h)

; After swapping back the original segment registers, we can no longer access DS,
; so we copy the chain ISR addresses to the code segment
g_vfm_oldPciIsr             dd 0
g_vfm_oldNmiIsr             dd 0
