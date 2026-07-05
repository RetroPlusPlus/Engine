; death — the ferry destroyed. The one long down-beat: deep noise with a slow envelope so it
; rumbles out over the respawn. Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $00
ldh [rNR41], a    ; length unused (the envelope ends the sound)
ld a, $F7
ldh [rNR42], a    ; envelope: initial volume 15, decrease, pace 7 (the long rumble)
ld a, $67
ldh [rNR43], a    ; noise: clock shift 6, 15-bit LFSR, divisor 7 — deep and slow
ld a, $80
ldh [rNR44], a    ; trigger
.idle:
jr .idle
