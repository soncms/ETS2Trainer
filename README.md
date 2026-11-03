# ETS2 / ATS Truck Trainer — English fork

English interface, engine multipliers up to **5x**, RPM limit increases up to **50%**, and ATS **1.62.1.1s** support, based on [sakuradairong/ETS2Trainer](https://github.com/sakuradairong/ETS2Trainer).

## Supported builds

- ATS: 1.62.1.1s (PE timestamp `6AE990FB`, image size `039DB000`), plus the upstream 1.61.3.1 profile.
- ETS2: upstream 1.61.1.1 and 1.61.1.9 profiles. ETS2 1.62 has not been verified.
- ATS 1.62 automatic money/XP editing is unavailable until its economy layout is verified. Other save tools retain upstream behavior.
- Exact executable fingerprints and code anchors are checked. Changing the version label does not enable an unknown build.

## Use

1. Install the x64 `scs-telemetry.dll` from [RenCloud/scs-sdk-plugin V.1.12.1](https://github.com/RenCloud/scs-sdk-plugin/releases/tag/V.1.12.1) into the game's `bin/win_x64/plugins/` folder. Restart the game once after installation and accept its SDK notification.
2. Load a single-player profile and enter your truck.
3. Run `ETS2Trainer.exe`; accept the Windows administrator prompt if shown.
4. Select **ATS** or **ETS2**, then **Attach game**.
5. Open **Vehicle tuning**, select **1.25×**, **1.50×**, **2.00×**, **3.00×**, **4.00×**, or **5.00×**, and click **Apply tuning**.
6. **Restore stock** restores the recorded engine values. Leave the trainer running while using the multiplier; the upstream maintenance loop follows the current vehicle after truck changes or save loads.

The multiplier changes actual engine torque, including its two runtime copies. The truck shop's HP text does not have to change. The RPM limit increase slider independently selects **0–50%** above the original engine and neutral limits (0% restores stock RPM). Click Apply tuning after adjusting either control.

TruckersMP engine writes are blocked. Convoy detection and the upstream restoration/transaction protections are retained. Do not run two trainers against the same game.

The English interface and diagnostics are translated; advanced features retain upstream scope. Original Chinese documentation: [README.zh-CN.md](README.zh-CN.md).

## Build on Windows

Install **Visual Studio 2022 Build Tools** with **Desktop development with C++** (MSVC x64/x86 tools and Windows SDK). Dear ImGui is already vendored. Git is needed to clone the repository.

```bat
cd cpp
build.bat
```

Output: `ETS2Trainer.exe` in the repository root. The script currently uses the standard VS 2022 BuildTools installation path; edit `VCVARS` if your installation differs.

Developer checks:

```bat
cd cpp
build.bat test
build\ETS2Trainer_test.exe --gametest
build\ETS2Trainer_test.exe --game=ats --enginetest
```

`--gametest` runs offline regression checks. `--enginetest` reads the running game's vehicle/engine data. `--powertest --game=ats` temporarily applies 1.25× torque and restores it; run it only with a loaded single-player truck. Diagnostic reports stay local and must not be committed.

## Tools for adapting another build

Running this trainer does **not** require Cheat Engine. Building it needs MSVC and Windows SDK. For reverse engineering, Python with `pefile` and `capstone` can inspect the PE image, code anchors and runtime fields. Cheat Engine or x64dbg is optional for interactive memory/disassembly work.

The ATS 1.62 update changes the root instruction RVA and engine vtable, and inserts eight bytes before the truck engine wear/fuel ratio fields. Profiles store this shift separately so older supported builds keep their original layout. Engine object type checks use the profile matched against the running executable, not a default profile. Telemetry independently checks fuel capacity, fuel level and component wear before engine writes.

See [docs/ats-1.62-validation.md](docs/ats-1.62-validation.md) for validation results and limits.

## Extended tuning and recovery

The core validates a maximum multiplier of 5.0 and a finite RPM increase in 0–50%. Target torque remains bounded at 60000 Nm, and existing game-specific absolute RPM/engine-identity limits remain in place. Values outside these ranges are refused. All values are computed from the saved stock baseline, including when changing presets or following another truck.

The legacy boolean API still means +10% for existing tests/callers. Recovery records retain the actual baseline and written RPM values, so existing settings remain compatible and arbitrary RPM increases can be restored after restarting. Importing recovery data never enables tuning automatically.

When updating, close the old trainer normally before starting the new executable. Keep its settings.ini when replacing the executable so any unconfirmed recovery records are preserved.
