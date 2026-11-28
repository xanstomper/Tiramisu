# Shared engine improvements

- Archive requests validate range offset, file size, and byte count before publishing cached data. Sequential reads hold the file cursor through the transfer. Positional reads retain independent offsets.
- Steam imports account-owned AppIDs separately from installed folders. Signed-out and sideloaded entries stay visible in the library and supply no account entitlements. Play metadata exposes the exact associated folder basename for validation.
