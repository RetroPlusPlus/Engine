; ball_lost — the ball fell past the paddle, or a ghost swallowed it. A low, longer "buzz" on pulse
; CH1, 25% duty (~250 Hz), a slow envelope so it sinks rather than snaps — the only down-beat cue in
; the set. Self-contained; CH1 sweep off.
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
ld a, $94
ldh [rNR12], a    ; envelope: initial volume 9, decrease, pace 4 (slow sink)
ld a, $F4
ldh [rNR13], a    ; frequency low  — period $5F4 → ~250 Hz
ld a, $85
ldh [rNR14], a    ; trigger + frequency high
.idle:
jr .idle
