; soft_break — the ball smashed a breakable wall. A crunchy noise burst on CH4 (the noise channel):
; mid-rough polynomial clock, a strong-but-quick envelope — masonry giving way, not a musical note.
; Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $00
ldh [rNR41], a    ; CH4 length (unused — envelope ends the sound)
ld a, $B2
ldh [rNR42], a    ; envelope: initial volume 11, decrease, pace 2 (punchy crunch)
ld a, $55
ldh [rNR43], a    ; noise clock: mid shift, 15-bit LFSR, mid divisor — a gravelly rumble
ld a, $80
ldh [rNR44], a    ; trigger
.idle:
jr .idle
