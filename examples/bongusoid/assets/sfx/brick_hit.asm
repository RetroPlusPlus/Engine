; brick_hit — a silver brick takes a hit but survives (or a gold brick bounces). A low, dull thunk on
; pulse CH1, 12.5% duty (~330 Hz) — distinct from a break. Self-contained; CH1 sweep is disabled (NR10=0).
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $00
ldh [rNR10], a    ; CH1 sweep off (no pitch glide)
ld a, $00
ldh [rNR11], a    ; CH1 duty 12.5% (thin, dull)
ld a, $62
ldh [rNR12], a    ; envelope: initial volume 6, decrease, pace 2
ld a, $73
ldh [rNR13], a    ; frequency low  — period $673 → ~330 Hz
ld a, $86
ldh [rNR14], a    ; trigger + frequency high
.idle:
jr .idle
