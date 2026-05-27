# AudioWave Player Release Notes

## Unreleased

### Added

- Added previous and next track buttons next to play/pause.
- Added playback order switching: sequential playback, repeat current file, and random order.
- Added Windows taskbar thumbnail controls for previous, play/pause, and next.
- Added taskbar progress display while audio is playing and the player window is inactive.
- Added restore-on-drop behavior: dropping an audio file on the taskbar icon restores a minimized player window and opens the file.
- Added the new AudioWave application logo and Windows application icon.
- Added file extensions in the file browser.
- Added a horizontal resize handle between the waveform area and browser area, plus a centered vertical resize handle between the folder tree and file list.
- Added mouse wheel volume changes when hovering the volume control.
- Added speaker mute/restore button that remembers the previous volume level.

### Changed

- Improved waveform rendering performance by reducing peak collection work and caching channel-aware waveform data.
- Waveform rendering now uses the source channel count: mono draws one waveform, stereo draws two when there is enough height, and multichannel files draw per-channel waves.
- Stereo and multichannel waveforms collapse to the left channel when the waveform area is too short.
- Updated light theme selection colors to use readable gray highlighting for files and folders.
- Refined dark theme column hover styling for better contrast.
- Reduced spacing around waveform and browser panels.
- Updated scrollbars to a thinner Windows-style appearance with rounded handles and line scroll buttons.
- Made file list scrolling smoother instead of snapping row-by-row.
- Lightened scrollbar and column divider colors in the light theme.
- Removed the total duration label from the volume area.
- Hid RMS/level meters when no audio file is selected.

### Fixed

- Fixed tree folder hover so only the hovered folder row is highlighted, instead of also highlighting parent path folders.
- Fixed selected file text readability in the light theme.
- Fixed startup failure caused by icon resource loading.
- Fixed waveform layout behavior for stereo files at small heights.
