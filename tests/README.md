# Component checks

Use an x64 Visual Studio developer shell with LLVM/Clang, CMake and Ninja on PATH:

```powershell
cmake -S tests -B out/tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build out/tests --parallel 8
ctest --test-dir out/tests --output-on-failure
```

Six core tests cover voice properties, Winsock negotiation, fixed-function shader stages, keyboard/script mapping and the actual title timing/tuning hooks. Hook tests provide guest-memory/entry-point doubles rather than executing the whole game. They include 60-to-30 switching and registration-before-parsing regressions.

On Windows, optionally pass `-DMC3_HD_TEST_ROOT="<absolute path to templates/game/out/native>"` at configuration. When completed fixtures exist, this adds actual WIC texture ownership/mip tests (`textures/replace/*.png`) and Media Foundation movie decode/loop/selection/address-reuse tests (`video_hd/rockstar.mp4`). No game is started. These fixtures are local game-derived outputs and must not be committed.

The optional movie check expects the completed 1920x1440 Rockstar-logo upscale. It is a component integration check, not proof of every movie, orientation or final rendered composition.

Disc format/corruption/import-safety checks are independent:

```powershell
python -m unittest discover -s tools/disc -p 'test_*.py' -v
```
