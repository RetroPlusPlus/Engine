; wall_bounce — ball off a wall or the ceiling. A shorter, lower tick than the paddle bounce: pulse CH2,
; 25% duty (~480 Hz), a fast envelope decay. Self-contained (enables the APU + panning/volume each play).
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $40
ldh [rNR21], a    ; CH2 duty 25% (thinner than the paddle blip)
ld a, $73
ldh [rNR22], a    ; envelope: initial volume 7, decrease, pace 3 (short)
ld a, $EF
ldh [rNR23], a    ; frequency low  — period $6EF → ~480 Hz
ld a, $86
ldh [rNR24], a    ; trigger + frequency high
.idle:
jr .idle
