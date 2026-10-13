# install.ps1 and uninstall.ps1 as a whole in answers mode, against the fake adb of tests\fakeadb.py (what
# tests\test_answers.py does with install.sh): every question from its variable, a question without an answer an
# error that names the variable, the progress stream, and nothing written.
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\answers.Tests.ps1
# Each run is a PowerShell of its own, of the edition this one is, in which `adb` is a function that runs the fake -
# a function wins over any adb.exe, so nothing here can reach a real device - with no self-update and a scratch work
# directory. Needs Python 3 (the fake, and the installer's own tools). Exit code: the number of failed checks.
$ErrorActionPreference = 'Stop'
$Top = Split-Path -Parent $PSScriptRoot
$script:failed = 0
$script:passed = 0
function Check($name, $got, $want) {
    if ([string]$got -ceq [string]$want) { $script:passed++ }
    else { $script:failed++; Write-Host "FAIL $name`n  got:  [$got]`n  want: [$want]" -ForegroundColor Red }
}
$py = $null
foreach ($n in 'python', 'python3', 'py') {
    foreach ($c in @(Get-Command $n -CommandType Application -ErrorAction SilentlyContinue)) {
        $exe = $(if ($c.Source) { $c.Source } else { $c.Path })
        $ok = $false
        try { & $exe -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' 2>$null | Out-Null; $ok = ($LASTEXITCODE -eq 0) } catch { }
        if ($ok -and -not $py) { $py = $exe }
    }
}
if (-not $py) {
    if ($env:GITHUB_ACTIONS) { Write-Host 'FAIL: no Python 3 for the fake adb'; exit 1 }
    Write-Host 'no Python 3 here: the installers were not run against the fake adb'
    exit 0
}

$fx = Join-Path ([IO.Path]::GetTempPath()) ('mu300-answers-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $fx | Out-Null
$fake = Join-Path (Join-Path $Top 'tests') 'fakeadb.py'
$state = Join-Path $fx 'device.json'
$self = (Get-Process -Id $PID).Path
$script:run = 0

# Device KEY VALUE...: the fake device of the next runs (tests\fakeadb.py's DEFAULT has the keys), its log emptied
function Device {
    $h = @{}
    for ($i = 0; $i + 1 -lt $args.Count; $i += 2) { $h[[string]$args[$i]] = $args[$i + 1] }
    [IO.File]::WriteAllText($state, ($h | ConvertTo-Json -Depth 5 -Compress))
    Remove-Item "$state.log" -ErrorAction SilentlyContinue
}
function Calls([string]$Kind) {
    if (-not (Test-Path "$state.log")) { return @() }
    return @([IO.File]::ReadAllLines("$state.log") | Where-Object { $_ } | ForEach-Object { $_ | ConvertFrom-Json } | Where-Object { -not $Kind -or $_.kind -eq $Kind })
}
# what a run may never leave in the fake's log: a command that would have written, one the fake has no answer for,
# a reboot
function Untouched { return (@(Calls 'write').Count + @(Calls 'unknown').Count + @(Calls 'reboot').Count) }
function UntouchedWhy { return ((@(Calls 'write') + @(Calls 'unknown') + @(Calls 'reboot') | ForEach-Object { "$($_.kind): $($_.cmd) $($_.argv -join ' ')" }) -join ' | ') }

# RunScript SCRIPT PARAMS ENV STDIN: @{ Rc; Out; Events; Facts; Asked } of one run
function RunScript([string]$Script, [string]$Params, [hashtable]$Vars, [string]$Stdin) {
    $script:run++
    $wrapper = Join-Path $fx "run$($script:run).ps1"
    $text = "function adb { & '$py' '$fake' @args }`r`n& '$(Join-Path $Top $Script)' $Params`r`nexit `$LASTEXITCODE`r`n"
    [IO.File]::WriteAllText($wrapper, $text)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $self
    $psi.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$wrapper`""
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.WorkingDirectory = $fx
    foreach ($k in @($psi.EnvironmentVariables.Keys)) { if ("$k" -like 'MU300_*' -or "$k" -eq 'ANDROID_SERIAL') { $psi.EnvironmentVariables.Remove($k) } }
    $psi.EnvironmentVariables['FAKEADB'] = $state
    $psi.EnvironmentVariables['MU300_WORK'] = (Join-Path $fx 'work')
    $psi.EnvironmentVariables['MU300_NO_SELF_UPDATE'] = '1'
    $psi.EnvironmentVariables['MU300_PROGRESS'] = '1'
    $psi.EnvironmentVariables['MU300_LANG'] = 'en'
    $psi.EnvironmentVariables['MU300_RELEASE'] = 'v2026.10.23'
    if ($Vars) { foreach ($k in $Vars.Keys) { $psi.EnvironmentVariables[[string]$k] = [string]$Vars[$k] } }
    $p = [System.Diagnostics.Process]::Start($psi)
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    if ($Stdin) { $p.StandardInput.Write($Stdin) }
    $p.StandardInput.Close()
    if (-not $p.WaitForExit(300000)) { try { $p.Kill() } catch { }; return @{ Rc = -1; Out = 'TIMEOUT'; Events = @(); Facts = @{}; Asked = @() } }
    $out = $outTask.Result + $errTask.Result
    $events = @()
    foreach ($l in ($out -split "`r?`n")) {
        $l = $l.TrimStart([char]0xFEFF)
        if ($l.StartsWith('@@mu300 ')) { $events += ($l.Substring(8) | ConvertFrom-Json) }
    }
    $facts = @{}
    foreach ($e in $events) { if ($e.event -eq 'info') { $facts[$e.key] = $e.value } }
    $asked = @($events | Where-Object { $_.event -eq 'question' -and -not $_.state } | ForEach-Object { "$($_.id)=$($_.answer)" })
    return @{ Rc = $p.ExitCode; Out = $out; Events = $events; Facts = $facts; Asked = $asked }
}
function LastEvent($r) { if ($r.Events.Count -eq 0) { return $null }; return $r.Events[$r.Events.Count - 1] }

# ---- -Check with the progress stream: what the graphical installer reads the device with ---------------------------
Device
$r = RunScript 'install.ps1' '-Check -NonInteractive'
Check 'check: exit'             $r.Rc 0
if ($r.Rc -ne 0 -or (LastEvent $r).event -ne 'done') { Write-Host $r.Out }
Check 'check: hello'            "$($r.Events[0].event) $($r.Events[0].script) $($r.Events[0].mode)" 'hello install.ps1 check'
Check 'check: done'             "$((LastEvent $r).event) $((LastEvent $r).result)" 'done ok'
Check 'check: steps'            (@($r.Events | Where-Object { $_.event -eq 'step' -and $_.id } | ForEach-Object { $_.id }) -join ' ') 'preflight storage'
Check 'check: model'            $r.Facts['device.model'] 'F50 / MU300'
Check 'check: kind'             $r.Facts['device.kind'] 'f50'
Check 'check: build'            $r.Facts['android.build'] 'MU300_ZYV1.0.0B09'
Check 'check: bootloader'       "$($r.Facts['bootloader.locked']) $($r.Facts['bootloader.verified']) $($r.Facts['bootloader.state'])" '0 orange unlocked'
Check 'check: no lock warning'  ($r.Out -match 'The bootloader is locked') $false
Check 'check: root'             "$($r.Facts['root']) $($r.Facts['root.su'])" 'yes 27.0:MAGISKSU'
Check 'check: slots'            "$($r.Facts['slot.android']) $($r.Facts['slot.linux'])" 'a b'
Check 'check: eMMC'             "$($r.Facts['emmc.sectors']) $($r.Facts['emmc.last_end'])" '122159104 54218752'
Check 'check: region'           "$($r.Facts['region.offset']) $($r.Facts['region.size'])" "$(54222848 * 512) $((122150912 - 54222848) * 512)"
Check 'check: empty region'     "$($r.Facts['region.existing']) $($r.Facts['region.dirty']) $($r.Facts['verdict.code'])" 'no 0 tested'
Check 'check: no card'          "[$($r.Facts['sd.device'])]" '[]'
Check 'check: partitions'       (@(([string]$r.Facts['partitions']).Trim() -split ' ')[-1]) 'userdata:12275712:41943040'
Check 'check: the text is there' ($r.Out -match 'result: OK: free and empty, same layout as the tested device') $true
Check "check: nothing written: $(UntouchedWhy)"  (Untouched) 0

Device 'model' 'U30_Air' 'device' 'U30Air' 'slot' 'b' 'locked' '1' 'verified' 'green'
$r = RunScript 'install.ps1' '-Check -NonInteractive'
Check 'u30: kind'               $r.Facts['device.kind'] 'u30air'
Check 'u30: slots'              "$($r.Facts['slot.android']) $($r.Facts['slot.linux'])" 'b a'
Check 'u30: bootloader'         "$($r.Facts['bootloader.locked']) $($r.Facts['bootloader.verified'])" '1 green'
# a locked bootloader is a warning and never a refusal (a patched TrustOS boots Linux on one): the check goes through
Check 'locked: exit'            $r.Rc 0
Check 'locked: the fact'        $r.Facts['bootloader.state'] 'locked'
Check 'locked: the warning'     ($r.Out -match 'WARNING: The bootloader is locked\. Did you patch TrustOS\?') $true
Check 'locked: done'            (LastEvent $r).event 'done'
Device 'locked' '' 'device_state' 'locked'
$r = RunScript 'install.ps1' '-Check -NonInteractive'
Check 'locked by the other property' "$($r.Facts['bootloader.state']) $($r.Rc)" 'locked 0'
Device 'locked' '' 'device_state' ''
$r = RunScript 'install.ps1' '-Check -NonInteractive'
Check 'lock not reported'       "$($r.Facts['bootloader.state']) $($r.Out -match 'The bootloader is locked')" 'unknown False'

# no root: said before it stops, with what needs no root
Device 'root' $false
$r = RunScript 'install.ps1' '-Check -NonInteractive'
Check 'no root: exit'           $r.Rc 1
Check 'no root: fact'           $r.Facts['root'] 'no'
Check 'no root: model'          $r.Facts['device.model'] 'F50 / MU300'
Check 'no root: error'          "$((LastEvent $r).event): $((LastEvent $r).message)" 'error: su does not work on the device'
Check "no root: nothing written: $(UntouchedWhy)"  (Untouched) 0

# an installation that is there; data in the free space
Device 'existing' $true 'existing_blocks' 1048576
$r = RunScript 'install.ps1' '-Check -NonInteractive'
Check 'existing: facts'         "$($r.Facts['region.existing']) $($r.Facts['region.size']) $($r.Facts['verdict.code'])" "yes $(1048576 * 4096) existing"
Device 'dirty' 3
$r = RunScript 'install.ps1' '-Check -NonInteractive'
Check 'dirty: facts'            "$($r.Facts['region.dirty']) $($r.Facts['verdict.code'])" '3 dirty'

# the card
$card = @{ dev = 'mmcblk1'; sectors = 124735488; partition = $true; type = 'SD'; state = 'yes' }
Device 'sd' $card
$r = RunScript 'install.ps1' '-Check -NonInteractive'
Check 'card: facts'             "$($r.Facts['sd.device']) $($r.Facts['sd.bytes']) $($r.Facts['sd.state'])" "/dev/block/mmcblk1p1 $(124735488 * 512) yes"

# userdata fills the disk (issue #32): the figures of the make-room path, and -Check writes nothing
Device 'layout' 'vendor64'
$r = RunScript 'install.ps1' '-Check -NonInteractive'
Check 'make room: exit'         $r.Rc 0
Check 'make room: region'       $r.Facts['region.size'] '0'
Check 'make room: userdata'     "$($r.Facts['makeroom.first']) $($r.Facts['makeroom.end']) $($r.Facts['makeroom.usable_end'])" '12275712 122155007 122155007'
Check 'make room: limits'       "$($r.Facts['makeroom.min_android']) $($r.Facts['makeroom.min_linux']) $($r.Facts['makeroom.max_linux'])" '8388608 1638400 101490688'
Check 'make room: done'         (LastEvent $r).event 'done'
Check 'make room: no question'  $r.Asked.Count 0
Check "make room: nothing written: $(UntouchedWhy)"  (Untouched) 0

# the language is a question too
Device
$r = RunScript 'install.ps1' '-Check -NonInteractive' @{ MU300_LANG = '' }
Check 'language: exit'          $r.Rc 2
Check 'language: names it'      ($r.Out -match 'set MU300_LANG') $true
Check 'language: asked nothing of the device' @(Calls '').Count 0

# ---- -Backup: the boot image and the first block of misc, and nothing else -------------------------------------------
Device 'boot_image_bytes' 70000
$r = RunScript 'install.ps1' '-Backup -NonInteractive'
Check 'backup: exit'            $r.Rc 0
if ($r.Rc -ne 0) { Write-Host $r.Out }
$bf = [string]$r.Facts['backup.boot']
Check 'backup: the file'        ($bf -and (Test-Path $bf) -and (Get-Item $bf).Length -eq 70000) $true
if ($bf -and (Test-Path $bf)) { Check 'backup: its hash' (Get-FileHash $bf -Algorithm SHA256).Hash.ToLower() $r.Facts['backup.sha256'] }
Check 'backup: misc'            ((Test-Path ([string]$r.Facts['backup.misc'])) -and (Get-Item ([string]$r.Facts['backup.misc'])).Length -eq 4096) $true
Check 'backup: done'            (LastEvent $r).event 'done'
Check 'backup: two pulls'       @(Calls 'pull').Count 2
Check 'backup: no NV partition named' (@(Calls '' | Where-Object { "$($_.cmd)" -match 'prodnv|fixnv|runtimenv|deltanv|calinv|nr_phy|miscdata|persist' }).Count) 0
Check "backup: nothing written: $(UntouchedWhy)"  (Untouched) 0
Device 'pull' $false
$r = RunScript 'install.ps1' '-Backup -NonInteractive' @{ MU300_WORK = (Join-Path $fx 'work2') }
Check 'backup, no pull: exit'   $r.Rc 1
Check 'backup, no pull: error'  (LastEvent $r).event 'error'

# ---- an installation with every answer in a file, as far as the first pull (which the fake refuses) ---------------
$answers = [ordered]@{ MU300_ANS_SYSTEMS = '3'; MU300_OPENWRT = 'luci'; MU300_ANS_BOOT_OS = 'ubuntu'; MU300_ANS_UBUNTU = '26.04'
    MU300_ANS_DEFAULT_LINUX = 'yes'; MU300_ANS_BOOT_ATTEMPTS = '4'; MU300_ANS_HOTSPOT = 'no'; MU300_ANS_GPU = 'no'
    MU300_ANS_VPN = 'no'; MU300_ANS_ADB = 'no'; MU300_ANS_PANEL = 'yes'; MU300_ANS_KERNEL = '6.18'
    MU300_ANS_PASSWORD = 'hunter2secret'; MU300_ANS_CONFIRM = 'INSTALL' }
function AnswersFileOf($h) {
    $f = Join-Path $fx "answers$($script:run).txt"
    [IO.File]::WriteAllText($f, (($h.Keys | ForEach-Object { "$_=$($h[$_])" }) -join "`n") + "`n")
    return $f
}
function Without($h, [string]$Key) { $o = [ordered]@{}; foreach ($k in $h.Keys) { if ($k -ne $Key) { $o[$k] = $h[$k] } }; return $o }
function With($h, [hashtable]$More) { $o = [ordered]@{}; foreach ($k in $h.Keys) { $o[$k] = $h[$k] }; foreach ($k in $More.Keys) { $o[$k] = $More[$k] }; return $o }

Device 'pull' $false
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf $answers)`""
Check 'install: every question from its variable' ($r.Asked -join ' ') 'MU300_ANS_SYSTEMS=3 MU300_ANS_BOOT_OS=ubuntu MU300_ANS_UBUNTU=26.04 MU300_ANS_DEFAULT_LINUX=yes MU300_ANS_BOOT_ATTEMPTS=4 MU300_ANS_HOTSPOT=no MU300_ANS_GPU=no MU300_ANS_VPN=no MU300_ANS_ADB=no MU300_ANS_PANEL=yes MU300_ANS_KERNEL=6.18 MU300_ANS_PASSWORD='
if ($r.Asked.Count -ne 12) { Write-Host $r.Out }
Device 'pull' $false 'locked' '1'
$rl = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf $answers)`""
Check 'install, locked: warned and not stopped' "$($rl.Out -match 'WARNING: The bootloader is locked') $($rl.Asked.Count) $((LastEvent $rl).message)" 'True 12 pulling boot_a failed'
Check 'install: steps'          (@($r.Events | Where-Object { $_.event -eq 'step' -and $_.id } | ForEach-Object { $_.id }) -join ' ') 'preflight storage choices pull'
Check 'install: stops at the pull' "$((LastEvent $r).event): $((LastEvent $r).message)" 'error: pulling boot_a failed'
Check 'install: exit'           $r.Rc 1
Check 'install: the password is never shown' ($r.Out -match 'hunter2') $false
# adb is started with the answers in its environment, and never with the password
Check 'install: adb sees the answers' (@(Calls '' | Where-Object { $_.answers -contains 'MU300_ANS_KERNEL' }).Count -gt 0) $true
Check 'install: adb never sees the password' (@(Calls '' | Where-Object { @($_.answers | ForEach-Object { "$_".ToUpper() }) -contains 'MU300_ANS_PASSWORD' }).Count) 0
Check 'install: the release'    $r.Facts['release'] 'v2026.10.23'
Check "install: nothing written: $(UntouchedWhy)"  (Untouched) 0

foreach ($missing in 'MU300_ANS_SYSTEMS', 'MU300_ANS_KERNEL', 'MU300_ANS_PASSWORD', 'MU300_ANS_DEFAULT_LINUX', 'MU300_OPENWRT', 'MU300_ANS_VPN') {
    Device 'pull' $false
    $r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf (Without $answers $missing))`""
    Check "missing ${missing}: exit"        $r.Rc 1
    Check "missing ${missing}: names it"    ("$((LastEvent $r).event): $((LastEvent $r).message)" -match "^error: .*set $missing ") $true
    Check "missing ${missing}: nothing read" @(Calls 'pull').Count 0
    Check "missing ${missing}: nothing written: $(UntouchedWhy)" (Untouched) 0
}

# an answer the installer refuses: Ubuntu 26.04 needs a mainline kernel, the kernel question comes back, and stops
Device 'pull' $false
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf (With $answers @{ MU300_ANS_KERNEL = '5.4' }))`""
Check 'refused: exit'           $r.Rc 1
Check 'refused: says so'        (LastEvent $r).message 'the answer in MU300_ANS_KERNEL was not accepted: 5.4'
foreach ($bad in @{ MU300_ANS_SYSTEMS = '7' }, @{ MU300_ANS_BOOT_OS = 'android' }, @{ MU300_ANS_BOOT_ATTEMPTS = '9' }, @{ MU300_ANS_UBUNTU = '22.04' }, @{ MU300_ANS_PASSWORD = 'short' }) {
    $r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf (With $answers $bad))`""
    Check "bad $(@($bad.Keys)[0]): stops"   "$($r.Rc) $((LastEvent $r).event) $(@(Calls 'pull').Count)" '1 error 0'
}

