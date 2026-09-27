# Steam integration

- `steam_keyboard`: Steam gamepad keyboard adapter used by the LR text editor.
- `steam_cloud_safety`: validates `sgw.dll` and installs guarded Steam hooks.
- `steam_achievement_policy`: side-effect-free achievement retry decisions.
- `steam_achievement_queue`: durable pending achievement storage.
- `steam_pending_journals`: durable retry lists for achievements and cloud work.
- `steam_remote_storage_interface`: 32-bit Remote Storage ABI used by `sgw.dll`.
- `steam_remote_storage_proxy`: local-first game-facing storage implementation.
- `steam_remote_storage_worker`: background reconciliation, uploads, and deletions.
- `steam_remote_storage_policy`: remote catalog and mutation policy.
- `steam_remote_storage_catalog`: local save catalog with pending deletions hidden.
- `steam_save_files`: validated paths, bounded I/O, and atomic replacement.
- `steam_save_mirror`: serialized local mutations, checkpoints, and conflict imports.
- `steam_save_checkpoints`: rotating recovery copies.
- `steam_save_identity`: size and SHA-256 save identity.
- `steam_save_sync_state`: last synchronized save identities.
- `steam_save_reconciliation_policy`: local/remote convergence decisions.
- `steam_shutdown_guard`: drains cloud work before Steam callback teardown.
