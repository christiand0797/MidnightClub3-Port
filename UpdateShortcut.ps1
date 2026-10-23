$WshShell = New-Object -ComObject WScript.Shell
$Shortcut = $WshShell.CreateShortcut('C:\Users\purpl\Desktop\MidnightClub3-Port\MC3 - Midnight Club 3.lnk')
$Shortcut.TargetPath = 'powershell.exe'
$Shortcut.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "C:\Users\purpl\Desktop\MidnightClub3-Port\LaunchGame.ps1"'
$Shortcut.WorkingDirectory = 'C:\Users\purpl\Desktop\MidnightClub3-Port'
$Shortcut.IconLocation = 'C:\Users\purpl\Desktop\MidnightClub3-Port\mc3_icon.ico'
$Shortcut.Description = 'Midnight Club 3: DUB Edition Remix - Native PC Port'
$Shortcut.Save()