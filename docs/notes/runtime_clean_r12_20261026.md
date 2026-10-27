# Clean runtime R12 source/build seal, 2026-10-26

The fresh isolated R12 build is accepted for native QA. It was built from exact public source commit `c68d9641275b40bc2615ed506ef06933ea5841b8`; native execution and clean first-launch validation of this new executable remain pending. Earlier r11 native acceptance is a baseline, not a run of R12.

The external control directory is `G:/MC3_Backups/release-work/corrected-content-0.3.3-dev.20261026-r1/runtime-clean-r12`. Its binding records the shorter work root `G:/MC3_Build_r12`; the successful binary directory is `build-attempt3`. Source and dependency archives were materialized independently, with no reused object, static library or generated game source. The original USA XBE was independently copied and checked against SHA256 `0b3d81a02834e57864a601d473d5296cc0a2463e4b837373d91030f751fa51a3` and SHA1 `5bca1cf092edaadf876e8f80129900407a4a07b8`. Fresh code generation produced 133 files totaling 203,855,111 bytes, all byte-identical to the canonical generated tree inspected read-only.

| Output | Size | SHA256 |
|---|---:|---|
| `G:/MC3_Build_r12/build-attempt3/mc3.exe` | 23,650,816 | `a08f330ce1883fe2639e0ccd753dadebab67db0611bd7dcfea0ca4605d8b9d1a` |
| `G:/MC3_Build_r12/build-attempt3/Check-ContentArchive.exe` | 454,656 | `9f80fb07f78980c7b8272dbcf9c4b5dc51d04c4d27e4ce28c6f3cc7b27814f31` |

The build uses explicit LLVM 23.1.3, CMake 4.4.4, Ninja 1.13.2, VS18 BuildTools/MSVC 14.51.36231 and Windows SDK 10.0.26100.0 paths. All FetchContent inputs were supplied offline as exact Git-commit archives: ImGui `8199457a7d9e453f8d3d9cadc14683fb54a858b5`, toml++ `30172438cee64926dc41fdd9c11fb3ba5b2ba9de`, SDL3 `fa2c02bb6e21974a89ea9824bc53c9932abe5f9c`, Zydis `a2278f1d254e492f6a6b39f6cb5d1f5d515659dc`, and Zycore `0b2432ced0884fd152b471d97ecf0258ff4d859f`. The pinned CRT inventory is for `C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/VC/Redist/MSVC/14.51.36231/x64/Microsoft.VC145.CRT`; packaging must select it explicitly.

All build phases used at most two parallel jobs. Configure, fresh recompiler, fresh game-code generation, game build and checker build completed in 66.422, 12.156, 6.359, 288.968 and 2.984 seconds respectively. Their commands and separate logs are retained. The main seal finished at 23:05:52 UTC; its final status checkpoint completed at approximately 23:05:56 UTC.

The before/after inventories match for 18,888 toolchain files (5,134,479,538 bytes), 3,835 source/dependency files including freshly generated code, 38 selected configured inputs, and all 133 generated files. A complete 52-file configured-build inventory additionally authenticated the generated CMake PCH `.cxx` stub before compilation. The complete final build inventory contains 547 files. The final supplemental audit authenticated all 1,595 unique actual Ninja compiler dependency paths against their before-build byte seals, rechecked output/XBE hashes and found no changed or unsealed dependency. Final review rehashed 63 small control receipts; it reused the already sealed 3.43 GB source-archive evidence rather than repeating that large archive audit.

The first attempt rejected Zydis's committed `subprojects -> dependencies/` alias. Its exact blob was subsequently represented as an inert regular file, matching the existing ordinary Windows checkout; a dedicated proof confirms that no Zydis CMake file consumes that Meson alias. The second attempt failed during compiler identification because raw backslashes in `CMAKE_MT` were escaped incorrectly by generated CMake code. Both failures and their separate partial artifacts remain preserved. Attempt3 used a new empty build directory and forward-slash CMake path arguments. The first supplemental dependency report omitted `.cxx` from its narrow configured-input filter; the corrected new report uses the existing full precompile inventory, with no rebuild or retroactive input pin.

Principal immutable receipts under the control directory:

- `final-seal-attempt3.json`: `5e7b5ac2ddd1a735e3063c63bfac0d3e48e5a477f9e9927497ee7699a7a17a05`.
- `attempt3-receipt-inventory.json`: `f79fdaeaf6f509858cd8d278813ada3f5fe6b72cb755e6c58af57b1af8750503`.
- `dependency-closure-attempt3-r2.json`: `59b86046a7b893510d72047d8e5ce2c2298e402863ede0e3276ce914f3e4d2b0`.
- Preserved failed closure `dependency-closure-attempt3.json`: `0124c04dcbf0fc7185ee84bab97a78273051924eff4fc46ea7513d29780360f6`.

This seal establishes the fresh build's source/input provenance and retained outputs. It does not establish bit-identical reproduction across hosts, new native rendering/input behavior, actual content-archive interoperability, or clean offline first launch. Those release gates still require their own receipts using these exact output hashes. No game was launched, and no canonical runtime source, original asset, personal save/settings, GPU/media lifecycle or runtime PBR state was changed by this build. A later source commit requires a fresh release source mapping before publication.
