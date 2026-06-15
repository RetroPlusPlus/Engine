; tone_c2 — trigger CH3 at C5 (~523 Hz). Wave channel set up once by wave_init.asm; this only sets the
; frequency and triggers (no wave rewrite, no DAC toggle).
ld a, $83
ldh [rNR33], a    ; frequency low — C5
ld a, $87
ldh [rNR34], a    ; trigger (bit 7) + frequency high
.idle:
jr .idle
