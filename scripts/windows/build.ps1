param(
    [Parameter(Mandatory)]
    [ValidateSet('app', 'package', 'smoke')]
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
    & $Program @CommandArguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Program failed with exit code $LASTEXITCODE"
    }
}

$repository = (Resolve-Path "$PSScriptRoot/../..").Path
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
            '-S', $repository, '-B', $appBuild, '-G', 'Ninja',
            '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_PREFIX_PATH=$prefixes",
            '-DCMAKE_CXX_FLAGS=/bigobj /utf-8',
            '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW',
            '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL',
            '-DSEGMENT_PUZZLER_QT_MAJOR=6', '-DDEPLOY=ON',
            '-DBUILD_TESTING=OFF', '-DBUILD_BENCHMARKS=OFF', '-DUSE_OMP=ON'
        )
        Invoke-Checked cmake @('--build', $appBuild, '--target', 'SegmentPuzzler', '--parallel', "$Parallel")
    }

    if ($Stage -eq 'package') {
        New-Item -ItemType Directory -Force $packages | Out-Null
        Invoke-Checked cpack @('--config', "$appBuild/CPackConfig.cmake", '-C', 'Release', '-G', 'ZIP', '-B', $packages)
    }

    if ($Stage -eq 'smoke') {
        $archives = @(Get-ChildItem $packages -Filter '*.zip')
        if ($archives.Count -ne 1) {
            throw "Expected one package ZIP in $packages; found $($archives.Count)."
        }
        $unpacked = Join-Path $Root "smoke/$([guid]::NewGuid().ToString('N'))"
        Expand-Archive $archives[0].FullName -DestinationPath $unpacked
        $executables = @(Get-ChildItem $unpacked -Recurse -Filter 'SegmentPuzzler.exe')
        if ($executables.Count -ne 1) {
            throw 'The ZIP must contain exactly one SegmentPuzzler.exe.'
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
            $process = Start-Process -FilePath $executables[0].FullName `
                -ArgumentList '--log-level', 'windows-ci-smoke' `
                -WorkingDirectory $executables[0].DirectoryName `
                -RedirectStandardOutput "$logs/startup-stdout.log" `
                -RedirectStandardError "$logs/startup-stderr.log" -PassThru
            if (-not $process.WaitForExit(30000)) {
                throw 'The packaged executable did not finish its startup check within 30 seconds.'
            }
            $process.WaitForExit()
            $stderr = Get-Content "$logs/startup-stderr.log" -Raw
            if ($process.ExitCode -ne 1 -or $stderr -notmatch 'Invalid value for --log-level: windows-ci-smoke') {
                throw "Packaged executable failed the startup check (exit $($process.ExitCode)): $stderr"
            }
            Write-Host 'Packaged executable loaded its DLLs and Qt platform plugin successfully.'
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
} finally {
    Stop-Transcript | Out-Null
}
