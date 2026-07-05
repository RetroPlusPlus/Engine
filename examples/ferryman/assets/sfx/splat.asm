; splat — a mutant baited into traffic (also voices a colonist lost off the top, its dark echo).
; A low, wet noise crunch: 15-bit LFSR at a low clock so it squelches rather than hisses, snappy
; envelope. Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $00
ldh [rNR41], a    ; length unused (the envelope ends the sound)
ld a, $B2
ldh [rNR42], a    ; envelope: initial volume 11, decrease, pace 2 (a heavy crack)
ld a, $56
ldh [rNR43], a    ; noise: clock shift 5, 15-bit LFSR, divisor 6 — the low squelch
ld a, $80
ldh [rNR44], a    ; trigger
.idle:
jr .idle
