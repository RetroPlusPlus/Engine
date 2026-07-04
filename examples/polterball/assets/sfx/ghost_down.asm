; ghost_down — the ignited ball smashed a frightened ghost. A bright, high "pop" on pulse CH2
; (~1400 Hz), 50% duty, punchy envelope — clearly a reward, pitched above every ordinary bounce so a
; ricochet chain reads as a run of rising pops over the ordinary play sounds. Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $80
ldh [rNR21], a    ; CH2 duty 50%
ld a, $C2
ldh [rNR22], a    ; envelope: initial volume 12, decrease, pace 2 (punchy)
ld a, $A2
ldh [rNR23], a    ; frequency low  — period $7A2 → ~1400 Hz
ld a, $87
ldh [rNR24], a    ; trigger + frequency high
.idle:
jr .idle
