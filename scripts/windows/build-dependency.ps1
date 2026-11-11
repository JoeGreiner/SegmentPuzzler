param(
    [Parameter(Mandatory)]
    [ValidateSet('itk', 'vtk')]
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

$Root = [System.IO.Path]::GetFullPath($Root)
$logs = Join-Path $Root 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null
Start-Transcript -Path "$logs/$Stage.log" -Force | Out-Null

try {
    if ($Stage -eq 'vtk') {
        if (-not $QtRoot -or -not (Test-Path "$QtRoot/bin/qmake.exe")) {
            throw 'Set QT_ROOT_DIR or pass -QtRoot with the Qt 6 MSVC installation prefix.'
        }
        $env:PATH = "$QtRoot/bin;$env:PATH"
    }
    $dependencies = Get-Content "$PSScriptRoot/../ci/dependencies.json" -Raw | ConvertFrom-Json
    $dependency = $dependencies.$Stage
    $source = Join-Path $Root "src/$Stage"
    $build = Join-Path $Root "build/$Stage"
    $install = Join-Path $Root "install/$Stage"
    New-Item -ItemType Directory -Force $source, $build, $install | Out-Null

    Write-Host "Building $Stage $($dependency.version), commit $($dependency.commit)"
    $archive = Join-Path $Root "$Stage.tar.gz"
    Invoke-WebRequest "https://github.com/$($dependency.repository)/archive/$($dependency.commit).tar.gz" -OutFile $archive
    Invoke-Checked tar @('-xzf', $archive, '--strip-components=1', '-C', $source)

    $configure = @(
        '-S', $source, '-B', $build, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_INSTALL_PREFIX=$install",
        '-DCMAKE_CXX_STANDARD=17', '-UCMAKE_CXX_FLAGS',
        '-DCMAKE_CXX_FLAGS_INIT=/bigobj /utf-8',
        '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW',
        '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL',
        '-DBUILD_TESTING=OFF'
    )
    if ($Stage -eq 'itk') {
        $configure += @(
            '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_EXAMPLES=OFF',
            '-DITK_BUILD_DEFAULT_MODULES=ON', '-DITK_WRAP_PYTHON=OFF',
            '-DITK_USE_SYSTEM_LIBRARIES=OFF'
        )
    } else {
        $configure += @(
            '-DBUILD_SHARED_LIBS=ON', '-DVTK_BUILD_TESTING=OFF',
            '-DVTK_BUILD_EXAMPLES=OFF', '-DVTK_WRAP_PYTHON=OFF',
            '-DVTK_WRAP_JAVA=OFF', '-DVTK_QT_VERSION=6',
            "-DCMAKE_PREFIX_PATH=$QtRoot", '-DVTK_SMP_IMPLEMENTATION_TYPE=STDThread',
            '-DVTK_GROUP_ENABLE_MPI=NO'
        )
        foreach ($group in @('StandAlone', 'Rendering', 'Imaging', 'Qt', 'Views', 'Web')) {
            $configure += "-DVTK_GROUP_ENABLE_${group}=DONT_WANT"
        }
        foreach ($module in @(
            'CommonCore', 'CommonDataModel', 'FiltersCore', 'FiltersGeneral',
            'FiltersSources', 'ImagingCore', 'ImagingGeneral', 'RenderingCore',
            'RenderingOpenGL2', 'RenderingAnnotation', 'InteractionStyle',
            'InteractionWidgets', 'GUISupportQt'
        )) {
            $configure += "-DVTK_MODULE_ENABLE_VTK_${module}=YES"
        }
    }
    Invoke-Checked cmake $configure
    Invoke-Checked cmake @('--build', $build, '--parallel', "$Parallel")
    Invoke-Checked cmake @('--install', $build)
} finally {
    Stop-Transcript | Out-Null
}
