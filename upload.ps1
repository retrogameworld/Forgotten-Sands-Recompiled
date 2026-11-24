param([switch]$DryRun)
# Publishes this folder to GitHub without the GitHub CLI. Started by upload.bat.
#   1. Checks that only this project's own files go up: no game files, no translated game code, no program files in
#      git, no keys or passwords, nothing personal - and that the release zip holds only the program and its
#      libraries.
#   2. Uses the GitHub sign-in that Git for Windows already keeps (Git Credential Manager; a sign-in window opens the
#      first time), creates the public repository if it isn't there yet, and pushes the code.
#   3. Puts release\ForgottenSandsRecompiled-Windows-x64.zip on a GitHub Release (replaced if that release already
#      has one).
# -DryRun: only the checks, nothing is uploaded.
# Optional, never uploaded: upload_private_words.txt next to this file, one word or path per line; the upload stops if
# any file that goes up (or any text inside the program) contains one of them.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $here
$repoName = 'Forgotten-Sands-Recompiled'
$zipName = 'ForgottenSandsRecompiled-Windows-x64.zip'
$zip = Join-Path $here "release\$zipName"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
function Stop-Here($why) { Write-Host ""; Write-Host "STOPPED: $why" -ForegroundColor Red; Write-Host "Nothing more was uploaded."; exit 1 }
function Git-Run { & git @args; if ($LASTEXITCODE -ne 0) { Stop-Here "git $($args -join ' ') failed (see above)" } }

if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Stop-Here "Git is not installed - get it from https://git-scm.com/download/win" }
if (-not (Test-Path $zip)) { Stop-Here "release\$zipName is missing" }

Write-Host "[1/5] Getting the files ready..."
if (-not (Test-Path '.git')) {
    & git init -q -b main; if ($LASTEXITCODE -ne 0) { Stop-Here "could not start a git repository here" }
}
Git-Run add -A
$files = @(& git ls-files --cached)
Write-Host "      $($files.Count) files"

