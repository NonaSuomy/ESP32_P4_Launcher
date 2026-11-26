param([string]$src, [string]$dst, [string]$kind)
# Windows Media Foundation transcoding (WinRT): wav/avi -> mp3 / m4a (AAC) / mp4 (H.264 + AAC)
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Media.Transcoding.MediaTranscoder, Windows.Media.Transcoding, ContentType = WindowsRuntime]
$null = [Windows.Media.MediaProperties.MediaEncodingProfile, Windows.Media.MediaProperties, ContentType = WindowsRuntime]

$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, [Type]$t) {
    $m = $asTaskGeneric.MakeGenericMethod($t)
    $task = $m.Invoke($null, @($op))
    $task.Wait() | Out-Null
    $task.Result
}
$asTaskAction = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncActionWithProgress`1' })[0]

$in = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($src)) ([Windows.Storage.StorageFile])
$dir = Await ([Windows.Storage.StorageFolder]::GetFolderFromPathAsync((Split-Path $dst))) ([Windows.Storage.StorageFolder])
$out = Await ($dir.CreateFileAsync((Split-Path $dst -Leaf), [Windows.Storage.CreationCollisionOption]::ReplaceExisting)) ([Windows.Storage.StorageFile])

$Q = [Windows.Media.MediaProperties.AudioEncodingQuality]::Medium
switch ($kind) {
    'mp3' { $p = [Windows.Media.MediaProperties.MediaEncodingProfile]::CreateMp3($Q) }
    'm4a' { $p = [Windows.Media.MediaProperties.MediaEncodingProfile]::CreateM4a($Q) }
    'mp4' { $p = [Windows.Media.MediaProperties.MediaEncodingProfile]::CreateMp4([Windows.Media.MediaProperties.VideoEncodingQuality]::Vga)
            $p.Video.Width = 640; $p.Video.Height = 360; $p.Video.Bitrate = 1200000
            $p.Video.FrameRate.Numerator = 25; $p.Video.FrameRate.Denominator = 1 }
}
$tc = New-Object Windows.Media.Transcoding.MediaTranscoder
$prep = Await ($tc.PrepareFileTranscodeAsync($in, $out, $p)) ([Windows.Media.Transcoding.PrepareTranscodeResult])
if (-not $prep.CanTranscode) { "cannot transcode: $($prep.FailureReason)"; exit 1 }
$act = $prep.TranscodeAsync()
$m = $asTaskAction.MakeGenericMethod([double])
$t = $m.Invoke($null, @($act))
$t.Wait() | Out-Null
"ok $((Get-Item $dst).Length) bytes"
