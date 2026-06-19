; level_clear — the board is cleared. The reward cue: a bright, high "ding" on pulse CH2, 50% duty
; (~1050 Hz) with the slowest envelope of the set so it rings out a moment longer than any other SFX.
; Self-contained. (A multi-note arpeggio flourish is a possible S4 polish — single note keeps S2 reliable.)
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $80
ldh [rNR21], a    ; CH2 duty 50%
ld a, $D5
ldh [rNR22], a    ; envelope: initial volume 13, decrease, pace 5 (rings out)
ld a, $83
ldh [rNR23], a    ; frequency low  — period $783 → ~1050 Hz
ld a, $87
ldh [rNR24], a    ; trigger + frequency high
.idle:
jr .idle
