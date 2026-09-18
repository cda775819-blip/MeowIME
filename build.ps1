# build.ps1 —— 喵喵助手 TSF 输入法：编译 + 部署 + 注册
#
# 用法：
#   .\build.ps1              只编译到 _build\CatTextService.dll
#   .\build.ps1 -Deploy      编译后覆盖到部署位置并重新注册（改代码后跑这个）
#   .\build.ps1 -Debug       顺便打开调试日志（会在 DLL 同目录生成 cand_debug.log）
#   .\build.ps1 -NoDebug     关掉调试日志（排查完记得关，日志里有你打的字）
#   .\build.ps1 -RebuildRime 重建 Rime 部署（改了 rime_data 下的配置才需要）
#
# 为什么要这个脚本：以前是十几个手工编号的 DLL（CatTextService2..18）没有版本号，
# 注册表的 HKLM 和 HKCU 两处还分别指向不同编号的构建，改完代码编译成新文件
# 根本不生效，很难判断当前跑的到底是哪一版。
# 现在固定一个输出路径 + 固定注册目标，改完 -Deploy 一次就生效。
param(
    [switch]$Deploy,
    [switch]$Debug,
    [switch]$NoDebug,
    [switch]$RebuildRime
)
$ErrorActionPreference = 'Stop'

# 脚本所在目录就是项目根目录（不要写死绝对路径，别人 clone 下来要能直接用）
$root    = Split-Path -Parent $MyInvocation.MyCommand.Path
$outDir  = Join-Path $root '_build'
$outDll  = Join-Path $outDir 'CatTextService.dll'
# 部署位置：放在 rime.dll / rime_data / rime_user 旁边，
# DLL 内部是按自身目录找 rime 运行时的（g_dllDir + "rime.dll"）
$deployDll = Join-Path $root 'CatTextService.dll'
$debugFlag = Join-Path $root 'CatTextService.debug'
$logFile   = Join-Path $root 'cand_debug.log'

$clsid = '{1F8A3C21-5B7E-4A2D-9E3F-4C8B1D2E7A5F}'
$hkcuClsid = "HKCU:\SOFTWARE\Classes\CLSID\$clsid\InprocServer32"

# ---------- 1. 编译 ----------
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$vs = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vs)) { throw "找不到 vcvars64.bat：$vs" }

# ---------- 1a. 生成版本资源并编进 DLL ----------
# 为什么必须做：这个 DLL 以前没有任何版本信息，19 个历史构建文件长得一模一样，
# "手上这个文件到底是哪一版"只能靠算哈希 —— 排查时为此白费过时间。
# 现在右键看属性就能确认；构建号用时间戳，保证每次编译都不同。
$rcFile  = Join-Path $outDir 'version.rc'
$resFile = Join-Path $outDir 'version.res'
$buildId = (Get-Date -Format 'yyyyMMdd.HHmm')
$stamp   = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
$rcText = @"
1 VERSIONINFO
FILEVERSION 0,1,3,0
PRODUCTVERSION 0,1,3,0
FILEOS 0x40004L
FILETYPE 0x2L
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "080404b0"
    BEGIN
      VALUE "CompanyName",      "MeowIME Contributors"
      VALUE "FileDescription",  "喵喵输入法 TSF 文本服务"
      VALUE "FileVersion",      "0.1.3.0"
      VALUE "InternalName",     "CatTextService"
      VALUE "LegalCopyright",   "MIT License"
      VALUE "OriginalFilename", "CatTextService.dll"
      VALUE "ProductName",      "喵喵输入法 (MeowIME)"
      VALUE "ProductVersion",   "0.1.3.0"
      VALUE "Comments",         "build $buildId ($stamp)"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x804, 1200
  END
END
"@
[System.IO.File]::WriteAllText($rcFile, $rcText, (New-Object System.Text.UTF8Encoding($false)))
# /c65001 不能省：.rc 是 UTF-8 且含中文，rc.exe 默认按系统 ANSI 代码页读会乱码
cmd /c ('call "' + $vs + '" >nul && rc.exe /nologo /c65001 /fo "' + $resFile + '" "' + $rcFile + '"')
if ($LASTEXITCODE -ne 0) { throw "版本资源编译失败" }
Write-Host "[1a] 版本资源已生成（build $buildId）" -ForegroundColor Gray

# /MT 静态链接 CRT —— 必须。否则会依赖 MSVCP140.dll / VCRUNTIME140.dll，
# 在没装 VC++ 运行库的机器上 DLL 根本加载不了。
$cmd = 'call "' + $vs + '" >nul && cd /d "' + $outDir + '" && ' +
       'cl.exe /nologo /LD /MT /EHsc /utf-8 /W3 ' +
       '/I"' + $root + '\rime_dl\rime\dist\include" ' +
       '"' + $root + '\CatTextService.cpp" "' + $resFile + '" /Fe:"' + $outDll + '" ' +
       '/link /DEF:"' + $root + '\CatTextService.def" ' +
       'ole32.lib oleaut32.lib uuid.lib user32.lib advapi32.lib gdi32.lib'
