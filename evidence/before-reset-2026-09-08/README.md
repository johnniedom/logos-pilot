# Evidence recorded before the public testnet was reset

The operator of the public LEZ testnet (`https://testnet.lez.logos.co`) reset the chain on
2026-09-08. Every transaction and account in these two tables was real and was verified against
that chain on the day it was recorded (the `how_verified` column says how); none of it resolves on
the chain that runs now, and `evidence/verify-testnet.sh` no longer reads these files.

They are kept for the record of what was done between 2026-08-29 and 2026-09-07. The live tables in
`evidence/` hold the same steps re-landed on the reset chain from 2026-09-09 on.
