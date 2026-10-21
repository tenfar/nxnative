# Merge MacNativeGames shader list into tenfar/nxnative

**Status:** prepared locally; not pushed. Needs Mac smoke before replacing empty `pso_manifest.tsv`.

## What is included
| Path | Content |
|------|---------|
| `dm/games/0100F2C0115B6000/upstream_mng/shader_list.tsv` | TotK MNG list (V/S/C/R identifiers) |
| `dm/games/01007EF00011E000/upstream_mng/shader_list.tsv` | BotW MNG list |
| `dm/games/*/upstream_mng/SOURCE.md` | provenance |
| `dm/games/*/pso_manifest.from_mng.tsv` | **candidate** prewarm from S/C only |

## Why not overwrite `pso_manifest.tsv` yet
1. MNG warm keys look like `<sha256>-<12hex>_b`; nxnative records `aaaaaaaaaaaaaaaa_b`.
2. Converter collapses to `key[:16]_b` only when that prefix exists in your `shader_map.tsv`.
3. MNG `R` pipeline blobs encode their longer names — skipped.
4. Translator chain may differ (`totk-native-v6-unverified` vs your chain). Wrong metallib = wrong/missing draws.

## Recommended apply order (Multica on TFMBP)
1. Copy `upstream_mng/` into the local checkout (safe, data only).
2. Build/run TotK or BotW with existing empty `pso_manifest`.
3. Optionally try `pso_manifest.from_mng.tsv` renamed behind a flag / backup — watch warm hits vs misses.
4. If warm keys miss, re-record with `DM_PSO_RECORD` and merge MNG **V** deps via future seed tooling instead.

## License
Both MIT. Keep SOURCE.md.
