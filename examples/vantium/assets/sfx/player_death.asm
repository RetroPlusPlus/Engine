; player_death — the Manta lost. The one hard down-beat: a low buzz on pulse CH1, 25% duty
; (~220 Hz), slow sinking envelope. Self-contained; CH1 sweep off.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $00
ldh [rNR10], a    ; CH1 sweep off
ld a, $40
ldh [rNR11], a    ; CH1 duty 25%
ld a, $95
ldh [rNR12], a    ; envelope: initial volume 9, decrease, pace 5 (slow sink)
ld a, $AB
ldh [rNR13], a    ; frequency low  — period $5AB → ~220 Hz
ld a, $85
ldh [rNR14], a    ; trigger + frequency high
.idle:
jr .idle
