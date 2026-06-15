; tone — a gentle diagnostic test tone (ENG-4.A).
;
; A soft ~250 Hz TRIANGLE wave on the wave channel (CH3) at moderate (50%) volume. A triangle is
; deliberately chosen over a square: its harmonics roll off steeply, so it carries little high-frequency
; energy (square waves are harsh and ring up into the tinnitus-sensitive band). Low pitch + low harmonics
; + moderate volume make it accessibility-considerate while still being a steady, audibly-correct, non-
; silent waveform — the minimal signal that proves the APU -> sink chain aligns on sample rate.
;
; It is a DRIVER, not a call: it loads a triangle into wave RAM, triggers CH3, then idles in a self-loop
; while the APU sustains the tone (the audio system steps it a cycle budget per tick). NO ROM; assembled
; in-process. Frequency: period $6FA -> 65536 / (2048 - 1786) = 250 Hz.

ld a, $80
ldh [rNR52], a    ; APU master enable (bit 7 on)
ld a, $00
ldh [rNR30], a    ; CH3 DAC OFF before writing wave RAM — rewriting wave RAM while the channel is
                  ; active corrupts it (the re-trigger "tinny / out of key" bug); disable the DAC first

; Load a 32-step triangle (0->15->0) into wave RAM ($FF30-$FF3F); each byte is two 4-bit samples.
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

ld a, $FF
ldh [rNR51], a    ; panning: route all channels to both L and R
ld a, $77
ldh [rNR50], a    ; master volume max on L/R (per-channel level below keeps it moderate)
ld a, $80
ldh [rNR30], a    ; CH3 DAC enable (bit 7)
ld a, $00
ldh [rNR31], a    ; CH3 length (length-disabled below, so this is unused)
ld a, $40
ldh [rNR32], a    ; CH3 output level: 50% (bits 6-5 = 10) — moderate, not full volume
ld a, $FA
ldh [rNR33], a    ; CH3 frequency low byte  (period $6FA)
ld a, $86
ldh [rNR34], a    ; CH3 trigger (bit 7) + length-disabled + frequency high bits ($6) -> ~250 Hz

.idle:
jr .idle          ; spin so the APU keeps sounding while the driver is stepped