# an installation that is there asks update or wipe
Device 'pull' $false 'existing' $true 'vpn_on' 'ubuntu'
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf $answers)`""
Check 'existing: asks'          ((LastEvent $r).message -match 'set MU300_ANS_EXISTING ') $true
Check 'existing: the VPN note'  ($r.Out -match 'The VPN is on in the system being replaced \(ubuntu\)') $true
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf (With $answers @{ MU300_ANS_EXISTING = 'update' }))`""
Check 'existing: update'        ($r.Asked -contains 'MU300_ANS_EXISTING=update') $true
Check 'existing: goes on'       (LastEvent $r).message 'pulling boot_a failed'
Check "existing: nothing written: $(UntouchedWhy)"  (Untouched) 0

# data in the free space needs its word
Device 'pull' $false 'dirty' 2
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf $answers)`""
Check 'dirty: asks'             ((LastEvent $r).message -match 'set MU300_ANS_OVERWRITE ') $true
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf (With $answers @{ MU300_ANS_OVERWRITE = '' }))`""
Check 'dirty: set to nothing is no' (LastEvent $r).message 'cancelled'
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf (With $answers @{ MU300_ANS_OVERWRITE = 'overwrite' }))`""
Check 'dirty: the word'         $r.Asked[0] 'MU300_ANS_OVERWRITE=overwrite'

# the card: where, and the word that erases it
Device 'pull' $false 'sd' (@{ dev = 'mmcblk1'; sectors = 124735488; partition = $true; type = 'SD'; state = 'no' })
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf $answers)`""
Check 'card: where is asked'    ((LastEvent $r).message -match 'set MU300_STORAGE ') $true
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf (With $answers @{ MU300_STORAGE = 'sd' }))`""
Check 'card: chosen'            $r.Facts['storage'] 'sd'
Check 'card: erasing is asked'  ((LastEvent $r).message -match 'set MU300_ANS_ERASE_SD ') $true
Device 'pull' $false 'sd' (@{ dev = 'mmcblk1'; sectors = 124735488; partition = $true; type = 'SD'; state = 'foreign' })
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf (With $answers @{ MU300_STORAGE = 'sd'; MU300_ANS_ERASE_SD = 'ERASE' }))`""
Check 'card: another filesystem is never formatted' ((LastEvent $r).message -match 'never formats one that is not its own') $true
Check "card: nothing written: $(UntouchedWhy)"  (Untouched) 0

