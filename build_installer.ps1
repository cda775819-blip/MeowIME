# build_installer.ps1 — build single-file installer MeowIME_Setup.exe
$ErrorActionPreference = 'Stop'

$root     = Split-Path -Parent $MyInvocation.MyCommand.Path
$staging  = Join-Path $root 'installer_staging'
$manifest = Join-Path $root 'installer_manifest.txt'
$rcFile   = Join-Path $root 'installer_res.rc'
$resFile  = Join-Path $root 'installer_res.res'
$outExe   = Join-Path $root 'MeowIME_Setup.exe'

# 1. clean and rebuild staging
if (Test-Path $staging) { Remove-Item $staging -Recurse -Force }
New-Item -ItemType Directory -Path (Join-Path $staging 'rime_user\build') -Force | Out-Null

# 2. copy distributable files
# 注意：以前这里固定抓 CatTextService18.dll（手工编号的构建），
# 改了代码但忘了改这个数字就会打出旧版本。现在统一用 build.ps1 的产物。
Copy-Item (Join-Path $root '_build\CatTextService.dll') (Join-Path $staging 'CatTextService.dll') -Force
Copy-Item (Join-Path $root 'rime.dll') (Join-Path $staging 'rime.dll') -Force
Copy-Item (Join-Path $root 'rime_data') (Join-Path $staging 'rime_data') -Recurse -Force
Copy-Item (Join-Path $root 'rime_user\build\*') (Join-Path $staging 'rime_user\build') -Recurse -Force

# 2.5 第三方许可证声明
# 包里分发了 librime（BSD-3-Clause）和 Rime 方案/词典数据（LGPL-3.0），
# 两份许可证都要求随二进制附带版权声明与许可证全文，缺了就是侵权。
# 安装器会把它们一起释放到安装目录，用户能直接查到。
Copy-Item (Join-Path $root 'THIRD-PARTY-NOTICES.txt') $staging -Force
Copy-Item (Join-Path $root 'LICENSE')                $staging -Force
Copy-Item (Join-Path $root 'licenses')               $staging -Recurse -Force

# 3. enumerate files, build manifest + rc
$files = Get-ChildItem $staging -Recurse -File
$sb = New-Object System.Text.StringBuilder
$rc = New-Object System.Text.StringBuilder
$i = 0
foreach ($f in $files) {
    $rel = $f.FullName.Substring($staging.Length + 1)
    $res = "F$i"
    [void]$sb.AppendLine("$res=$rel")
    $rcPath = $f.FullName.Replace('\', '\\')
    [void]$rc.AppendLine("$res RCDATA `"$rcPath`"")
    $i++
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($manifest, $sb.ToString(), $utf8NoBom)
[void]$rc.AppendLine("FILELIST RCDATA `"$($manifest.Replace('\','\\'))`"")

# 版本信息。分发的 exe 若没有版本信息，右键看属性是空白，SmartScreen 和杀软
# 也更容易起疑。改版本号改这里 + installer.cpp 无关。
[void]$rc.AppendLine(@"
1 VERSIONINFO
FILEVERSION 0,1,2,0
PRODUCTVERSION 0,1,2,0
FILEOS 0x40004L
FILETYPE 0x1L
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "080404b0"
    BEGIN
      VALUE "CompanyName",      "MeowIME Contributors"
      VALUE "FileDescription",  "喵喵输入法 安装程序 (MeowIME Setup)"
      VALUE "FileVersion",      "0.1.2.0"
      VALUE "InternalName",     "MeowIME_Setup"
      VALUE "LegalCopyright",   "MIT License"
      VALUE "OriginalFilename", "MeowIME_Setup.exe"
      VALUE "ProductName",      "喵喵输入法 (MeowIME)"
      VALUE "ProductVersion",   "0.1.2.0"
      VALUE "Comments",         "https://github.com/cda775819-blip/MeowIME"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x804, 1200
  END
END
"@)

[System.IO.File]::WriteAllText($rcFile, $rc.ToString(), $utf8NoBom)

Write-Host "files in manifest: $i"

# 4. compile resource + link installer
$vsDevCmd = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
$cmd = 'call "' + $vsDevCmd + '" >nul && ' +
       'rc.exe /nologo /c65001 /fo "' + $resFile + '" "' + $rcFile + '" && ' +
       'cl.exe /nologo /MT /EHsc /utf-8 "' + $root + '\installer.cpp" "' + $resFile + '" ' +
       '/Fe:"' + $outExe + '" /link /SUBSYSTEM:WINDOWS shell32.lib user32.lib advapi32.lib ole32.lib && ' +
       'mt.exe -nologo -manifest "' + $root + '\installer.manifest" -outputresource:"' + $outExe + ';#1"'
cmd /c $cmd

if ($LASTEXITCODE -ne 0) { Write-Host "build failed, exit $LASTEXITCODE" ; exit 1 }
Write-Host "build ok: $outExe"
