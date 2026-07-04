; pellet_eat — the ball ate a pellet. The most frequent cue in the game, so it is the QUIETEST and
; SHORTEST of the set: a tiny high tick on pulse CH2, 25% duty (~1800 Hz), low initial volume with the
; fastest decay — present as a texture, never fatiguing. Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $40
ldh [rNR21], a    ; CH2 duty 25% (thin)
ld a, $61
ldh [rNR22], a    ; envelope: initial volume 6, decrease, pace 1 (near-instant fade)
ld a, $B7
ldh [rNR23], a    ; frequency low  — period $7B7 → ~1800 Hz
ld a, $87
ldh [rNR24], a    ; trigger + frequency high
.idle:
jr .idle
