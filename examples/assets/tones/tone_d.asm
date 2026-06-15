; tone_d — trigger CH3 at D4 (~294 Hz). Wave channel set up once by wave_init.asm; this only sets the
; frequency and triggers (no wave rewrite, no DAC toggle).
ld a, $21
ldh [rNR33], a    ; frequency low — D4
ld a, $87
ldh [rNR34], a    ; trigger (bit 7) + frequency high
.idle:
jr .idle
