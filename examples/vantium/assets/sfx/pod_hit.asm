; pod_hit — a fuel pod popped. A short bright blip on pulse CH2, 50% duty (~900 Hz), snappy
; decay — a reward tick above the fire sound, below the kill crunch. Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $80
ldh [rNR21], a    ; CH2 duty 50%
ld a, $A2
ldh [rNR22], a    ; envelope: initial volume 10, decrease, pace 2
ld a, $6E
ldh [rNR23], a    ; frequency low  — period $76E → ~900 Hz
ld a, $87
ldh [rNR24], a    ; trigger + frequency high
.idle:
jr .idle
