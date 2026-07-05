; foil — the ferry body-blocked an abduction and the colonist tumbled free. A springy thunk: CH1
; sweeping UP fast from a low ~150 Hz (the boing), medium ring. The save deserves a comic voice.
; Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $12
ldh [rNR10], a    ; CH1 sweep: pace 1, addition (pitch springs upward), shift 2
ld a, $80
ldh [rNR11], a    ; CH1 duty 50%
ld a, $B3
ldh [rNR12], a    ; envelope: initial volume 11, decrease, pace 3
ld a, $96
ldh [rNR13], a    ; frequency low  — period $496 → ~150 Hz start
ld a, $84
ldh [rNR14], a    ; trigger + frequency high
.idle:
jr .idle
