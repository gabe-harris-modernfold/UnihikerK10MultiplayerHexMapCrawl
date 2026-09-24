
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$s.SelectVoice('Microsoft David Desktop')
$s.Rate = 0
$fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000,
    [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
    [System.Speech.AudioFormat.AudioChannel]::Mono)
foreach ($line in [System.IO.File]::ReadAllLines('C:\SourceCode\Esp32HexMapCrawl\scripts\lpc\out\tts\jobs.tsv', [System.Text.Encoding]::UTF8)) {
    if (-not $line) { continue }
    $path, $text = $line -split "`t", 2
    $part = $path + '.part'
    $s.SetOutputToWaveFile($part, $fmt)
    $s.Speak($text)
    $s.SetOutputToNull()
    Move-Item -LiteralPath $part -Destination $path -Force
}
$s.Dispose()
