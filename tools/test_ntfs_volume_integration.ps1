param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$QtRoot,
    [string]$Python = 'python'
)

$ErrorActionPreference = 'Stop'
$runner = Join-Path $PSScriptRoot 'test_native_ntfs_index.py'
if (-not (Test-Path -LiteralPath $runner -PathType Leaf)) {
    throw 'The native NTFS test runner is missing.'
}

# Only a newly created, uniquely named VHD is ever initialized or formatted.
# No caller-supplied drive letter or physical disk number is accepted.
$runToken = [Guid]::NewGuid().ToString('N')
$scratchDirectory = Join-Path ([IO.Path]::GetTempPath()) "ksword-ntfs-$runToken"
$imagePath = Join-Path $scratchDirectory 'scratch.vhd'
$diskpartScript = Join-Path $scratchDirectory 'create-vhd.txt'
[UInt64]$scratchBytes = 128MB
$volumeLabel = "KSwordNtfs-$($runToken.Substring(0, 8))"
$mounted = $false
$completed = $false
New-Item -ItemType Directory -Path $scratchDirectory -ErrorAction Stop | Out-Null

try {
    if (Test-Path -LiteralPath $imagePath) { throw 'Refusing to reuse an existing image.' }
    [IO.File]::WriteAllLines($diskpartScript, @(
        "create vdisk file=`"$imagePath`" maximum=128 type=expandable",
        'exit'
    ), [Text.Encoding]::ASCII)
    & "$env:SystemRoot\System32\diskpart.exe" /s $diskpartScript
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $imagePath -PathType Leaf)) {
        throw 'Could not create the isolated NTFS test image.'
    }

    $image = Mount-DiskImage -ImagePath $imagePath -StorageType VHD -Access ReadWrite -NoDriveLetter -PassThru
    $mounted = $true
    $associatedDisks = @($image | Get-Disk)
    if ($associatedDisks.Count -ne 1) { throw 'The test image must map to exactly one disk.' }
    $disk = $associatedDisks[0]
    $expectedDevice = "\\.\PHYSICALDRIVE$($disk.Number)"
    if (-not [String]::Equals($image.DevicePath, $expectedDevice, [StringComparison]::OrdinalIgnoreCase) -or
        $disk.IsBoot -or $disk.IsSystem -or $disk.PartitionStyle -ne 'RAW' -or
        [UInt64]$disk.Size -ne $scratchBytes -or [int]$disk.BusType -notin @(14, 15)) {
        throw 'The new image did not pass isolated virtual-disk identity checks.'
    }

    if ($disk.IsOffline) { $disk | Set-Disk -IsOffline $false }
    if ($disk.IsReadOnly) { $disk | Set-Disk -IsReadOnly $false }
    $initialized = $disk | Initialize-Disk -PartitionStyle MBR -PassThru
    $partition = $initialized | New-Partition -UseMaximumSize -AssignDriveLetter
    $volume = $partition | Format-Volume -FileSystem NTFS -NewFileSystemLabel $volumeLabel `
        -AllocationUnitSize 4096 -Force -Confirm:$false
    if (-not $partition.DriveLetter) { throw 'The scratch partition has no drive letter.' }
    $owner = Get-Partition -DriveLetter $partition.DriveLetter | Get-Disk
    if ($owner.Number -ne $disk.Number -or $volume.FileSystemLabel -ne $volumeLabel -or
        $volume.FileSystem -ne 'NTFS') {
        throw 'The scratch volume did not retain its verified image identity.'
    }

    $volumeName = "$($partition.DriveLetter):"
    $volumeRoot = "$volumeName\"
    $workDirectory = Join-Path $volumeRoot "fixture-$runToken"
    New-Item -ItemType Directory -Path $workDirectory | Out-Null
    # The production indexer only reads journals. This creates one solely on the
    # owned, disposable fixture volume so the real USN replay can be exercised.
    & "$env:SystemRoot\System32\fsutil.exe" usn createjournal m=8388608 a=1048576 $volumeName
    if ($LASTEXITCODE -ne 0) { throw 'Could not enable the scratch volume journal.' }

    Write-Host "NTFS_SCRATCH_VOLUME=$volumeRoot IMAGE=$imagePath"
    & $Python -B $runner --qt-root $QtRoot --volume-root $volumeRoot --work-dir $workDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Native NTFS index integration tests failed.' }
    $completed = $true
}
finally {
    $detachFailure = $null
    if (Test-Path -LiteralPath $imagePath) {
        try {
            $state = Get-DiskImage -ImagePath $imagePath -ErrorAction Stop
            if ($state.Attached) {
                Dismount-DiskImage -ImagePath $imagePath -ErrorAction Stop
                if ((Get-DiskImage -ImagePath $imagePath).Attached) {
                    throw 'The scratch image is still attached.'
                }
            }
        }
        catch {
            if ($mounted) { $detachFailure = $_ }
        }
    }
    if ($detachFailure) {
        Write-Warning "Could not detach the owned scratch image: $detachFailure"
        if ($completed) { throw 'Scratch-volume cleanup failed.' }
    }
    else {
        Remove-Item -LiteralPath $scratchDirectory -Recurse -Force -ErrorAction Stop
    }
}
