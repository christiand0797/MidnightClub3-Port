# MC3 Browser Multiplayer — One-command launcher

Starts the WebSocket relay + Stream proxy together, then opens the browser client.

#Requires -RunAsAdministrator
$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$relayDir = Join-Path $scriptDir 'relay'
$streamDir = Join-Path $scriptDir 'stream'
$browserHtml = Join-Path $scriptDir 'browser/index.html'

Write-Host "=== MC3 Browser Multiplayer Launcher ===" -ForegroundColor Cyan

# Check relay is running
$relayPort = 8080
$conn = Get-NetTCPConnection -LocalPort $relayPort -ErrorAction SilentlyContinue
if (-not $conn) {
    Write-Host "Starting WebSocket relay on port $relayPort..." -ForegroundColor Yellow
    Start-Process node -ArgumentList "server.js" -WorkingDirectory $relayDir -WindowStyle Hidden -RedirectStandardOutput "$relayDir/relay.log" -RedirectStandardError "$relayDir/relay.err"
    Start-Sleep -Seconds 2
} else {
    Write-Host "Relay already running on port $relayPort" -ForegroundColor Green
}

# Start stream proxy
$streamPort = 8765
Write-Host "Starting Stream Proxy (UDP $streamPort, WebSocket $($streamPort + 1))..." -ForegroundColor Yellow
$proxyProc = Start-Process py -ArgumentList "-3.12", "tools/multiplayer/stream/stream_proxy.py", "--relay", "ws://localhost:8080", "--port", $streamPort, "--fps", "60", "--quality", "75", "--scale", "1.0" -WorkingDirectory $scriptDir -WindowStyle Hidden -RedirectStandardOutput "$streamDir/proxy.log" -RedirectStandardError "$streamDir/proxy.err"
Start-Sleep -Seconds 2

# Open browser client
Write-Host "Opening browser client..." -ForegroundColor Cyan
Start-Process "$browserHtml?relay=ws://localhost:8080&stream=ws://localhost:$($streamPort + 1)"

Write-Host "`n=== Multiplayer stack running ===" -ForegroundColor Green
Write-Host "Relay:    ws://localhost:8080"
Write-Host "Proxy:    UDP $streamPort / WS $($streamPort + 1)"
Write-Host "Browser:  $browserHtml"
Write-Host ""
Write-Host "To stop: Close the game, then press Ctrl+C in this window (or kill node/python processes)."
Write-Host ""

# Keep window open
while ($true) { Start-Sleep -Seconds 5 }
