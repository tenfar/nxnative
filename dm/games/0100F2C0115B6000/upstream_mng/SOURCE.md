# Upstream shader list (MacNativeGames)

Source: https://github.com/ly05010419/MacNativeGames (MIT)
Files: `games/totk/assets/shader_list.tsv` (TotK), `games/botw/assets/shader_list.tsv` (BotW).

Imported as identifiers only. Runtime of nxnative uses `dm_ctl_sha16` + `pso_manifest.tsv` with keys like `aaaaaaaaaaaaaaaa_b`.
MacNativeGames warm rows use keys like `<sha256>-<12hex>_b` and translation chain `totk-native-v6-unverified`.

Do not copy S/R/C into `pso_manifest.tsv` until a Mac smoke proves the key and translator chain match.
