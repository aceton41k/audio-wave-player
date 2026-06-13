# AudioWave Player

Native Windows audio player built with C++20, Win32, GDI+ and Media Foundation.

## Features

- Pure Win32 desktop UI.
- Media Foundation playback for WAV, MP3, M4A/MP4, AAC, WMA and FLAC when the installed Windows codecs support them.
- Seekable waveform generated through `IMFSourceReader`.
- Playback controls: open, previous, play/pause, stop, next, restart, volume and mute.
- Sequential, repeat-one and shuffle playback.
- Dark and light themes.
- Drag-and-drop file opening.
- Folder audio list based on the currently opened file.
- Live RMS meters.

## Requirements

- Windows 10 or newer.
- Visual Studio 2022 with the Desktop development with C++ workload.
- Windows 10/11 SDK.

No .NET runtime, NAudio package or FFmpeg executable is required.

## Build

Open `AudioWave.Player.sln` in Visual Studio and build `Release|x64`.

Command line from a Visual Studio Developer PowerShell:

```powershell
msbuild .\AudioWave.Player.sln /p:Configuration=Release /p:Platform=x64
```

## Notes

Media Foundation support depends on codecs installed with Windows. Unsupported or DRM-protected files will fail to open instead of being transcoded by FFmpeg.
