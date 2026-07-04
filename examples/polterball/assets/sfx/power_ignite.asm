; power_ignite — the ball ate a power pellet and ignited. The signature cue: a rising sweep on pulse
; CH1 — the hardware frequency sweep climbs the pitch from ~300 Hz upward while the envelope rings it
; out, an unmistakable "power up" whoosh no other cue in the set has. Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $12
ldh [rNR10], a    ; CH1 sweep: period 1, addition (pitch rises), shift 2
ld a, $80
ldh [rNR11], a    ; CH1 duty 50%
ld a, $C4
ldh [rNR12], a    ; envelope: initial volume 12, decrease, pace 4 (rings while it climbs)
ld a, $4B
ldh [rNR13], a    ; frequency low  — period $64B → ~300 Hz start
ld a, $86
ldh [rNR14], a    ; trigger + frequency high
.idle:
jr .idle
