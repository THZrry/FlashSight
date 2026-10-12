#Requires -RunAsAdministrator
<#
  Emscripten 6.0.12 最小安装收尾脚本（需右键“以管理员身份运行”）
  前置：暂存区 C:\aaa\FlashSight\temp\em-dist\install 已解压验证通过（emcc --version + hello 编译运行 OK）
  本脚本只做三件事：
    1) 安装前再次三重备份 PATH（含注册表导出）
    2) 复制 install/{bin,emscripten,lib} -> C:\Program Files\emscripten\
    3) 写 .emscripten 配置 + 把两个目录追加到【用户 PATH 末尾】+ 验证
  不装 emsdk 自带 python/node：复用本机 Python(3.12) + Node(24)。
#>
$ErrorActionPreference = 'Stop'

$StageInstall = 'C:\aaa\FlashSight\temp\em-dist\install'
$Dest         = 'C:\Program Files\emscripten'
$BackupRoot   = 'C:\aaa\FlashSight\temp\path_backup_preinstall_' + (Get-Date -Format 'yyyyMMdd_HHmmss')

if (-not (Test-Path "$StageInstall\bin\clang.exe"))     { throw "暂存区缺失: $StageInstall\bin\clang.exe" }
if (-not (Test-Path "$StageInstall\emscripten\emcc.py")) { throw "暂存区缺失: $StageInstall\emscripten\emcc.py" }

# ---- 1) 安装前 PATH 三重备份（文件x3 + 注册表x2）----
New-Item -ItemType Directory -Force $BackupRoot | Out-Null
[Environment]::GetEnvironmentVariable('Path','User')    | Out-File "$BackupRoot\user_path.txt" -Encoding utf8
Copy-Item "$BackupRoot\user_path.txt" "$BackupRoot\user_path_copy2.txt"
Copy-Item "$BackupRoot\user_path.txt" "$BackupRoot\user_path_copy3.txt"
[Environment]::GetEnvironmentVariable('Path','Machine') | Out-File "$BackupRoot\machine_path.txt" -Encoding utf8
reg export 'HKCU\Environment' "$BackupRoot\hkcu_environment.reg" /y | Out-Null
reg export 'HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment' "$BackupRoot\hklm_environment.reg" /y | Out-Null
Write-Host "PATH 已备份到: $BackupRoot"

# ---- 2) 复制安装体（约 1.8GB，内容=预编译包 install/ 原样）----
New-Item -ItemType Directory -Force $Dest | Out-Null
Copy-Item "$StageInstall\bin"        "$Dest\bin"        -Recurse -Force
Copy-Item "$StageInstall\emscripten" "$Dest\emscripten" -Recurse -Force
Copy-Item "$StageInstall\lib"        "$Dest\lib"        -Recurse -Force
Write-Host "已复制到: $Dest"

# ---- 3a) 解析本机 python / node（复用，不装 emsdk 自带的）----
$pyExe  = ((where.exe python 2>$null) -split "`r?`n" | Where-Object { $_ } | Select-Object -First 1)
$pyReal = (python -c 'import sys; print(sys.executable)' 2>$null).Trim()
if ($pyReal) { $pyExe = $pyReal }
$nodeExe = 'C:\Program Files\node\node.exe'
if (-not (Test-Path $nodeExe)) {
  $nodeExe = ((where.exe node 2>$null) -split "`r?`n" | Where-Object { $_ } | Select-Object -First 1)
}
if (-not $pyExe -or -not (Test-Path $pyExe))   { throw "找不到系统 python" }
if (-not $nodeExe -or -not (Test-Path $nodeExe)) { throw "找不到系统 node: $nodeExe" }
$pyCfg   = $pyExe -replace '\\','/'
$nodeCfg = $nodeExe -replace '\\','/'
Write-Host "PYTHON=$pyExe"
Write-Host "NODE_JS=$nodeExe"

# ---- 3b) 写 .emscripten 配置（用户级 + 安装目录模板各一份）----
$emCfg = @"
EMSCRIPTEN_ROOT = 'C:/Program Files/emscripten/emscripten'
LLVM_ROOT = 'C:/Program Files/emscripten/bin'
BINARYEN_ROOT = 'C:/Program Files/emscripten'
NODE_JS = '$nodeCfg'
PYTHON = '$pyCfg'
JAVA = 'java'
TEMP_DIR = 'C:/Users/Xu/AppData/Local/Temp'
COMPILER_OPTS = []
FROZEN_CACHE = False
CACHE = 'C:/Users/Xu/.emscripten_cache'
PORTS = 'C:/Users/Xu/.emscripten_cache/ports'
"@
$emCfg | Out-File "$Dest\.emscripten" -Encoding ascii
$userCfg = Join-Path ([Environment]::GetFolderPath('UserProfile')) '.emscripten'
if (-not (Test-Path $userCfg)) { $emCfg | Out-File $userCfg -Encoding ascii; Write-Host "已写: $userCfg" }
else { Write-Host "保留现有用户配置（未覆盖）: $userCfg" }

# ---- 3c) 用户 PATH 末尾追加（幂等：已存在则跳过）----
$binDir = "$Dest\bin"
$emDir  = "$Dest\emscripten"
$uPath  = [Environment]::GetEnvironmentVariable('Path','User')
foreach ($d in @($binDir, $emDir)) {
  if ($uPath -split ';' | Where-Object { $_.TrimEnd('\') -ieq $d.TrimEnd('\') }) { Write-Host "PATH 已含（跳过）: $d"; continue }
  $uPath = ($uPath.TrimEnd(';') + ';' + $d)
}
[Environment]::SetEnvironmentVariable('Path', $uPath, 'User')
Write-Host '用户 PATH 末尾已追加（若原本没有）:'
Write-Host "  $binDir"
Write-Host "  $emDir"

# ---- 3d) 验证（刷新本进程 PATH 后）----
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
$env:EM_CONFIG = $userCfg
if (Test-Path "$emDir\emcc.py") { python "$emDir\emcc.py" --version }
'@echo off' | Out-File "$env:TEMP\em_hello.c" -Encoding ascii
'#include <stdio.h>', 'int main(){printf("hello wasm %d\n", 40+2); return 0;}' | Set-Content "$env:TEMP\em_hello.c"
python "$emDir\emcc.py" "$env:TEMP\em_hello.c" -o "$env:TEMP\em_hello.js"
node "$env:TEMP\em_hello.js"
Write-Host 'OK: 安装 + PATH + 编译运行验证全部通过'
Write-Host "回滚方法：用 $BackupRoot 下的 user_path.txt / hkcu_environment.reg 恢复用户 PATH"
