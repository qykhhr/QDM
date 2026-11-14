param(
    [ValidateSet('Chrome','Edge','Both')][string]$Browser = 'Both',
    [string]$QdmDir = (Join-Path $PSScriptRoot '..\build\release\bin'),
    [string]$DataDir = '',
    [switch]$Unregister
)
# 只注册当前用户，无需管理员。JSON 使用无 BOM UTF-8，路径由 ConvertTo-Json 转义。
# 本脚本不安装浏览器扩展，不修改浏览器安全设置，也不终止正在下载的 QDM。
$ErrorActionPreference = 'Stop'
$qdmResolvedDir = (Resolve-Path -LiteralPath $QdmDir).Path
$qdmHostName = 'com.qdm.download_manager'
$qdmManifestPath = Join-Path $qdmResolvedDir 'qdm-native-host.json'
$qdmKeys = @()
if ($Browser -in 'Chrome','Both') { $qdmKeys += "HKCU:\Software\Google\Chrome\NativeMessagingHosts\$qdmHostName" }
if ($Browser -in 'Edge','Both') { $qdmKeys += "HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\$qdmHostName" }
if ($Unregister) {
    foreach ($qdmKey in $qdmKeys) {
        if (Test-Path -LiteralPath $qdmKey) {
            $qdmCurrent = (Get-Item -LiteralPath $qdmKey).GetValue('')
            if ($qdmCurrent -ne $qdmManifestPath) { throw "Registration points to another installation: $qdmCurrent" }
            Remove-Item -LiteralPath $qdmKey
        }
    }
    Write-Output 'Removed selected QDM host registrations. Application files and downloads were preserved.'
    exit
}
$qdmHostPath = Join-Path $qdmResolvedDir 'QDM.NativeHost.exe'
if (!(Test-Path -LiteralPath $qdmHostPath -PathType Leaf)) { throw 'Build QDM.NativeHost.exe before registration.' }
$qdmManifest = @{ name=$qdmHostName; description='QDM desktop download bridge'; path=$qdmHostPath; type='stdio'; allowed_origins=@('chrome-extension://nkblcdnjneadmggjpnjajlbebpbmhjpj/') }
[IO.File]::WriteAllText($qdmManifestPath, ($qdmManifest | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding($false)))
$qdmDataPath = ''
if ($DataDir) { $qdmDataPath = [IO.Path]::GetFullPath($DataDir) }
[IO.File]::WriteAllText((Join-Path $qdmResolvedDir 'native-host-settings.json'), (@{data_dir=$qdmDataPath} | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
foreach ($qdmKey in $qdmKeys) {
    New-Item -Path $qdmKey -Force | Out-Null
    Set-Item -LiteralPath $qdmKey -Value $qdmManifestPath
}
Write-Output "Registered QDM native host for $Browser. Load browser-extension in the browser's developer mode, then start QDM 0.2.0+."
