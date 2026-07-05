; bank — the whole deck delivered at the sanctuary (also voices a crossing cleared). The signature
; reward cue: a long rising sweep on pulse CH1 from ~500 Hz under a ringing envelope — the only
; long rising voice in the set, so a payout is unmistakable. Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $13
ldh [rNR10], a    ; CH1 sweep: pace 1, addition (pitch rises), shift 3
ld a, $40
ldh [rNR11], a    ; CH1 duty 25% (reedy, festival-horn-ish)
ld a, $D5
ldh [rNR12], a    ; envelope: initial volume 13, decrease, pace 5 (rings while it climbs)
ld a, $FA
ldh [rNR13], a    ; frequency low  — period $6FA → ~500 Hz start
ld a, $86
ldh [rNR14], a    ; trigger + frequency high
.idle:
jr .idle
