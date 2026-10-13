param(
    [Parameter(Mandatory)]
    [ValidateSet('Capture', 'Restore')]
    [string] $Mode,
    [Parameter(Mandatory)]
    [string] $SourceRoot
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $SourceRoot).Path
$manifest = Join-Path $root 'out/source-timestamps.json'

function Get-SourcePath([string] $RelativePath) {
    $path = [IO.Path]::GetFullPath((Join-Path $root $RelativePath))
    if (!$path.StartsWith($root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Source timestamp path is outside the checkout'
    }
    return $path
}

if ($Mode -eq 'Capture') {
    $tracked = & git -C $root -c core.quotepath=false ls-files --recurse-submodules
    if ($LASTEXITCODE -ne 0) { throw 'Could not list tracked sources' }
    $entries = @(foreach ($relative in $tracked) {
        $path = Get-SourcePath $relative
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $file = Get-Item -LiteralPath $path
        [PSCustomObject]@{
            path = $relative
            hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
            ticks = $file.LastWriteTimeUtc.Ticks
        }
    })
    [IO.File]::WriteAllText($manifest, (ConvertTo-Json -InputObject $entries -Depth 3), [Text.UTF8Encoding]::new($false))
    Write-Host "Saved timestamps for $($entries.Count) source files"
} elseif (Test-Path -LiteralPath $manifest) {
    $restored = 0
    foreach ($entry in (Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json)) {
        $path = Get-SourcePath $entry.path
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.hash) { continue }
        (Get-Item -LiteralPath $path).LastWriteTimeUtc = [DateTime]::new([long]$entry.ticks, [DateTimeKind]::Utc)
        $restored++
    }
    Write-Host "Restored timestamps for $restored unchanged source files"
}
