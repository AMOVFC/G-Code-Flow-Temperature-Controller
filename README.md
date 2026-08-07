![image](https://github.com/user-attachments/assets/b5b84ecc-84e5-4583-97c8-efdcdf985504)  
# G-Code Flow & Temperature Controller

> **This repository contains two implementations.**
>
> - **`sb53` (C++)** — an in-progress clean-room rewrite. Command-line only today, no GUI
>   yet. Build and usage instructions are immediately below.
> - **The original Delphi application (V1.1)** — the released, GUI version. Its
>   documentation begins at [Original Delphi Version](#original-delphi-version-v11).
>
> The rewrite reproduces the original's temperature curve to within ~1 °C
> (correlation +0.9947 on a matched benchy). It has **not yet been validated by test
> prints** — see [Status](#status).

---

# The C++ rewrite (`sb53`)

## Status

| | |
|---|---|
| G-code analysis, temperature planning, rewriting | ✅ working |
| Command-line tool | ✅ working |
| Validated against the original tool | ✅ +0.9947 curve correlation |
| **Validated by an actual print** | ❌ **not yet** |
| Reads your existing `Config.sdb` profiles | ❌ not yet — calibration is entered by hand |
| Graphical interface | ✅ local web UI (`sb53 serve`) |

> ⚠️ **Do not run output from this on a printer unattended.** It has never been physically
> validated. Compare against the original tool's output first, and watch the first print.

Detailed progress lives in [docs/STATE.md](docs/STATE.md). What the tool actually does,
and why, is in [docs/ALGORITHM.md](docs/ALGORITHM.md).

## Getting the executable

There is **no prebuilt download yet** — you build it. This takes about two minutes.

### Prerequisites

**Visual Studio 2022** (Community is fine) with two workloads/components:

- *Desktop development with C++*
- *C++ CMake tools for Windows*

That is everything. CMake, Ninja and the compiler all ship inside Visual Studio — you do
**not** need a separate CMake install, and you do **not** need a "Developer Command
Prompt". The build script finds them via `vswhere`.

The **first** build downloads the Catch2 test framework from GitHub, so it needs network
access once. Later builds work offline.

### Build

```powershell
git clone <this-repo>
cd G-Code-Flow-Temperature-Controller
./tools/build.ps1
```

The executable lands at **`build/bin/sb53.exe`**.

Options: `-Clean` wipes the build directory, `-Config Debug` builds unoptimised,
`-NoTests` skips the test run. On failure the script prints the compiler errors and
writes a full log to `build/build.log`.

### The motion estimator

`sb53` does not compute kinematics itself. It calls **`klipper_estimator.exe`**, which is
already in this repository at `bin/klipper_estimator.exe`.

> This is a **custom fork**, not upstream Annex-Engineering. Do not substitute the
> upstream build; the output format is not guaranteed to match.

Beside the estimator there must be a **`config.json`** describing your printer's motion
limits. `sb53` looks for it next to whatever estimator you point it at.

## Configuring your printer

`config.json` tells the estimator how fast your machine can actually move. **Getting this
wrong silently corrupts everything downstream** — move timing drives the flow curve, which
drives the temperature plan.

### Option 1 — pull it from Klipper (best)

```powershell
./bin/klipper_estimator.exe --config_moonraker_url http://YOUR_PRINTER_IP dump-config > myconfig.json
```

### Option 2 — write it by hand from `printer.cfg`

Map your Klipper settings across:

| `config.json` | from `printer.cfg` |
|---|---|
| `max_velocity` | `[printer] max_velocity` |
| `max_acceleration` | `[printer] max_accel` |
| `square_corner_velocity` | `[printer] square_corner_velocity` |
| `axis_limiter` → `max_velocity` / `max_accel` | `[printer] max_z_velocity` / `max_z_accel` |
| `extruder_limiter` → `max_velocity` / `max_accel` | `[extruder] max_extrude_only_velocity` / `max_extrude_only_accel` |

A worked example for a CoreXY Voron-derived machine is in
[`testdata/printer-configs/awd-v0.json`](testdata/printer-configs/awd-v0.json).

> **Sanity check:** run `sb53 analyze` and compare its estimated time against your
> slicer's. They should be within roughly 10%. If the tool reports far longer, your
> `config.json` understates the machine's real limits.

## Configuring filament profiles

The rewrite does **not** read `Config.sdb` yet — calibration is passed as command-line
flags. If you already have profiles in the original tool, read them out with:

```powershell
python tools/dump-profiles.py "path/to/Config/Config.sdb"
```

(Read-only; it cannot modify your database.)

### What the numbers mean

You supply **three (flow, temperature) points** and the tool interpolates linearly
between them.

| Flag | Meaning |
|---|---|
| `--low` / `--low-temp` | flow (mm³/s) and temperature (°C) at the low end |
| `--mid` / `--mid-temp` | the middle calibration point |
| `--high` / `--high-temp` | the high end |
| `--rise` | how fast the hotend heats, °C per second |
| `--fall` | how fast it **cools**, °C per second — usually much slower |
| `--smoothing` | averaging window in seconds; 10–30 recommended |
| `--bias` | **0 = quality, 10 = most aggressive** (see below) |
| `--cool-below` / `--cool-drop` | fast-layer cooling (see below); `--cool-drop 0` disables |
| `--adjust-pa` | enable pressure-advance adjustment (Klipper only) |
| `--start-macro` / `--temp-token` | start-macro name and the parameter to rewrite (defaults `PRINT_START` / `EXTRUDER_TEMP`) |

Flow points must be **strictly increasing** (`low < mid < high`) — the tool refuses
otherwise, because the inverse mapping divides by the gaps between them.

### The bias control

`--bias 0` tracks each second's **average** flow: smoother temperature, fewer swings,
gentler on sensitive filament. Higher values track the **peak** flow instead: hotter, more
sustained flow, shorter prints. `--bias 10` is fully aggressive.

> ⚠️ **This is the opposite of the original tool's stored value.** The original's
> `SPEED_QUALITY_OPT` runs 0 = aggressive, 10 = quality. If you are copying a number out
> of `Config.sdb`, **subtract it from 10**. A stored `3` corresponds to `--bias 7`.
>
> Using the value directly would give you prints roughly 14 °C colder than you are used
> to, with no warning. Verified empirically — see
> [ADR-0006](docs/adr/0006-explainable-blend-and-smoothing.md).

Calibrating the three points is unchanged from the original tool; the visual method is
described under [Ideal Flow/Temperature Calibration](#ideal-flowtemperature-calibration)
below.

### Fast-layer cooling

**New in the rewrite — the original tool has no equivalent.**

On small or fast layers the previous layer has not set before the next lands on top of
it, so it sags and loses definition — the classic drooping chimney on a Benchy. Slicers
handle this by slowing down; this lowers the nozzle temperature instead, which stiffens
the extrudate sooner without costing as much time.

- `--cool-below <seconds>` — layers at or above this get no reduction.
- `--cool-drop <°C>` — the reduction at zero layer time, ramping linearly up to the
  threshold.

Off by default, because it changes printed output. The reduction is always clamped to
your calibrated low temperature — cooling never takes the nozzle somewhere you have not
validated.

> ⚠️ **Pick the threshold from your actual layer times, not by feel.** A Benchy at 8
> minutes over 192 layers averages ~2.5 s per layer, so `--cool-below 15` cools
> essentially the whole print and degenerates into a flat temperature offset. Run
> `sb53 serve` and look at the layer-timing chart, or check `median layer time` in the
> web UI's statistics. For small models 3–5 s is usually the useful range.

## Using it

### The web interface (easiest)

```powershell
./build/bin/sb53.exe serve
```

Then open **http://127.0.0.1:8765** in your browser.

Everything is on one page: paste in a G-code path, set your calibration, press **Analyse**
to see the flow curve, the resulting temperature curve, and a per-layer timing chart —
then **Process & write** when it looks right.

The layer-timing chart is the point of the page. It shows how long every layer takes and
highlights which ones fall under your fast-layer cooling threshold, so you can pick that
number from evidence instead of guessing.

> The server binds to **loopback only** and is not reachable from your network. It is
> still a local process that reads and writes files anywhere you can, so do not expose the
> port.

`--port 8765` changes the port. Ctrl+C stops it.

### Inspect a file — changes nothing

```powershell
./build/bin/sb53.exe scan myprint.gcode
```

Reports extrusion mode, print-body bounds, detected printer/filament profiles, and
whether the file has already been processed.

### Analyse flow and preview the temperature plan — changes nothing

```powershell
./build/bin/sb53.exe analyze myprint.gcode --estimator bin/klipper_estimator.exe `
    --low 1 --mid 80 --high 105 --low-temp 220 --mid-temp 280 --high-temp 310 `
    --smoothing 20 --bias 7 --rise 5 --fall 1
```

Prints move count, estimated time, filament used, peak/mean flow, and an ASCII plot of
flow against planned temperature. **Use this to check your settings before processing
anything.**

### Process a file

```powershell
./build/bin/sb53.exe process myprint.gcode --out processed.gcode `
    --estimator bin/klipper_estimator.exe `
    --low 1 --mid 80 --high 105 --low-temp 220 --mid-temp 280 --high-temp 310 `
    --smoothing 20 --bias 7 --rise 5 --fall 1
```

Omit `--out` to overwrite in place — which is what a slicer post-processing hook expects.

Output is written to a scratch file and moved into place only on success, so a failed run
can never leave a half-written file where your input was.

### Compare two processed files

```powershell
./build/bin/sb53.exe compare old.gcode new.gcode
```

Extracts the commanded temperature sequence from each, indexes both by cumulative
extruded filament, and reports correlation and differences. Use it to check the rewrite
against the original tool, or to see what changing a setting actually did.

### Exit codes

`0` success · `1` error (details printed to stderr) · `64` bad usage

Errors carry a stable machine-readable code alongside the message, e.g.
`error: [absolute-extrusion-unsupported] ...`, so scripts can branch on the cause.

## Slicer integration

In OrcaSlicer, *Print Settings → Others → Post-processing Scripts*:

```
"C:\path\to\build\bin\sb53.exe" process --estimator "C:\path\to\bin\klipper_estimator.exe" --low 1 --mid 80 --high 105 --low-temp 220 --mid-temp 280 --high-temp 310 --smoothing 20 --bias 7 --rise 5 --fall 1
```

The slicer appends the G-code path, which `sb53` overwrites in place.

**Requirements**, all enforced with a clear error rather than silently producing bad
output:

- **Relative extrusion (`M83`)** must be enabled. Absolute (`M82`) is rejected — flow
  tracking is meaningless without it.
- The file must not already be processed. Re-running would compound the adjustments.
- Your end G-code needs `; PRINT_END` as its first line if the slicer does not emit
  `; EXECUTABLE_BLOCK_END`.

## Testing

```powershell
./tools/build.ps1              # builds and runs the whole suite
```

48 tests covering parsing, flow aggregation, temperature planning, the rewriter, and the
comparison tool. They are hermetic — **no test spawns the estimator or touches your
printer**; the estimator's output is replayed from a recorded fixture.

Test fixtures live in `testdata/fixtures/` (committed, ~90 KB each). Full-size reference
files are git-ignored but pinned by SHA-256 in `testdata/manifest.json`; tests needing
them skip cleanly when absent. See [testdata/README.md](testdata/README.md).

### Verifying against the original tool

The safest way to gain confidence before printing:

1. Slice a model and keep the raw G-code.
2. Process one copy with the original `SB53-Systems.exe`, another with `sb53 process`,
   using the same calibration (remembering to invert the bias).
3. `sb53 compare old.gcode new.gcode` — expect correlation above 0.95.
4. Print both and compare the results.

> When capturing output from the original tool, **do not use its Save button** — it
> deletes its own intermediate files.

## Known limitations

- Profiles are not read from `Config.sdb` yet — calibration is entered by hand.
- The web UI takes a file **path**; there is no file picker, because a browser cannot
  hand a server a local path.
- Windows only. The core library is portable and builds on Linux, but process execution is
  not implemented there yet.
- Multi-tool and multi-material printing are not supported.
- Arc moves (`G2`/`G3`) work but are slow — the estimator subdivides them.

---

# Original Delphi Version (V1.1)

> Everything below documents the original released application. It remains the version to
> use for real printing until the rewrite has been physically validated.

> **This script dynamically adjusts nozzle temperature and print speed (flow rate) to significantly improve print quality and reduce print time, all while simplifying slicer settings. By optimizing these parameters, it makes 3D printing more accessible, less complex and faster.  **

Most slicers use a fixed nozzle temperature throughout an entire print, even though the extrusion flow constantly changes. This means the filament is often printed either hotter or colder than necessary.

**G-Code Flow & Temperature Controller** is an open-source post-processing tool that analyzes the average volumetric flow of the G-code and dynamically adjusts both nozzle temperature and printing speed.

The goal is simple: maintain the optimal melting conditions throughout the print while reducing manual tuning.

---

# Why?

The amount of heat required to properly melt filament depends primarily on the extrusion flow.

- Low flow requires less heat.
- High flow requires more heat.

However, traditional slicers use a single temperature for the entire print, forcing users to compromise between:

- Surface quality
- Printing speed
- Layer adhesion
- Stringing
- Over/less heating

---
  
# Philosophy

Traditional slicing assumes that extrusion must adapt to a fixed temperature, and the temperature must adapt to a few test simples.  

This project follows the opposite philosophy:

> **Temperature should adapt to the real extrusion flow.**

By dynamically linking nozzle temperature with volumetric flow, the printer can maintain more consistent melting conditions throughout the entire print.

---

# what this script does

Depending on your speeds, accelerations, Jerk/SCV, print geometry,... this script calculates the average flow rate that can be reached every second, and dynamically adjusts the nozzle temperature accordingly. It also modifies print speed in the G-code to match the ideal flow rate, considering layer height and line width.  
   
---

# Features

- Compatible with all firmwares 
- Designed for OrcaSlicer
- No firmware modification required
- Fully automatic after first setup
- Open Source

---

# Benefits

- Better surface quality
- Better layer adhesion
- Improved dimensional consistency
- Reduced overheating on slow sections
- Higher printing speed when possible
- Less manual tuning
- Better use of the printer/filament's capabilities

---

# Example

Instead of printing the entire model at **220°C**, the controller may automatically apply:

| Volumetric Flow | Temperature |
|----------------:|------------:|
| 1 mm³/s | 190°C |
| 15 mm³/s | 220°C |
| 22 mm³/s | 235°C |

Printing speed is then adjusted accordingly to maintain optimal extrusion conditions.

---

# Future Development

Planned improvements include:

- Support for additional slicers
- Intelligent Fan control
- Automatic filament calibration
- Material-specific profiles
- Advanced flow calibration 
- Community contributions

---  
  
Your feedback and ideas are welcome. Let’s innovate and push the boundaries of 3D printing together:  
*"Alone we go faster; together we go further."*  
  
:warning: Please use this script responsibly and with caution, applying reasonable values and closely monitoring your printer's behavior.  
  
![IMG20240927152206](https://github.com/user-attachments/assets/5de13b9c-2930-4809-b027-79e65188029b)  
  
### `Happy Smart 3D Printing :)`  
`See my OrcaSlicer` [Settings](https://github.com/sb53systems/G-Code-Flow-Temperature-Controller/blob/main/My_Config.md) :gear:  
`See the Print Samples` [Discussion](https://github.com/sb53systems/G-Code-Flow-Temperature-Controller/discussions/8), Share yours :bulb:  
### If you find my work worthy, Buy me a [![image](https://github.com/sb53systems/G-Code-Flow-Temperature-Controller/assets/33290411/a504ac44-082d-40f1-a9d0-4abc3da242d8)](https://ko-fi.com/sb53system) and give this project a :star:. Thank you :rose:  
  
# License

This project is licensed under the MIT License.  
  
# About this Repository
### By Salim BELAYEL.  
Discord: sb53systems  
Email : sb53systems@gmail.com  
  
This project began in June 2024.  
Compiled with `Delphi 12 Community Edition`.  
  
![SB53-Systems~1](https://github.com/sb53systems/G-Code-Flow-Temperature-Controller/assets/33290411/b94703a1-cf21-4109-bfa6-b9bcff438a1d)  
[![Made in Algeria](https://www.madeinalgeria.dev/badge/g-code-flow-temperature-controller.svg)](https://www.madeinalgeria.dev/projects/g-code-flow-temperature-controller)  
  
# Latest Release (Download Link)
### [SB53 G-Code Flow/Temperature Controller V1.1](https://github.com/sb53systems/G-Code-Flow-Temperature-Controller/releases/tag/V1.1)  
![image](https://github.com/user-attachments/assets/2c3bd5d9-b310-4103-b303-4ee84b73a78f)  
(Updated in May 12 2025)  
  
### Note
You can retain your old script configuration after an update (or backup/share) by using/save your old "/Config" folder from the previous version. Simply replace the files "SB53-Systems.exe" and "Klipper_estimator.exe.". Note that the final version 1.1 include a different Klipper_Estimator script.  
  
  
# Video that speak about the project on Youtube
### By `PRINTING PERSPECTIVE` 
- Variable Temperature 3D Printing – The FUTURE of 3D Printing?   
  [![Sans titre](https://github.com/user-attachments/assets/b4fc2c73-4a1b-4467-8a5d-7326e216152d)](https://www.youtube.com/watch?v=P6Y8uUPd3yg)  
  
### By `Roetz 4.0`
- Every Speed-3DPrinter will use this method in 2026! (Minuteman Episode 16)  
  [![image](https://github.com/user-attachments/assets/92f9bae8-7e22-408d-b8e6-8ba178a68ab8)](https://www.youtube.com/watch?v=EaORGjZbS-c)
  
# Reddit post  
### By Akyariss for FDMminiatures
- Printing FDM Miniatures with a post-processing script  
  [<img width="277" height="195" alt="image" src="https://github.com/user-attachments/assets/7fe58823-286d-40b3-81eb-d0c177ffb0d5" />](https://www.reddit.com/r/FDMminiatures/comments/1my1r2h/printing_fdm_miniatures_with_a_postprocessing/)  
  
# Similar Github projects
I invite you to test similar Github projects that could offer other features like:  
- Compatibility with multiple operating systems.
- Compatibility with multiple Slicers.
- Other or more Advanced approach.
- Regular updates with new features.
- Fast processing with large files.
- ...etc.
  
Feel free to support and assist the contributors, either financially or through your feedback.  
### - MZ Flow Temp Processor `Python`.  
[See the Project on Github](https://github.com/Yury-MonZon/MZ_Flow_Temp_processor)  
![graphs1](https://github.com/user-attachments/assets/2cbcd0f4-5488-4f96-a061-76cef2274b81)  
  
# Instructions and Prerequisites  
1. The script can be used as a standard program by running the `SB53-Systems.exe` file and manually opening a G-Code file, or by integrating it into the slicer as a post-processing script.  
    ```
    D:\SB53_G-Code_Flow_Temperature_Controller_V1.1\SB53-Systems.exe;
    ```  
    ![image](https://github.com/user-attachments/assets/722d57b1-6568-4317-ba3f-5873c66e221c)  
  
2. Included a [Custom](https://github.com/sb53systems/klipper_estimator) version of [Klipper_Estimator V 3.7.3](https://github.com/Annex-Engineering/klipper_estimator). An accurate tool that uses `Klipper Look-Ahead kinematics` to estimate the time and average flow rate for each move in the G-Code. (+/- 1s total time for Klipper Firmware). The mechanism is very close to other Firmwares (+/- 5% if used correctly).  
   Note that the file `\Klipper_estimator.exe` (already included) is required and must be in the same Folder with this Script.  
   ![image](https://github.com/user-attachments/assets/30449359-fabd-4b3d-9593-523db606c0c1)  
  
3. Editing the `First Layer Temperature` is important, the script can :
    - Edit the specified `"Start Print"` Macro (Klipper).  
    ![image](https://github.com/user-attachments/assets/26b1e09e-0750-43f6-995f-8671da5838e0)  
    ![image](https://github.com/user-attachments/assets/a3c814af-4522-4177-907c-7aab631505f7)
      
    - Edit the `"M109 S"` G-Code command, provided that:
      - It is preceded by the comment "; Temp_To_Edit"  
      ```
       ; Temp_To_Edit
      ```  
      ![image](https://github.com/user-attachments/assets/a51b55dc-831b-48e2-88e8-18b2c99c3222)  
    
      - And add the comment "; PRINT_END" at the start of the `Machine End G-Code`, this will allow the script to avoid the print end Moves.  
      ```
       ; PRINT_END
      ```  
      ![image](https://github.com/user-attachments/assets/05d7ba2e-c3fc-43bc-971b-691dd6e5ff86)  
  
4. The `Initial Temperature` estimated by the Script depends on the speed of the first moves of the G-Code, you can adjust it by fixing the speed of the `Purge Line` or the speed of the `First layer Perimeters`.  
  
5. To have a best `Speed/Quality Optemization`, the Slicer Profil must be set for Max Moves and Max Volumetric Speed. The Nozzle temperature is not important because it will be reset in the script, and the speed will be reduced (not increased) to the Recommended Flow.   
    Example below with my max 200mm/s Printer speed : (Same profil for `PLA, PETG and ABS`)  
    
    ![image](https://github.com/user-attachments/assets/c0a30aed-046a-48ad-b819-93def3b28de5)  
  
    The speed of `Overhangs` and small `Internal/External Bridges` should be set to the maximum speed, this will ensure that the filament is extruded at the recommended flow rate and that it is not too hot and falls off, or too cold and shrinks, also avoiding sudden flow changes and unnecessary temperature drops caused by the average flow calculated in the script.  
    ![image](https://github.com/user-attachments/assets/050be022-7cef-47ff-b1aa-15f8b5134dce)  
    [See my overhangs test examples.](https://github.com/sb53systems/G-Code-Flow-Temperature-Controller/blob/main/Overhangs_Test.md)  
  
    For larger bridges, I use `Thick Bridges` in Orca Slicer and a single 5015 radial fan for part cooling, there is not much deference between a 60mm bridge at 30mm/s and another at 100mm/s, and since I don't print a lot of wide bridges, I prefer to keep an automatic speed.  
    [See my Bridge test examples.](https://github.com/sb53systems/G-Code-Flow-Temperature-Controller/blob/main/Bridges_Test.md)  
  
    You can use a modifier (or more) in the slicer that changes the speed of a few lower layers to the bridge, the temperature and speed should gradually decrease to the desired bridge speed. Example bellow for 50mm/s External Bridge speed. (This approach can only be optimized when it is integrated into the Slicer)  
    ![image](https://github.com/user-attachments/assets/51f2cba4-d57d-4ea7-8ef7-d0c36dd61dc0)   
    ![image](https://github.com/user-attachments/assets/1cee9879-389b-4117-9048-b96c76e51891)  
  
6. You have to set your filament settings:
    - The maximum recommended volumetric speed at the maximum temperature that your Hotend or Filament can handle.
    - The Fan Cooling perdiode and the Min print speed, according to the Filament and your cooling configuration.
  
    You can reduce the speed of the `Cooling Fan` (Except for Bridges and Ovehangs), the `Min Print Speed`, and the `Min Layer Time`.  
    ![image](https://github.com/user-attachments/assets/5dc1f64d-48dc-4d39-8290-ad8251267990)  
    ![image](https://github.com/user-attachments/assets/c07c5e7c-b137-4af3-86b6-efeaecdc06cc)  
  
### Note that :  
  - The script is specifically programmed to reduce speeds only when they exceed the recommended flow rate, while lower speeds will remain as set in the G-Code (Slicer speed).
  - The script does not display the temperature curve in the generated G-Code Chart if Arcs moves are used. This is due to the integration and interpretation of the Klipper_Estimator output.
  - PA can be adjusted based on temperature only for Klipper firmware. For other firmwares you need to uncheck the Adjust PA option.  
  - Changing PA while printing can cause bubbles in the walls. The script is programmed to adjust PA only in sparse infill, internal solid infill, support, and internal bridges.  
  ![351913375-991fe2b8-3935-46ff-816e-5b0aee981b4d](https://github.com/user-attachments/assets/602b96a8-2666-44bd-b70f-aa5c06deadd4)  
  
  - This script doesn't support `Multi-Tool` or `Multi-Material` printing.
  - For a `Bambu Lab` 3D printer, you need to Avoid `Flow Calibration` and `First Layer Inspection` used at the beginning of the print.  
  - `Ironing` is not recommended with this script, as it can affect the desired results and increase printing time.  
  - `Adaptive Pressure Advance` is not recommended with this script.  
  - `Delta Printers` kinematic limits are not supported with the current version of Klipper Estimator.  
  - Reading or generating large G-Code files with this Script can takes up to 2 minutes, depending in your `CPU`.
  - Processing G-Code with `Arcs Moves` will take longer, because the Klipper Estimator script will cut them into small segments based on the parameter `"mm_per_arc_segment": 0.1`.
  - The generated G-Code is 10% to 30% larger than the original one due to Temp and Speed adjustment.
  - This script is currently only available for `Windows OS`. With `Delphi 12` and some changes to the source code, it can be compiled for other operating systems (I can help with this or do it later!).  
  
# Ideal Flow/Temperature Calibration
The visual calibration method (effective for PETG, PLA, etc.) involves selecting the desired appearance (closest to the original filament) over 3 to 5 prints:  

  1. For high flow rates (>3mm³/s), print a `Cylinder` in `Vase Mode` (You can use the maximum layer height and maximum line width), while limiting the maximum volumetric speed to the desired test flow rate. Start with the maximum recommended temperature, then manually and gradually reduce the temperature during printing. (This step should be done without the script)  
  2. For flow rates below 5 or 3 mm³/s, print small object like a 3DBenchy at 20% or 30% scale. (This step should be done with the script)  
  
### Note: 
- This method is not suitable for non-shiny filaments or those that do not change color. To determine the ideal values, other advanced solutions will be necessary, particularly for assessing layer adhesion, dimensional accuracy, or the final temperature of the extruded filament.
- This script allows to play with only three `Flow/Temperature` values, which means it will adjust them linearly.  
  
![image](https://github.com/user-attachments/assets/9a36e823-f3c5-4fe1-9453-c590bf6e5435)  
  
The ideal would be to obtain the final temperature of the extruded filament using a more accurate and automated solution that gives a curve closer to reality!  
This will also be useful with Filaments that cannot be visually calibrated.  
  
I challenge makers to find a precise, cost-effective, and user-friendly solution for the majority of 3D printers :rocket:  
  
# Observations and Tips
  - I recommend that you calibrate your PID values ​​for a temperature between 70% and 90% of the maximum temperature.  
  - A printer with higher accelerations and lower hotend heating/cooling time, will have a better result with this approach because it allows for better flow stabilization (Quality) and Higher Max/Average Flow (Speed).  
  - With a resonable Edeal Flow/Temperature calibration, the same good quality is achieved with the majority of filament brands without any changes in the script.    
  - With some prints, changing the `Max/Average Smoothing value` may affect the result and print time, you have to experiment yourself (I recommend values between 10 and 30).  
  - Aim for `Speed Optimization` as long as it doesn't affect the desired quality, usually the printing time will only vary by a few minutes.  
  - If your filament is very sensitive and you need to reduce the speed for overhangs or small features in between large features, aim for `Quality Optimization` to reduce flow variation.  
  - `Fuzzy Skin`, `Variable_Layer_Height` and `Scarf_Joint_Seam` can cause print delay due to frequent flow changes.  
  - The outer wall speed is greater than the inner wall speed due to a deferent line width. This script will adapt the speed to any line width and layer height, making it very effective for `Variable_Layer_Height` and `Precise_Z_Height`.  
![371310408-26026ed0-d97e-4423-9d84-68c5b2a863e8](https://github.com/user-attachments/assets/e83fd21d-e34a-4def-869d-c62838b0b8b3)  
![image](https://github.com/user-attachments/assets/2fe5dd0f-008a-400b-9fa9-10228bf07b40)  
  
# Usage  
The script will popup once you Print or Export the G-Code from the Slicer, ask the user whether the script will be applied or not.   
  
![image](https://github.com/user-attachments/assets/f1589c73-8261-4171-89c9-ff0ca416f5fb)  
  
If yes, the first execution:  
  - You have to set the appropriate `Extruder/Printer` values.
  - Klipper Estimator script requires a file containing the maximum limits of the printer `(config .json)`.
    - For Klipper, you can get this file by entering the printer's IP address or by selecting a local file.
    - For other firmware, you'll need to edit the file manually and input the equivalent values.  
  Config.json file Example:  
    ```
    {
    "max_velocity": 400.0,
    "max_acceleration": 10000.0,
    "minimum_cruise_ratio": 0.5,
    "square_corner_velocity": 5.0,
    "instant_corner_velocity": 1.0,
    "mm_per_arc_segment": 0.1,
    "move_checkers": [
      {
        "axis_limiter": {
          "axis": [
            0.0,
            0.0,
            1.0
          ],
          "max_velocity": 5.0,
          "max_accel": 200.0
        }
      },
      {
        "extruder_limiter": {
          "max_velocity": 106.43243214765772,
          "max_accel": 2660.8108036914427
        }
      }
    ]
    }
    ```  
    Note that You have to set this file for each `Printer/Extruder` preset.  
  - After saving the `Extruder/Printer` preset, you need to select the `Filament Type`, then set the filament values (start with 1mm3/2) and save with a specific name. (for each Extruder)  
    ![image](https://github.com/user-attachments/assets/9b6c98a9-0847-4118-a9d6-f37696be13a9)  
    
  ### Note that:
  - In subsequent uses, the script can recognize the `Extruder/Printer` and `Filament` used, if they are written with the same name as in the slicer (Copy and Paste).  
    ![image](https://github.com/user-attachments/assets/7b467275-4bed-4927-adc6-0a6306d95de6)![image](https://github.com/user-attachments/assets/0219a6d0-63d1-4b7e-b465-d45c74db0d49)  
    ![image](https://github.com/user-attachments/assets/58a4ac0d-620c-4715-9b16-e55401641720)![image](https://github.com/user-attachments/assets/fefd7247-1c32-4f94-a1b5-1fc3124d0812)  
  - If you make any changes to the script, be sure to refresh the estimation and then regenerate the G-Code.  
    ![image](https://github.com/user-attachments/assets/50612330-e3c0-4bee-b368-66a5f3641955)  
  
# 3DBenchy Example
![image](https://github.com/user-attachments/assets/503d1f8b-22f4-4848-9ca2-123b641b2796)  
![image](https://github.com/user-attachments/assets/d22e0e20-9e8e-4fad-93d0-8c0590770d70)  
  
Below is the Generated G-Code 
  
![image](https://github.com/user-attachments/assets/b070daba-f98d-4948-bc27-c19f7718c22b)  
![a3204c69-90d5-42a7-bcb5-144e6ae8c590](https://github.com/user-attachments/assets/b948ca48-a4da-4d4b-9a8a-69ee79812703)