# making room is never assumed
Device 'pull' 'gpt' 'layout' 'vendor64'
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf $answers)`""
Check 'make room: asked'        ((LastEvent $r).message -match 'set MU300_ANS_MAKE_ROOM ') $true
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf (With $answers @{ MU300_ANS_MAKE_ROOM = 'no' }))`""
Check 'make room: no'           (LastEvent $r).message 'nothing was changed.'
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf (With $answers @{ MU300_ANS_MAKE_ROOM = 'yes'; MU300_ANS_LINUX_GIB = '4' }))`""
Check 'make room: the word is asked' ((LastEvent $r).message -match 'set MU300_ANS_ERASE_ANDROID ') $true
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf (With $answers @{ MU300_ANS_MAKE_ROOM = 'yes'; MU300_ANS_LINUX_GIB = '4'; MU300_ANS_ERASE_ANDROID = 'erase' }))`""
Check 'make room: not the word' (LastEvent $r).message 'nothing was changed.'
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf (With $answers @{ MU300_ANS_MAKE_ROOM = 'yes'; MU300_ANS_LINUX_GIB = '900'; MU300_ANS_ERASE_ANDROID = 'ERASE' }))`""
Check 'make room: a size that does not fit' (LastEvent $r).message 'the answer in MU300_ANS_LINUX_GIB was not accepted: 900'
Check "make room: nothing written: $(UntouchedWhy)"  (Untouched) 0

# an answer on standard input, while it runs
Device 'pull' $false
$r = RunScript 'install.ps1' "-AnswersFile `"$(AnswersFileOf (With $answers @{ MU300_ANS_SYSTEMS = '@stdin' }))`"" $null "2`n"
Check 'stdin: waiting event'    (@($r.Events | Where-Object { $_.event -eq 'question' -and $_.state -eq 'waiting' } | ForEach-Object { $_.id }) -join ' ') 'MU300_ANS_SYSTEMS'
Check 'stdin: the answer'       $r.Asked[0] 'MU300_ANS_SYSTEMS=2'
Check 'stdin: one system, nothing to choose' (@($r.Asked | Where-Object { $_ -like 'MU300_ANS_BOOT_OS=*' }).Count) 0

# the variables from the environment (-NonInteractive), without a file
Device 'pull' $false
$r = RunScript 'install.ps1' '-NonInteractive' @{ MU300_ANS_SYSTEMS = '2'; MU300_OPENWRT = 'plain'; MU300_ANS_DEFAULT_LINUX = 'no'; MU300_ANS_HOTSPOT = 'yes'
    MU300_ANS_GPU = 'yes'; MU300_ANS_VPN = 'no'; MU300_ANS_ADB = 'no'; MU300_ANS_KERNEL = '5.4'; MU300_ANS_PASSWORD = 'hunter2secret' }
Check 'environment: questions'  ($r.Asked -join ' ') 'MU300_ANS_SYSTEMS=2 MU300_ANS_DEFAULT_LINUX=no MU300_ANS_HOTSPOT=yes MU300_ANS_GPU=yes MU300_ANS_VPN=no MU300_ANS_ADB=no MU300_ANS_KERNEL=5.4 MU300_ANS_PASSWORD='
Check 'environment: stops at the pull' (LastEvent $r).message 'pulling boot_a failed'
Check 'environment: the password is never shown' ($r.Out -match 'hunter2') $false
Check 'environment: adb sees the answers' (@(Calls '' | Where-Object { @($_.answers | ForEach-Object { "$_".ToUpper() }) -contains 'MU300_ANS_KERNEL' }).Count -gt 0) $true
Check 'environment: adb never sees the password' (@(Calls '' | Where-Object { @($_.answers | ForEach-Object { "$_".ToUpper() }) -contains 'MU300_ANS_PASSWORD' }).Count) 0

# ---- -AdbPath: asks the device nothing -----------------------------------------------------------------------------
Device
New-Item -ItemType Directory -Force -Path (Join-Path $fx 'platform-tools') | Out-Null
[IO.File]::WriteAllText((Join-Path (Join-Path $fx 'platform-tools') 'adb.exe'), '')
$r = RunScript 'install.ps1' '-AdbPath'
Check 'adb path: exit'          $r.Rc 0
Check 'adb path: an adb.exe'    ($r.Out.Trim() -match 'adb\.exe$') $true
Check 'adb path: the device was not asked' @(Calls '').Count 0

# ---- uninstall.ps1: nothing is removed without the word --------------------------------------------------------------
Device 'existing' $true
$r = RunScript 'uninstall.ps1' '-NonInteractive'
Check 'uninstall: hello'        "$($r.Events[0].event) $($r.Events[0].script)" 'hello uninstall.ps1'
Check 'uninstall: wipe is asked' ((LastEvent $r).message -match 'set MU300_ANS_WIPE ') $true
if ((LastEvent $r).message -notmatch 'set MU300_ANS_WIPE ') { Write-Host $r.Out }
$r = RunScript 'uninstall.ps1' '-NonInteractive' @{ MU300_ANS_WIPE = 'keep' }
Check 'uninstall: the word is asked' ((LastEvent $r).message -match 'set MU300_ANS_CONFIRM ') $true
Check 'uninstall: facts'        "$($r.Facts['region.existing']) $($r.Facts['slot.linux'])" 'yes b'
$r = RunScript 'uninstall.ps1' '-NonInteractive' @{ MU300_ANS_WIPE = 'keep'; MU300_ANS_CONFIRM = 'uninstall' }
Check 'uninstall: not the word' (LastEvent $r).message 'cancelled'
Check "uninstall: nothing written: $(UntouchedWhy)"  (Untouched) 0

Remove-Item -Recurse -Force $fx -ErrorAction SilentlyContinue
Write-Host "$script:passed passed, $script:failed failed"
exit $script:failed
