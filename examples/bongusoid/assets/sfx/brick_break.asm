; brick_break — a brick is destroyed. A brighter, punchier blip than a survive-hit: pulse CH2, 50% duty,
; high (~950 Hz), a strong-but-quick envelope. Self-contained.
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
ld a, $76
ldh [rNR23], a    ; frequency low  — period $776 → ~950 Hz
ld a, $87
ldh [rNR24], a    ; trigger + frequency high
.idle:
jr .idle
