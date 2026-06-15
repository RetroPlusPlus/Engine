; tone_e — trigger CH3 at E4 (~330 Hz). Wave channel set up once by wave_init.asm; this only sets the
; frequency and triggers (no wave rewrite, no DAC toggle).
ld a, $39
ldh [rNR33], a    ; frequency low — E4
ld a, $87
ldh [rNR34], a    ; trigger (bit 7) + frequency high
.idle:
jr .idle
