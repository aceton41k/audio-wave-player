# AudioWave Player Release Notes

## Unreleased

### Changed

- Rewrote the application from WPF/.NET to native C++20 with Win32, GDI+ and Media Foundation.
- Replaced NAudio playback with `IMFPMediaPlayer`.
- Replaced FFmpeg-based decoding and waveform analysis with `IMFSourceReader`.
- Replaced XAML UI with custom Win32 drawing and native controls.

### Added

- Visual Studio C++ solution and project files.
- Drag-and-drop file opening.
- Seekable waveform rendering.
- Folder audio list, previous/next playback, repeat-one, shuffle, volume, mute and RMS meters.

### Removed

- .NET 8, WPF, NAudio and FFmpeg runtime requirements.
