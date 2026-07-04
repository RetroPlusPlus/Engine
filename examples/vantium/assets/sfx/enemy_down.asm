; enemy_down — a fighter or mine destroyed. A crunchy noise burst on CH4: mid-rough clock, a
; strong-but-quick envelope — wreckage, not a note. Self-contained.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $FF
ldh [rNR51], a    ; pan all channels L + R
ld a, $77
ldh [rNR50], a    ; master volume L + R
ld a, $00
ldh [rNR41], a    ; CH4 length (unused — envelope ends the sound)
ld a, $B2
ldh [rNR42], a    ; envelope: initial volume 11, decrease, pace 2
ld a, $54
ldh [rNR43], a    ; noise clock: mid shift, gravelly
ld a, $80
ldh [rNR44], a    ; trigger
.idle:
jr .idle
