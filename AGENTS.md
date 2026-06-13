# Agent Rules

- Before every build, stop any running `AudioWave.Player.exe` process whose executable path is under `C:\Users\ac\Documents\audio_player\x64\Debug`.
- Always build into the same output directory: `C:\Users\ac\Documents\audio_player\x64\Debug`.
- Use MSBuild from Visual Studio 18 Insiders:
  `E:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe`.
- Maintain a lightweight project context cache: accumulate useful facts about architecture, build/test workflows, known pitfalls, and recent investigation results while working, then review that context before starting new tasks to prepare proactively. Do not store secrets or unrelated personal data.
- Preferred build command from the repository root:

```powershell
& 'E:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe' .\AudioWave.Player.sln /p:Configuration=Debug /p:Platform=x64
```
