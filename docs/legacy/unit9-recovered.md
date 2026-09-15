# Recovering `Unit9` — the missing form

Archaeology, recorded so the finding is not lost and so nobody repeats the investigation.

## The problem

`Source/V1.1/Unit1.pas:244` imports `Unit9`:

```pascal
uses Unit2, Unit3, Unit4, Unit5, Unit6, Unit7, Unit8, Unit9;
```

No `Unit9.pas` or `Unit9.dfm` exists anywhere in the repository, and `Unit9` is absent
from `Project1.dpr`. Searching the full history finds nothing:

```
git log --all --diff-filter=A -- "*Unit9*"     → no results (738 commits)
```

The import was introduced in commit `3e51ef3` ("Update of May 12 2025"), the most recent
change to that file. It appears to be work-in-progress that was never committed — the
author's local tree had the file; the repository never did.

`Form9` is referenced exactly once, at `Unit1.pas:1036`:

```pascal
procedure TForm1.Button2Click(Sender: TObject);
begin
  Form9.showmodal;
end;
```

directly parallel to `Button1Click` → `Form8.ShowModal`. So it is one modal dialog behind
one button.

## What it was

Delphi embeds form definitions as `RT_RCDATA` resources in the compiled executable, so the
form survives in the shipped V1.2 binary even though its source does not. Scanning
`Source/V1.2/SB53-Systems.exe` recovers `TForm9`'s structure:

- A **`TCommPortDriver`** named `CommPortDriver1` — serial port access — configured for a
  custom port (`\\.\COM4`) and custom baud rate, with line-status checking and an
  `OnReceiveData` handler.
- A **tabbed layout** with pages captioned **`Calibration`**, **`Optemizations`** *(sic)*,
  and **`Fan Control`**.
- A chart on the Calibration page.
- Controls captioned `Communication port:` (a combo box defaulting to `None`), `Connect`,
  `Ombiant Temp :` *(sic)*, and `Object Temp :`.
- Handlers: `Button1Click`, `Button2Click`, `BitBtn2Click`, `Timer1Timer`, `Label5Click`,
  `CommPortDriver1ReceiveData`.

**`Unit9` is a live serial-port printer console** — connecting directly to the printer to
read temperatures and drive calibration and fan control.

This matches the README's "Future Development" list: *Intelligent Fan control*,
*Automatic filament calibration*, *Advanced flow calibration*.

## Which binary is which

Useful for anyone comparing the two shipped executables:

| Binary | Size | Contains |
|---|---|---|
| `bin/SB53-Systems.exe` | 9,121,280 | `TForm2`…`TForm8`, **no** `TForm9` |
| `Source/V1.2/SB53-Systems.exe` | 9,782,272 | `TForm2`…**`TForm9`** |

So `bin/` is a V1.1-generation build matching the committed source, and `Source/V1.2/` is
a later build containing the unreleased serial-console work. Both are **32-bit x86**.

**For differential comparison, use `bin/SB53-Systems.exe`** — it corresponds to the
released, documented behaviour.

## Consequence for the rewrite

**Not in scope.** `Unit9` has zero coupling to the G-code processing pipeline — it is an
independent dialog for live printer interaction, and it was never released.

Its absence is therefore not a blocker at all. It is one of the two reasons the legacy
source does not compile, and the fix is simply not to port it.

The concept is worth revisiting eventually. [ALGORITHM.md §5.4](../ALGORITHM.md) notes
that slew limiting is currently **open-loop** — it assumes the hotend achieves its
commanded heating and cooling rates. A serial connection giving real thermal response
would allow closing that loop, which is plainly what this dialog was reaching toward.
Recorded as an open question, not a task.
