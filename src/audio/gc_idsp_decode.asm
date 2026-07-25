; Wii/GameCube DSP task for Nintendo DSP-ADPCM blocks.
; The CPU supplies already de-interleaved channel frames and a command block.
; The accelerator performs the predictor/scale decode; this task only moves
; state and samples between the accelerator, DSP RAM, and main RAM.

DMACR:              equ     0xffc9
DMABLEN:            equ     0xffcb
DMADSPM:            equ     0xffcd
DMAMMEMH:           equ     0xffce
DMAMMEML:           equ     0xffcf

ACCOEF:             equ     0xffa0
ACFMT:              equ     0xffd1
ACSAH:              equ     0xffd4
ACSAL:              equ     0xffd5
ACEAH:              equ     0xffd6
ACEAL:              equ     0xffd7
ACCAH:              equ     0xffd8
ACCAL:              equ     0xffd9
ACPDS:              equ     0xffda
ACYN1:              equ     0xffdb
ACYN2:              equ     0xffdc
ACDAT:              equ     0xffdd
ACGAN:              equ     0xffde

DIRQ:               equ     0xfffb
DMBH:               equ     0xfffc
DMBL:               equ     0xfffd
CMBH:               equ     0xfffe
CMBL:               equ     0xffff

DMA_TO_DSP:         equ     0x0000
DMA_TO_CPU:         equ     0x0001

; Command block is 64 DSP words (128 bytes), big-endian on Wii.
CMD_ADDR:           equ     0x0100
CMD_LEFT_START_H:   equ     CMD_ADDR+0
CMD_LEFT_START_L:   equ     CMD_ADDR+1
CMD_LEFT_END_H:     equ     CMD_ADDR+2
CMD_LEFT_END_L:     equ     CMD_ADDR+3
CMD_LEFT_CUR_H:     equ     CMD_ADDR+4
CMD_LEFT_CUR_L:     equ     CMD_ADDR+5
CMD_RIGHT_START_H:  equ     CMD_ADDR+6
CMD_RIGHT_START_L:  equ     CMD_ADDR+7
CMD_RIGHT_END_H:    equ     CMD_ADDR+8
CMD_RIGHT_END_L:    equ     CMD_ADDR+9
CMD_RIGHT_CUR_H:    equ     CMD_ADDR+10
CMD_RIGHT_CUR_L:    equ     CMD_ADDR+11
CMD_OUTPUT_H:       equ     CMD_ADDR+12
CMD_OUTPUT_L:       equ     CMD_ADDR+13
CMD_SAMPLE_COUNT:   equ     CMD_ADDR+14
CMD_FRAME_COUNT:    equ     CMD_ADDR+15
CMD_CHANNEL_COUNT:  equ     CMD_ADDR+16
CMD_LEFT_PRED:      equ     CMD_ADDR+17
CMD_RIGHT_PRED:     equ     CMD_ADDR+18
CMD_LEFT_HIST1:     equ     CMD_ADDR+19
CMD_LEFT_HIST2:     equ     CMD_ADDR+20
CMD_RIGHT_HIST1:    equ     CMD_ADDR+21
CMD_RIGHT_HIST2:    equ     CMD_ADDR+22
CMD_LEFT_COEFS:     equ     CMD_ADDR+23
CMD_RIGHT_COEFS:    equ     CMD_ADDR+39

; Scratch/output layout. The largest supported command is 868 samples.
LEFT_PCM:           equ     0x0200
RIGHT_PCM:          equ     0x0580
OUT_PCM:            equ     0x0900
LEFT_STATE1:        equ     CMD_ADDR+60
LEFT_STATE2:        equ     CMD_ADDR+61

_start:
    nop
    nop
    jmp     exception1
    jmp     exception2
    jmp     exception3
    jmp     exception4
    jmp     exception5
    jmp     exception6
    jmp     exception7

init_task:
    sbset   #0x02
    sbset   #0x03
    sbclr   #0x04
    sbset   #0x05
    sbset   #0x06
    s16
    clr15
    m0
    lri     $config,#0xff
    lri     $wr0,#0xffff
    lri     $wr1,#0xffff
    lri     $wr2,#0xffff
    lri     $wr3,#0xffff
    si      @DMBH,#0xdcd1
    si      @DMBL,#0x0000
    si      @DIRQ,#0x01

