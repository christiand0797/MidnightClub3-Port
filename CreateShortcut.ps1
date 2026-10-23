$WshShell = New-Object -ComObject WScript.Shell
$Shortcut = $WshShell.CreateShortcut('MC3 - Midnight Club 3.lnk')
$Shortcut.TargetPath = 'cmd.exe'
$Shortcut.Arguments = '/c cd /d "' + (Get-Location).Path + '" && Launch-MC3.bat'
$Shortcut.WorkingDirectory = (Get-Location).Path
$Shortcut.IconLocation = (Get-Location).Path + '\mc3_icon.ico'
$Shortcut.Description = 'Midnight Club 3: DUB Edition Remix - Native PC Port'
$Shortcut.Save()