cmd /c $cmd
if ($LASTEXITCODE -ne 0) { throw "编译失败，exit $LASTEXITCODE" }
if (-not (Test-Path $outDll)) { throw "编译没有产出 $outDll" }
$len = (Get-Item $outDll).Length
Write-Host "[1/4] 编译完成 $outDll ($len bytes)" -ForegroundColor Green

# ---------- 2. 部署 ----------
if ($Deploy) {
    # 先看看谁真加载着旧 DLL（被加载着就覆盖不了）
    $holders = @()
    Get-Process -ErrorAction SilentlyContinue | ForEach-Object {
        $p = $_
        try {
            if ($p.Modules | Where-Object { $_.FileName -like '*CatTextService*.dll' }) {
                $holders += $p.ProcessName
            }
        } catch { }
    }
    if ($holders.Count -gt 0) {
        Write-Host "      正加载旧 DLL 的程序：$(($holders | Sort-Object -Unique) -join ', ')" -ForegroundColor Yellow
    } else {
        Write-Host "      当前没有进程加载旧 DLL" -ForegroundColor Gray
    }

    # 正在运行的程序会锁住已加载的 DLL，直接覆盖会 ERROR_SHARING_VIOLATION。
    # 解决办法不是让用户去关程序，而是换个文件名部署、把注册指过去 ——
    # 注册表指向哪个文件是我们说了算的，换了名字照样生效。
    $target = $deployDll
    $n = 0
    while ($true) {
        try {
            Copy-Item $outDll $target -Force -ErrorAction Stop
            break
        } catch {
            if ($n -ge 8) {
                throw "连 $target 都写不进去：$($_.Exception.Message)`n关掉聊天软件/浏览器后重跑。"
            }
            $n++
            $target = Join-Path $root "CatTextService.$n.dll"
            Write-Host "      $deployDll 被占用，改用 $(Split-Path $target -Leaf)" -ForegroundColor Yellow
        }
    }
    Write-Host "[2/4] 已部署到 $target" -ForegroundColor Green

    # 注册：只改 HKCU 的 InprocServer32。HKCU 优先级高于 HKLM，
    # 不需要管理员权限。CTF\TIP 那几项以前已经写进 HKLM 了，不用动。
    $prev = (Get-ItemProperty $hkcuClsid -Name '(default)' -ErrorAction SilentlyContinue).'(default)'
    New-Item -Path $hkcuClsid -Force | Out-Null
    Set-ItemProperty -Path $hkcuClsid -Name '(default)' -Value $target
    if ($prev) { Write-Host "      原指向: $prev" }
    Write-Host "[3/4] 已注册 $target" -ForegroundColor Green
    Write-Host "      回滚：Set-ItemProperty '$hkcuClsid' -Name '(default)' -Value '$prev'"
} else {
    Write-Host "[2/4] 跳过部署（加 -Deploy 才会覆盖并注册）" -ForegroundColor Yellow
    Write-Host "[3/4] 跳过注册" -ForegroundColor Yellow
}

# ---------- 2.5 重建 Rime 部署（改了 rime_data 下的配置才需要）----------
# 关键：只改 rime_data\*.yaml 而不重建，输入法仍会按旧的编译产物跑，
# 表现就是「改了配置完全没反应」。参数顺序必须是
#     --build <用户目录> <共享目录>
# 反了会返回 0 但静默什么都不做。
if ($RebuildRime) {
    $dep = Join-Path $root 'rime_dl\rime\dist\bin\rime_deployer.exe'
    if (-not (Test-Path $dep)) { throw "找不到 rime_deployer.exe：$dep" }
    # rime_deployer 的依赖在 dist\lib 下，不在自己旁边
    $env:PATH = (Join-Path $root 'rime_dl\rime\dist\lib') + ';' + $env:PATH
    & $dep --build (Join-Path $root 'rime_user') (Join-Path $root 'rime_data')
    if ($LASTEXITCODE -ne 0) { throw "Rime 重建失败，exit $LASTEXITCODE" }
    Write-Host "[2.5] 已重建 Rime 部署（改了 rime_data 配置后必须做）" -ForegroundColor Green
}

# ---------- 3. 调试日志开关 ----------
if ($Debug -and $NoDebug) { throw "-Debug 和 -NoDebug 不能同时用" }
if ($Debug) {
    New-Item -ItemType File -Force -Path $debugFlag | Out-Null
    Write-Host "[4/4] 已打开调试日志 -> $logFile" -ForegroundColor Yellow
    Write-Host "      日志含你打的字，排查完跑 .\build.ps1 -NoDebug 关掉"
} elseif ($NoDebug) {
    Remove-Item $debugFlag -Force -ErrorAction SilentlyContinue
    Write-Host "[4/4] 已关闭调试日志" -ForegroundColor Green
} else {
    $on = Test-Path $debugFlag
    Write-Host "[4/4] 调试日志当前：$(if($on){'开'}else{'关'})" -ForegroundColor Gray
}

if ($Deploy) {
    Write-Host ""
    Write-Host "下一步：重启一下正在用输入法的程序（记事本 / 聊天软件），" -ForegroundColor Cyan
    Write-Host "或者直接注销一次，然后就能用新版本了。" -ForegroundColor Cyan
}