wait_commands:
    clr     $acc0
    clr     $acc1
    call    wait_mail_sent
    call    wait_mail_recv
    ; CMBH bit 15 is the CPU-mailbox-full status flag, not payload.
    andi    $acc1.m,#0x7fff
    lri     $acc0.m,#0x1d5a
    cmp
    jne     wait_commands
    lrs     $acc1.m,@CMBL
    cmpi    $acc1.m,#0x0001
    jeq     decode_command
    cmpi    $acc1.m,#0x0002
    jeq     shutdown_task
    jmp     wait_commands

decode_command:
    ; The next mailbox word pair is the physical address of the command block.
    call    wait_mail_recv
    lrs     $acc0.m,@CMBH
    andi    $acc0.m,#0x7fff
    lri     $ar1,#CMD_ADDR+62
    srri    @$ar1,$acc0.m
    srs     @DMAMMEMH,$acc0.m
    call    wait_mail_recv
    lrs     $acc0.m,@CMBL
    srri    @$ar1,$acc0.m
    srs     @DMAMMEML,$acc0.m
    si      @DMACR,#DMA_TO_DSP
    si      @DMADSPM,#CMD_ADDR
    si      @DMABLEN,#128
    call    wait_dma

    call    decode_left
    lri     $ar1,#CMD_CHANNEL_COUNT
    lrri    $acc0.m,@$ar1
    cmpi    $acc0.m,#0x0001
    jeq     decode_ready
    call    decode_right

decode_ready:
    call    interleave
    call    dma_output
    si      @DMBH,#0xdcd1
    ; DCD10004 is dispatched to the task request callback by libogc.
    si      @DMBL,#0x0004
    si      @DIRQ,#0x01
    jmp     wait_commands

shutdown_task:
    si      @DMBH,#0xdcd1
    si      @DMBL,#0x0003
    si      @DIRQ,#0x01
    halt

decode_left:
    call    setup_left
    lri     $ar0,#LEFT_PCM
    lri     $ar1,#CMD_SAMPLE_COUNT
    lrri    $acc0.m,@$ar1
    mrr     $acx0.l,$acc0.m
    bloop   $acx0.l,decode_left_end
    lrs     $acc0.m,@ACDAT
decode_left_end:
    srri    @$ar0,$acc0.m
    lrs     $acc0.m,@ACYN1
    lri     $ar1,#LEFT_STATE1
    srri    @$ar1,$acc0.m
    lrs     $acc0.m,@ACYN2
    srri    @$ar1,$acc0.m
    ret

decode_right:
    call    setup_right
    lri     $ar0,#RIGHT_PCM
    lri     $ar1,#CMD_SAMPLE_COUNT
    lrri    $acc0.m,@$ar1
    mrr     $acx0.l,$acc0.m
    bloop   $acx0.l,decode_right_end
    lrs     $acc0.m,@ACDAT
decode_right_end:
    srri    @$ar0,$acc0.m
    ret

setup_left:
    si      @ACFMT,#0x0000
    lri     $ar1,#CMD_LEFT_START_H
    lrri    $acc0.m,@$ar1
    srs     @ACSAH,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACSAL,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACEAH,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACEAL,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCAH,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCAL,$acc0.m
    lri     $ar1,#CMD_LEFT_COEFS
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+0,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+1,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+2,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+3,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+4,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+5,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+6,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+7,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+8,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+9,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+10,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+11,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+12,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+13,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+14,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+15,$acc0.m

    ; Match Nintendo AX: writing YN2 last arms/resets accelerator reads.
    si      @ACGAN,#0x0000
    lri     $ar1,#CMD_LEFT_PRED
    lrri    $acc0.m,@$ar1
    srs     @ACPDS,$acc0.m
    lri     $ar1,#CMD_LEFT_HIST1
    lrri    $acc0.m,@$ar1
    srs     @ACYN1,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACYN2,$acc0.m
    ret

