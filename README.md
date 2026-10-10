# WreckBox Home — update channel

Update feed for the WreckBox Home player (moOde on Raspberry Pi 5). Source: the private `wreckbox-home` repo.

- `latest.json` (this branch) names the current release: version, package URL, SHA-256.
- Packages are GitHub releases in this repo tagged `home-vX.Y.Z` (never marked "Latest", so app downloads aren't affected).
- Every package is signed; the player verifies it against its built-in public key before installing.
