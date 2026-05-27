using System.IO;
using System.Windows;
using AudioWavePlayer.Services;

namespace AudioWavePlayer;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);

        var startupFilePath = e.Args.FirstOrDefault(path =>
            File.Exists(path) && FfmpegService.IsSupported(path));

        var mainWindow = new MainWindow(startupFilePath);
        MainWindow = mainWindow;
        mainWindow.Show();
    }
}
