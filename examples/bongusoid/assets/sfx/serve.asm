; serve — the ball is launched. A confident mid-high "pew" on pulse CH2, 50% duty (~750 Hz), a slightly
; longer envelope than a bounce so it reads as an intentional action. Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $80
ldh [rNR21], a    ; CH2 duty 50%
ld a, $B4
ldh [rNR22], a    ; envelope: initial volume 11, decrease, pace 4 (a touch longer)
ld a, $51
ldh [rNR23], a    ; frequency low  — period $751 → ~750 Hz
ld a, $87
ldh [rNR24], a    ; trigger + frequency high
.idle:
jr .idle
