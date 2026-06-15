; wave_init — one-time setup of the wave channel (CH3), the way a real sound driver inits its hardware
; once rather than re-configuring it on every note. Enables the APU, loads the triangle into wave RAM
; (safe to write here because the channel has NEVER been triggered yet), turns the DAC on, and sets
; panning + volume. It leaves the channel ARMED but SILENT — no trigger. The per-note routines then
; only set the frequency and trigger, so a note never rewrites wave RAM (which would corrupt it) and
; never toggles the DAC (which would pop). Played once on each system at startup.
ld a, $80
ldh [rNR52], a    ; APU master enable
ld a, $01
ldh [$FF30], a
ld a, $23
ldh [$FF31], a
ld a, $45
ldh [$FF32], a
ld a, $67
ldh [$FF33], a
ld a, $89
ldh [$FF34], a
ld a, $AB
ldh [$FF35], a
ld a, $CD
ldh [$FF36], a
ld a, $EF
ldh [$FF37], a
ld a, $FE
ldh [$FF38], a
ld a, $DC
ldh [$FF39], a
ld a, $BA
ldh [$FF3A], a
ld a, $98
ldh [$FF3B], a
ld a, $76
ldh [$FF3C], a
ld a, $54
ldh [$FF3D], a
ld a, $32
ldh [$FF3E], a
ld a, $10
ldh [$FF3F], a
ld a, $80
ldh [rNR30], a    ; CH3 DAC on (channel still untriggered, so it stays silent)
ld a, $FF
ldh [rNR51], a    ; panning: all channels to L and R
ld a, $77
ldh [rNR50], a    ; master volume max on L/R
ld a, $00
ldh [rNR31], a    ; length (length-disabled at trigger, so unused)
ld a, $40
ldh [rNR32], a    ; CH3 output level 50% — moderate, accessibility-considerate
.idle:
jr .idle
