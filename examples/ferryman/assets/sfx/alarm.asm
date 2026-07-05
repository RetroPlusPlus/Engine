; alarm — an abductor's beam lit / a mutant arrived: the you-are-being-robbed klaxon. The set's
; one TWO-NOTE cue: a high honk, a timed gap (a busy-wait loop — the VM runs at hardware speed,
; so cycles are real time), then a lower honk that rings out. Unmistakably a warning, never a
; reward. Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $80
ldh [rNR21], a    ; CH2 duty 50%
ld a, $A4
ldh [rNR22], a    ; envelope: initial volume 10, decrease, pace 4
ld a, $51
ldh [rNR23], a    ; note 1 frequency low — period $751 → ~750 Hz
ld a, $87
ldh [rNR24], a    ; trigger note 1
ld bc, $6000      ; ~0.16 s of busy-wait between the honks (28 cycles/iteration @ 4.19 MHz)
.gap:
dec bc
ld a, b
or c
jr nz, .gap
ld a, $A6
ldh [rNR22], a    ; note 2 rings a touch longer (pace 6)
ld a, $09
ldh [rNR23], a    ; note 2 frequency low — period $709 → ~530 Hz
ld a, $87
ldh [rNR24], a    ; trigger note 2
.idle:
jr .idle
