; fire — the Manta's bolt. The most frequent cue in the game, so the QUIETEST and SHORTEST: a
; thin high tick on pulse CH2, 25% duty (~1600 Hz), low volume, near-instant decay. Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $40
ldh [rNR21], a    ; CH2 duty 25% (thin)
ld a, $61
ldh [rNR22], a    ; envelope: initial volume 6, decrease, pace 1
ld a, $AE
ldh [rNR23], a    ; frequency low  — period $7AE → ~1600 Hz
ld a, $87
ldh [rNR24], a    ; trigger + frequency high
.idle:
jr .idle
