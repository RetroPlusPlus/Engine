; touchdown — wheels on the strip. A settling two-step feel from one note: mid pulse CH1
; (~500 Hz) with a gentle downward sweep and a moderate decay — arrival, not victory (the
; destruct rumble follows it). Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $1A
ldh [rNR10], a    ; CH1 sweep: period 1, subtract (pitch settles downward), shift 2
ld a, $80
ldh [rNR11], a    ; CH1 duty 50%
ld a, $B3
ldh [rNR12], a    ; envelope: initial volume 11, decrease, pace 3
ld a, $F8
ldh [rNR13], a    ; frequency low  — period $6F8 → ~500 Hz
ld a, $86
ldh [rNR14], a    ; trigger + frequency high
.idle:
jr .idle