Write-Host "[2/5] Checking that nothing from the game or your PC is in it..."
$allowed = '^(README\.md|BUILDING\.md|THIRD_PARTY\.md|LICENSE|release_readme\.txt|\.gitignore|\.gitattributes|upload\.bat|upload\.ps1|source/.+\.(cpp|h|hpp|inc|c|txt|toml|py|cmake))$'
$other = $files | Where-Object { $_ -notmatch $allowed }
if ($other) { $other | Select-Object -First 20 | ForEach-Object { "   $_" }; Stop-Here "files other than the project's own code and docs are in the folder (listed above)" }
$gameCode = $files | Where-Object { $_ -match '(^|/)ppc_(recomp\.\d+|func_mapping)\.cpp$|(^|/)default\.xex$|\.(iso|xex|forge|bik|xma|xpr|pak|dll|exe|zip)$' }
if ($gameCode) { $gameCode | Select-Object -First 10 | ForEach-Object { "   $_" }; Stop-Here "game files or translated game code are in the folder (listed above)" }
foreach ($f in $files) {
    if ((Get-Item -LiteralPath $f).Length -gt 2MB) { Stop-Here "$f is unexpectedly large" }
}
$words = @()
$wordFile = Join-Path $here 'upload_private_words.txt'
if (Test-Path $wordFile) { $words = @(Get-Content $wordFile | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
function Find-Word($text) {
    foreach ($w in $words) { if ($text -match ('(?i)(?<![A-Za-z0-9])' + [regex]::Escape($w) + '(?![A-Za-z0-9])')) { return $w } }
    return $null
}
foreach ($f in $files) {
    $t = [IO.File]::ReadAllText((Join-Path $here $f))
    if ($t -match '(?m)^\s*[A-Za-z0-9_]*(password|passwd|storepass|keypass|token|secret|api_key)\s*[:=]\s*["'']?[A-Za-z0-9+/_\-]{8,}') { Stop-Here "$f looks like it has a password or token in it" }
    if ($t -match '-----BEGIN [A-Z ]*PRIVATE KEY-----|ghp_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}') { Stop-Here "$f has a key or GitHub token in it" }
    $w = Find-Word $t
    if ($w) { Stop-Here "$f mentions '$w' (from upload_private_words.txt)" }
}
# the release zip: only the program, its libraries, settings.toml, README.txt and licences - nothing from the game
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $okName = '^ForgottenSandsRecompiled/(ForgottenSandsRecomp\.exe|settings\.toml|README\.txt|(avcodec|avformat|avutil|swresample)-\d+\.dll|dxcompiler\.dll|msvcp140(_atomic_wait)?\.dll|vcruntime140(_1)?\.dll|licenses/[^/]+\.(txt|md)|licenses/)$'
    $entries = @($z.Entries | Where-Object { $_.FullName -notmatch '/$' -or $_.FullName -match 'licenses/$' })
    $bad = $entries | Where-Object { ($_.FullName -replace '\\', '/') -notmatch $okName }
    if ($bad) { $bad | Select-Object -First 10 | ForEach-Object { "   $($_.FullName)" }; Stop-Here "the zip has files that do not belong in it (listed above)" }
    $exe = $entries | Where-Object { $_.FullName -match 'ForgottenSandsRecomp\.exe$' }
    if (-not $exe) { Stop-Here "the zip has no ForgottenSandsRecomp.exe" }
    foreach ($e in $entries | Where-Object { $_.FullName -match '\.(exe|txt|md|toml)$' }) {
        $s = $e.Open(); $ms = New-Object IO.MemoryStream; $s.CopyTo($ms); $s.Dispose()
        $text = [Text.Encoding]::GetEncoding(28591).GetString($ms.ToArray())
        $w = Find-Word $text
        if ($w) { Stop-Here "$($e.FullName) in the zip mentions '$w' (from upload_private_words.txt)" }
        if ($e.FullName -match '\.exe$' -and $text -match '[A-Za-z]:\\Users\\[^\\\x00]+\\') { Stop-Here "the program in the zip has a Windows user folder name in it" }
    }
} finally { $z.Dispose() }
Write-Host "      clean"
if ($DryRun) { Write-Host ""; Write-Host "Check done (DryRun) - nothing was uploaded."; exit 0 }

Write-Host "[3/5] Your GitHub sign-in (a sign-in window may open the first time)..."
$OutputEncoding = New-Object System.Text.UTF8Encoding $false
$bash = 'C:\Program Files\Git\bin\bash.exe'
if (Test-Path $bash) { $cred = & $bash -c "printf 'protocol=https\nhost=github.com\n\n' | git credential fill" }
else { $cred = "protocol=https`nhost=github.com`n`n" | & git credential fill }
$token = ($cred | Where-Object { $_ -like 'password=*' }) -replace '^password=', ''
if (-not $token) { Stop-Here "no GitHub sign-in - Git for Windows (with its Credential Manager) asks for one here" }
$h = @{ Authorization = "Bearer $token"; Accept = 'application/vnd.github+json'; 'User-Agent' = 'forgotten-sands-recompiled-upload' }
try { $me = Invoke-RestMethod -Headers $h -Uri 'https://api.github.com/user' } catch { Stop-Here "GitHub didn't accept the sign-in ($($_.Exception.Message))" }
$login = $me.login
# keep the sign-in so git push below doesn't ask again
if (Test-Path $bash) { $env:GCRED = ($cred -join "`n"); & $bash -c 'printf "%s\n\n" "$GCRED" | git credential approve'; Remove-Item Env:GCRED }
Write-Host "      signed in as $login"

$repo = "$login/$repoName"
$info = $null
try { $info = Invoke-RestMethod -Headers $h -Uri "https://api.github.com/repos/$repo" } catch { $info = $null }
if (-not $info) {
    Write-Host "      creating the public repository $repo"
    $req = @{ name = $repoName; description = 'Unofficial native PC port of Prince of Persia: The Forgotten Sands (Xbox 360) by static recompilation - bring your own disc'; private = $false; has_wiki = $false } | ConvertTo-Json
    try { $info = Invoke-RestMethod -Method Post -Headers $h -Uri 'https://api.github.com/user/repos' -Body ([Text.Encoding]::UTF8.GetBytes($req)) -ContentType 'application/json' }
    catch { Stop-Here "could not create the repository ($($_.Exception.Message))" }
}

Write-Host "[4/5] Uploading the code..."
$url = "https://github.com/$repo.git"
$old = & git config --get remote.origin.url
if (-not $old) { Git-Run remote add origin $url } else { Git-Run remote set-url origin $url }
& git diff --cached --quiet
if ($LASTEXITCODE -ne 0) {
    $msg = if (& git rev-parse -q --verify HEAD) { "Update $(Get-Date -Format yyyy-MM-dd)" } else { 'Forgotten Sands Recompiled' }
    Git-Run -c "user.name=$login" -c "user.email=$login@users.noreply.github.com" commit -q -m $msg
} else { Write-Host "      nothing new since the last upload" }
Git-Run push -u origin HEAD:main

Write-Host ("[5/5] Uploading the program ({0:N0} MB)..." -f ((Get-Item $zip).Length / 1MB))
$tag = 'v' + (Get-Item $zip).LastWriteTime.ToString('yyyy.MM.dd')
$rel = $null
try { $rel = Invoke-RestMethod -Headers $h -Uri "https://api.github.com/repos/$repo/releases/tags/$tag" } catch { $rel = $null }
if (-not $rel) {
    $body = @"
**Forgotten Sands Recompiled** - native Windows PC port of Prince of Persia: The Forgotten Sands (Xbox 360). No game files are included.

1. Unpack **$zipName** into a folder of its own.
2. Put your own disc image (.iso) of the Xbox 360 disc release (title ID 5553083E) into that folder.
3. Start ``ForgottenSandsRecomp.exe``. The first start unpacks the disc image into a ``game`` folder.

A controller is required. Settings: ``settings.toml`` next to the exe. See the README for details.
"@
    $req = @{ tag_name = $tag; target_commitish = 'main'; name = "Forgotten Sands Recompiled $tag"; body = $body; make_latest = 'true' } | ConvertTo-Json
    try { $rel = Invoke-RestMethod -Method Post -Headers $h -Uri "https://api.github.com/repos/$repo/releases" -Body ([Text.Encoding]::UTF8.GetBytes($req)) -ContentType 'application/json' }
    catch { Stop-Here "could not create the release ($($_.Exception.Message))" }
} else {
    foreach ($a in $rel.assets) { if ($a.name -eq $zipName) { Invoke-RestMethod -Method Delete -Headers $h -Uri $a.url | Out-Null; Write-Host "      (the older $zipName on $tag replaced)" } }
}
$up = "https://uploads.github.com/repos/$repo/releases/$($rel.id)/assets?name=$zipName"
try { Invoke-RestMethod -Method Post -Headers $h -Uri $up -InFile $zip -ContentType 'application/zip' -TimeoutSec 7200 | Out-Null }
catch { Stop-Here "the program didn't upload ($($_.Exception.Message))" }
Write-Host ""
Write-Host "Done! https://github.com/$repo  (download: $($rel.html_url))" -ForegroundColor Green
