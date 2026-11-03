# Запуск RIP 3 с модом. Рассчитан на обычного человека: двойной щелчок по start_rip3.cmd.
# Все этапы показываются по-русски; ошибки остаются на экране, подробности — в build\launch.log.
$ErrorActionPreference = 'Stop'
$root    = Split-Path -Parent $MyInvocation.MyCommand.Path
$build   = Join-Path $root 'build'
$log     = Join-Path $build 'launch.log'
$injlog  = Join-Path $build 'inject.log'
$expected = 'C4305AA61DBC06E72776E936943300DDDF3A6D82176CD393C4F50D04C9D925D2'

try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch {}
$Host.UI.RawUI.WindowTitle = 'RIP 3 — запуск с модом'

function Write-Log([string]$msg) {
    try { Add-Content -LiteralPath $log -Value ("{0} {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg) -Encoding UTF8 } catch {}
}
function Step([string]$msg) {
    Write-Host ""
    Write-Host "  > $msg" -ForegroundColor Cyan
    Write-Log "STEP $msg"
}
function Ok([string]$msg) {
    Write-Host "    [OK] $msg" -ForegroundColor Green
    Write-Log "OK $msg"
}
function Warn([string]$msg) {
    Write-Host "    [!] $msg" -ForegroundColor Yellow
    Write-Log "WARN $msg"
}
function Fail([string]$msg, [int]$code) {
    Write-Host ""
    Write-Host "  [ОШИБКА] $msg" -ForegroundColor Red
    Write-Host ""
    Write-Log "ERROR $msg"
    exit $code
}

Clear-Host
Write-Host "==================================================" -ForegroundColor DarkCyan
Write-Host "        RIP 3: The Last Hero — мод" -ForegroundColor White
Write-Host "   русский язык • FSR • сетевой кооп" -ForegroundColor Gray
Write-Host "==================================================" -ForegroundColor DarkCyan
Write-Log "=== запуск ==="

# 1. Проверяем, что рядом лежит собранный мод.
Step "Проверяю файлы мода..."
$required = @(
    'inject.exe', 'rip3coop.exe',
    'rip3video.dll', 'rip3ru.dll', 'rip3net.dll',
    'rip3video.ini'
)
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $build $name))) {
        Fail "Не найден build\$name. Собери мод: scripts\build.ps1." 1
    }
}
Ok "Все файлы на месте."

# 2. Игра не должна быть запущена: инжектор подключается только к новому процессу.
$old = @(Get-Process -Name RIP3 -ErrorAction SilentlyContinue)
if ($old.Count -gt 0) {
    Warn "RIP 3 уже запущена, жду закрытия..."
    $until = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $until -and @(Get-Process -Name RIP3 -ErrorAction SilentlyContinue).Count -gt 0) {
        Start-Sleep -Milliseconds 500
    }
    if (@(Get-Process -Name RIP3 -ErrorAction SilentlyContinue).Count -gt 0) {
        Fail "Игра не закрылась за 20 секунд. Закрой её и повтори." 1
    }
    Start-Sleep -Seconds 2
    Ok "Игра закрыта, продолжаю."
}

# 3. Запускаем игру через Steam. После авто-перезапуска Steam иногда ещё
#    считает старый процесс живым, поэтому пробуем несколько раз.
Step "Запускаю Steam и RIP 3..."
$game = $null
for ($try = 1; $try -le 3 -and -not $game; $try++) {
    try { Start-Process 'steam://rungameid/2550' }
    catch { Fail "Не удалось открыть Steam. Запусти Steam вручную и повтори." 2 }
    $until = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $until) {
        $game = Get-Process -Name RIP3 -ErrorAction SilentlyContinue |
                Sort-Object StartTime -Descending | Select-Object -First 1
        if ($game) { break }
        Start-Sleep -Milliseconds 25
    }
    if (-not $game -and $try -lt 3) {
        Warn "Steam не запустил игру, повторяю ($try/3)..."
        Start-Sleep -Seconds 3
    }
}
if (-not $game) {
    Fail "Игра не запустилась за минуту. Проверь, что Steam открыт и ты вошёл в аккаунт." 3
}
Ok "Игра запущена (PID $($game.Id))."

# 4. Проверяем версию exe: мод патчит фиксированные адреса и не переживает обновление игры.
Step "Проверяю версию игры..."
try {
    $gamePath = $game.Path
    $hash = (Get-FileHash -LiteralPath $gamePath -Algorithm SHA256).Hash
} catch {
    Fail "Не удалось прочитать RIP3.exe: $($_.Exception.Message)" 4
}
if ($hash -ne $expected) {
    Write-Host "    Найдена другая версия игры:" -ForegroundColor Yellow
    Write-Host "      $hash" -ForegroundColor DarkGray
    Fail "Эта версия мода не подходит. Игра, скорее всего, обновилась. Мод не подключён." 5
}
Ok "Версия игры подходит."

# 5. Подключаем DLL.
Step "Подключаю мод..."
$dlls = @('rip3video.dll', 'rip3ru.dll', 'rip3net.dll') | ForEach-Object { Join-Path $build $_ }
$out = & (Join-Path $build 'inject.exe') --pid $game.Id @dlls 2>&1
$code = $LASTEXITCODE
$out | Out-File -LiteralPath $injlog -Encoding UTF8 -Append
if ($code -ne 0) {
    Write-Host ""
    $out | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
    Fail "Не удалось подключить мод (код $code). Подробности: build\inject.log" 6
}
Ok "Мод подключён."

Start-Sleep -Seconds 2
$game.Refresh()
if ($game.HasExited) {
    Fail "Игра закрылась сразу после подключения. Посмотри build\rip3video.log и build\rip3net.log." 7
}

Write-Host ""
Write-Host "==================================================" -ForegroundColor Green
Write-Host "   ГОТОВО. Игра запущена с модом." -ForegroundColor Green
Write-Host "==================================================" -ForegroundColor Green
Write-Host ""
Write-Host "   В главном меню: «СЕТЕВАЯ ИГРА» или F7 — кооп." -ForegroundColor White
Write-Host "   Настройки экрана: build\rip3video.ini" -ForegroundColor Gray
Write-Host ""
Write-Host "   Это окно закроется через 6 секунд..." -ForegroundColor DarkGray
for ($i = 6; $i -ge 1; $i--) {
    Write-Host ("`r   {0} " -f $i) -NoNewline
    Start-Sleep -Seconds 1
}
Write-Host ""
exit 0