setup_right:
    si      @ACFMT,#0x0000
    lri     $ar1,#CMD_RIGHT_START_H
    lrri    $acc0.m,@$ar1
    srs     @ACSAH,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACSAL,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACEAH,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACEAL,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCAH,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCAL,$acc0.m
    lri     $ar1,#CMD_RIGHT_COEFS
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+0,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+1,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+2,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+3,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+4,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+5,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+6,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+7,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+8,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+9,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+10,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+11,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+12,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+13,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+14,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACCOEF+15,$acc0.m

    ; Match Nintendo AX: writing YN2 last arms/resets accelerator reads.
    si      @ACGAN,#0x0000
    lri     $ar1,#CMD_RIGHT_PRED
    lrri    $acc0.m,@$ar1
    srs     @ACPDS,$acc0.m
    lri     $ar1,#CMD_RIGHT_HIST1
    lrri    $acc0.m,@$ar1
    srs     @ACYN1,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @ACYN2,$acc0.m
    ret

interleave:
    lri     $ar0,#LEFT_PCM
    lri     $ar1,#RIGHT_PCM
    lri     $ar2,#OUT_PCM
    lri     $ar3,#CMD_SAMPLE_COUNT
    lrri    $acc0.m,@$ar3
    mrr     $acx0.l,$acc0.m
    lri     $ar3,#CMD_CHANNEL_COUNT
    lrri    $acc0.m,@$ar3
    cmpi    $acc0.m,#0x0001
    jeq     interleave_mono
    bloop   $acx0.l,interleave_stereo_end
    lrri    $acc0.m,@$ar0
    lrri    $acc1.m,@$ar1
    srri    @$ar2,$acc0.m
interleave_stereo_end:
    srri    @$ar2,$acc1.m
    jmp     interleave_state

interleave_mono:
    bloop   $acx0.l,interleave_mono_end
    lrri    $acc0.m,@$ar0
    srri    @$ar2,$acc0.m
interleave_mono_end:
    srri    @$ar2,$acc0.m

interleave_state:
    lri     $ar1,#CMD_SAMPLE_COUNT
    lrri    $acc0.m,@$ar1
    asl     $acc0,#1
    mrr     $ix0,$acc0.m
    lri     $ar2,#OUT_PCM
    addarn  $ar2,$ix0
    lri     $ar1,#LEFT_STATE1
    lrri    $acc0.m,@$ar1
    srri    @$ar2,$acc0.m
    lrri    $acc0.m,@$ar1
    srri    @$ar2,$acc0.m
    lrs     $acc0.m,@ACYN1
    srri    @$ar2,$acc0.m
    lrs     $acc0.m,@ACYN2
    srri    @$ar2,$acc0.m
    ret

dma_output:
    lri     $ar1,#CMD_OUTPUT_H
    lrri    $acc0.m,@$ar1
    srs     @DMAMMEMH,$acc0.m
    lrri    $acc0.m,@$ar1
    srs     @DMAMMEML,$acc0.m
    si      @DMADSPM,#OUT_PCM
    lri     $ar1,#CMD_SAMPLE_COUNT
    lrri    $acc0.m,@$ar1
    asl     $acc0,#2
    addi    $acc0.m,#8
    si      @DMACR,#DMA_TO_CPU
    ; Writing DMABLEN starts the transfer, so the direction must be set first.
    srs     @DMABLEN,$acc0.m
    call    wait_dma
    ret

wait_dma:
    lrs     $acc0.m,@DMACR
    andf    $acc0.m,#0x0004
    jlnz    wait_dma
    ret

wait_mail_sent:
    lrs     $acc0.m,@DMBH
    andf    $acc0.m,#0x8000
    jlnz    wait_mail_sent
    ret

wait_mail_recv:
    lrs     $acc1.m,@CMBH
    andcf   $acc1.m,#0x8000
    jlnz    wait_mail_recv
    ret

exception1:
    rti
exception2:
    rti
exception3:
    rti
exception4:
    rti
exception5:
    rti
exception6:
    rti
exception7:
    rti
