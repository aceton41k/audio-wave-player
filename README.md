# AudioWave Player

Minimal native Windows audio player prototype inspired by waveform-first players.

<img  width="70%" alt="image" src="https://github.com/user-attachments/assets/0782a678-d780-4b49-b83d-8d98300a31a4" />

## Features

- Native WPF UI for Windows.
- Portable-friendly layout.
- Large seekable waveform.
- Playback controls: open, play/pause, stop, restart, volume.
- Dark and light themes.
- Format line with sample rate, channels, lossless/lossy hint, approximate bitrate, extension.
- Input formats through FFmpeg: WAV, MP3, M4A, FLAC.

## Requirements

- .NET 8 SDK for development.
- `ffmpeg.exe` for decoding.

For portable use, put FFmpeg here after publishing:

```text
AudioWavePlayer.exe
ffmpeg/
  ffmpeg.exe
```

The app also searches for `ffmpeg.exe` next to the executable and in `PATH`.
Decoded waveform/playback cache is stored in `cache/decoded` near the executable when that folder is writable, with `%LOCALAPPDATA%/AudioWavePlayer` as a fallback.

## Development

```powershell
dotnet restore
dotnet build
dotnet run
```

## Portable Publish

```powershell
dotnet publish -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true
```

Copy `ffmpeg/ffmpeg.exe` into the publish directory.

## License Notes

The app code is intended for an open source repository. `NAudio` is MIT licensed. FFmpeg licensing depends on the distributed build and enabled codecs, so choose an FFmpeg build whose license terms match the project distribution model.
