$WshShell = New-Object -ComObject WScript.Shell
$Shortcut = $WshShell.CreateShortcut('C:\Users\purpl\Desktop\MidnightClub3-Port\MC3 - Midnight Club 3.lnk')
Write-Host "Target: " $Shortcut.TargetPath
Write-Host "Arguments: " $Shortcut.Arguments
Write-Host "WorkingDir: " $Shortcut.WorkingDirectory
Write-Host "Icon: " $Shortcut.IconLocation