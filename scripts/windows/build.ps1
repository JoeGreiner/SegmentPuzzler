param(
    [Parameter(Mandatory)]
    [ValidateSet('app', 'package', 'check-package', 'check-installer')]
    [string] $Stage,
    [string] $Root = 'D:\spci',
    [string] $QtRoot = $env:QT_ROOT_DIR,
    [ValidateRange(1, 64)]
    [int] $Parallel = 3
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
Set-StrictMode -Version Latest

if (-not $IsWindows) {
    throw 'Run this script with PowerShell 7 in an x64 MSVC developer environment on Windows.'
}

function Invoke-Checked {
    param([string] $Program, [string[]] $CommandArguments)
    & $Program @CommandArguments 2>&1 | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}

function Test-ApplicationStartup {
    param([string] $Executable, [string] $LogPrefix)
    $application = Get-Item -LiteralPath $Executable
    $tiffPlugin = Join-Path $application.DirectoryName 'imageformats/qtiff.dll'
    if (-not (Test-Path -LiteralPath $tiffPlugin -PathType Leaf)) {
        throw 'Packaged application is missing the Qt TIFF image plugin.'
    }
    # An invalid log level exits after QApplication initialisation, before creating the 3D window.
    $environmentNames = @('PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORM', 'QML2_IMPORT_PATH')
    $savedEnvironment = @{}
    foreach ($name in $environmentNames) {
        $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    $process = $null
    try {
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
        $env:QT_PLUGIN_PATH = $null
        $env:QT_QPA_PLATFORM_PLUGIN_PATH = $null
        $env:QML2_IMPORT_PATH = $null
        $env:QT_QPA_PLATFORM = 'windows'
        $process = Start-Process -FilePath $application.FullName `
            -ArgumentList '--log-level', 'package-check' `
            -WorkingDirectory $application.DirectoryName `
            -RedirectStandardOutput "$LogPrefix-stdout.log" `
            -RedirectStandardError "$LogPrefix-stderr.log" -PassThru
        if (-not $process.WaitForExit(30000)) {
            throw 'The packaged executable did not finish its startup check within 30 seconds.'
        }
        $process.WaitForExit()
        $stderr = Get-Content "$LogPrefix-stderr.log" -Raw
        if ($process.ExitCode -ne 1 -or $stderr -notmatch 'Invalid value for --log-level: package-check') {
            throw "Packaged executable failed the startup check (exit $($process.ExitCode)): $stderr"
        }
        Write-Host "Startup check passed: $($application.FullName)"
    } finally {
        if ($null -ne $process -and -not $process.HasExited) {
            $process.Kill()
            $process.WaitForExit()
        }
        foreach ($name in $environmentNames) {
            [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
        }
    }
}

function Invoke-Installer {
    param([string] $Executable, [string] $Arguments)
    $process = Start-Process -FilePath $Executable -ArgumentList $Arguments -PassThru
    try {
        if (-not $process.WaitForExit(300000)) {
            throw "Installer timed out: $Executable"
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) {
            throw "Installer failed with exit code $($process.ExitCode): $Executable"
        }
    } finally {
        if (-not $process.HasExited) {
            $process.Kill()
            $process.WaitForExit()
        }
    }
}

$repository = (Resolve-Path "$PSScriptRoot/../..").Path
$appSource = if ($env:APP_SOURCE_DIR) { $env:APP_SOURCE_DIR } else { $repository }
$Root = [System.IO.Path]::GetFullPath($Root)
$logs = Join-Path $Root 'logs'
$appBuild = Join-Path $Root 'build/app'
$packages = Join-Path $Root 'packages'
New-Item -ItemType Directory -Force $logs | Out-Null
Start-Transcript -Path "$logs/$Stage.log" -Force | Out-Null

try {
    if ($Stage -in @('app', 'package')) {
        if (-not $QtRoot -or -not (Test-Path "$QtRoot/bin/qmake.exe")) {
            throw 'Set QT_ROOT_DIR or pass -QtRoot with the Qt 6 MSVC installation prefix.'
        }
        $env:PATH = "$QtRoot/bin;$env:PATH"
    }

    if ($Stage -eq 'app') {
        $prefixes = "$QtRoot;$Root/install/itk;$Root/install/vtk"
        Invoke-Checked cmake @(
            '-S', $appSource, '-B', $appBuild, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_PREFIX_PATH=$prefixes",
            '-UCMAKE_CXX_FLAGS', '-DCMAKE_CXX_FLAGS_INIT=/bigobj /utf-8',
            '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW',
            '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL',
            '-DSEGMENT_PUZZLER_QT_MAJOR=6', '-DDEPLOY=ON',
            '-DBUILD_TESTING=OFF', '-DBUILD_BENCHMARKS=OFF', '-DUSE_OMP=ON'
        )
        Invoke-Checked cmake @('--build', $appBuild, '--target', 'SegmentPuzzler', '--parallel', "$Parallel")
    }

    if ($Stage -eq 'package') {
        New-Item -ItemType Directory -Force $packages | Out-Null
        Invoke-Checked cpack @('--config', "$appBuild/CPackConfig.cmake", '-C', 'Release', '-G', 'NSIS;ZIP', '-B', $packages)
    }

    if ($Stage -eq 'check-package') {
        $archives = @(Get-ChildItem $packages -Filter '*.zip')
        if ($archives.Count -ne 1) {
            throw "Expected one package ZIP in $packages; found $($archives.Count)."
        }
        $unpacked = Join-Path $Root "package-check/$([guid]::NewGuid().ToString('N'))"
        Expand-Archive $archives[0].FullName -DestinationPath $unpacked
        $executables = @(Get-ChildItem $unpacked -Recurse -Filter 'SegmentPuzzler.exe')
        if ($executables.Count -ne 1) {
            throw 'The ZIP must contain exactly one SegmentPuzzler.exe.'
        }

        Test-ApplicationStartup $executables[0].FullName "$logs/startup"
    }

    if ($Stage -eq 'check-installer') {
        $installers = @(Get-ChildItem $packages -Filter '*.exe')
        if ($installers.Count -ne 1) {
            throw "Expected one NSIS installer in $packages; found $($installers.Count)."
        }
        $installDirectory = Join-Path $Root "package-check/$([guid]::NewGuid().ToString('N'))/SegmentPuzzler Test"
        $executable = Join-Path $installDirectory 'bin/SegmentPuzzler.exe'
        $uninstaller = Join-Path $installDirectory 'Uninstall.exe'
        try {
            Write-Host "Installing $($installers[0].Name) into $installDirectory"
            # NSIS requires /D last, without quotes, even when the path contains spaces.
            Invoke-Installer $installers[0].FullName "/S /D=$installDirectory"
            if (-not (Test-Path -LiteralPath $uninstaller)) {
                throw 'The installer did not create Uninstall.exe.'
            }
            Test-ApplicationStartup $executable "$logs/installed-startup"
        } finally {
            if (Test-Path -LiteralPath $uninstaller) {
                # _?= keeps the uninstaller in this process so we can wait for it.
                Invoke-Installer $uninstaller "/S _?=$installDirectory"
                if (Test-Path -LiteralPath $executable) {
                    throw 'The uninstaller left SegmentPuzzler.exe installed.'
                }
                Write-Host 'Uninstall check passed.'
            }
        }
    }
} finally {
    Stop-Transcript | Out-Null
}
