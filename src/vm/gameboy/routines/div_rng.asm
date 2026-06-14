; divRng — the common Game Boy entropy source: the free-running DIV register IS the random byte.
; No inputs; returns the rDIV byte in A. Its rDIV-dependence is exactly what makes it a VM case.
ldh a, [rDIV]
ret
