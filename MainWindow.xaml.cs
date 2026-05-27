using System.Diagnostics;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;
using System.Text;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shell;
using System.Windows.Threading;
using AudioWavePlayer.Services;
using Microsoft.Win32;
using NAudio.Wave;

namespace AudioWavePlayer;

public partial class MainWindow : Window, INotifyPropertyChanged
{
    private const int WmDropFiles = 0x0233;
    private const uint DragQueryAllFiles = 0xFFFFFFFF;

    private FfmpegService? _ffmpeg;
    private HwndSource? _hwndSource;
    private ThumbButtonInfo? _taskbarPlayPauseButton;
    private PlaybackOrderMode _playbackOrderMode = PlaybackOrderMode.Sequential;
    private readonly Stopwatch _playbackClock = new();
    private WaveOutEvent? _output;
    private WaveStream? _reader;
    private IWaveProvider? _playbackProvider;
    private IDisposable? _playbackProviderDisposable;
    private CancellationTokenSource? _loadCancellation;
    private string? _decodedPath;
    private string? _currentAudioPath;
    private TimeSpan _clockAnchorTime = TimeSpan.Zero;
    private TimeSpan _trackDuration = TimeSpan.Zero;
    private bool _isPlaying;
    private bool _isDarkTheme = true;
    private const double VolumeBarWidth = 132;
    private const double RmsFloorDb = -60;
    private double _volume = 0.80;
    private double _previousVolume = 0.80;
    private float[] _peaks = [];
    private int _waveformChannelCount = 1;
    private float[] _rmsLevels = [];
    private double _leftRms;
    private double _rightRms;
    private double _leftRmsDb = RmsFloorDb;
    private double _rightRmsDb = RmsFloorDb;
    private double _playbackProgress;
    private int _loadVersion;
    private bool _suppressBrowserSelection;
    private string _trackFileName = "Open an audio file";
    private string _trackDurationText = "";
    private string _trackFormat = "WAV, MP3, M4A, FLAC via FFmpeg";
    private string _trackFormatBadge = "";
    private string _currentTime = "0:00.000";
    private string _totalTime = "0:00.000";

    public event PropertyChangedEventHandler? PropertyChanged;

    public ObservableCollection<FolderNode> FolderNodes { get; } = [];
    public ObservableCollection<AudioFileItem> BrowserFiles { get; } = [];

    public MainWindow(string? startupFilePath = null)
    {
        InitializeComponent();
        DataContext = this;
        InitializeTaskbarControls();
        ApplyTheme();
        InitializeBrowserRoots();
        PreviewKeyDown += MainWindow_PreviewKeyDown;
        StateChanged += (_, _) => Waveform.SuppressSeekFor(TimeSpan.FromMilliseconds(650));
        Activated += (_, _) => UpdateTaskbarPlaybackState();
        Deactivated += (_, _) => UpdateTaskbarPlaybackState();
        Loaded += (_, _) => Dispatcher.BeginInvoke(UpdateBrowserRowLimit, DispatcherPriority.Loaded);
        if (!string.IsNullOrWhiteSpace(startupFilePath))
        {
            Loaded += async (_, _) => await LoadFileAsync(startupFilePath);
        }

        SizeChanged += (_, _) => Dispatcher.BeginInvoke(UpdateBrowserRowLimit, DispatcherPriority.Render);
    }

    public float[] Peaks
    {
        get => _peaks;
        set => SetField(ref _peaks, value);
    }

    public int WaveformChannelCount
    {
        get => _waveformChannelCount;
        set => SetField(ref _waveformChannelCount, Math.Max(1, value));
    }

    public double PlaybackProgress
    {
        get => _playbackProgress;
        set => SetField(ref _playbackProgress, Math.Clamp(value, 0, 1));
    }

    public double Volume
    {
        get => _volume;
        set
        {
            if (SetField(ref _volume, Math.Clamp(value, 0, 1)))
            {
                if (_output is not null)
                {
                    _output.Volume = (float)_volume;
                }

                OnPropertyChanged(nameof(VolumePercentText));
                OnPropertyChanged(nameof(VolumeFillWidth));
                OnPropertyChanged(nameof(VolumeIcon));
                OnPropertyChanged(nameof(VolumeToolTip));
            }
        }
    }

    public string VolumePercentText => $"{(int)Math.Round(Volume * 100)}";
    public double VolumeFillWidth => VolumeBarWidth * Volume;
    public string VolumeIcon => Volume <= 0 ? "\uE74F" : "\uE767";
    public string VolumeToolTip => Volume <= 0 ? "Restore volume" : "Mute";
    public TimeSpan TrackDuration => _trackDuration;

    public bool IsDarkTheme
    {
        get => _isDarkTheme;
        set
        {
            if (SetField(ref _isDarkTheme, value))
            {
                ApplyTheme();
                OnPropertyChanged(nameof(ThemeButtonIcon));
            }
        }
    }

    public string TrackFileName
    {
        get => _trackFileName;
        set => SetField(ref _trackFileName, value);
    }

    public string TrackDurationText
    {
        get => _trackDurationText;
        set => SetField(ref _trackDurationText, value);
    }

    public string TrackFormat
    {
        get => _trackFormat;
        set => SetField(ref _trackFormat, value);
    }

    public string TrackFormatBadge
    {
        get => _trackFormatBadge;
        set
        {
            if (SetField(ref _trackFormatBadge, value))
            {
                OnPropertyChanged(nameof(TrackFormatBadgeText));
            }
        }
    }

    public string TrackFormatBadgeText => string.IsNullOrWhiteSpace(TrackFormatBadge) ? "" : $"[{TrackFormatBadge}]";

    public string CurrentTime
    {
        get => _currentTime;
        set => SetField(ref _currentTime, value);
    }

    public string TotalTime
    {
        get => _totalTime;
        set => SetField(ref _totalTime, value);
    }

    public string PlayButtonIcon => _isPlaying ? "\uE769" : "\uE768";
    public bool IsPlaybackActive => _isPlaying;
    public string PlaybackStateText => _isPlaying ? "Played" : "Paused";
    public string PlaybackOrderIcon => _playbackOrderMode switch
    {
        PlaybackOrderMode.RepeatOne => "\uE8ED",
        PlaybackOrderMode.Shuffle => "\uE8B1",
        _ => "\uE8EE"
    };
    public string PlaybackOrderToolTip => _playbackOrderMode switch
    {
        PlaybackOrderMode.RepeatOne => "Repeat one file",
        PlaybackOrderMode.Shuffle => "Random order",
        _ => "Sequential order"
    };
    public string ThemeButtonIcon => IsDarkTheme ? "\uE708" : "\uE706";
    public double LeftRmsLevel => DbToMeter(_leftRmsDb);
    public double RightRmsLevel => DbToMeter(_rightRmsDb);
    public Visibility RmsVisibility => _currentAudioPath is null ? Visibility.Collapsed : Visibility.Visible;
    public string RmsText => $"L {FormatDb(_leftRmsDb)}  R {FormatDb(_rightRmsDb)}";
    public string LeftRmsText => FormatDb(_leftRmsDb);
    public string RightRmsText => FormatDb(_rightRmsDb);
    public string LeftRmsNumberText => FormatDbNumber(_leftRmsDb);
    public string RightRmsNumberText => FormatDbNumber(_rightRmsDb);

    public Brush WindowBackground { get; private set; } = Brushes.Black;
    public Brush TopBarBackground { get; private set; } = Brushes.Black;
    public Brush BottomBarBackground { get; private set; } = Brushes.Black;
    public Brush WaveShellBackground { get; private set; } = Brushes.Black;
    public Brush PrimaryText { get; private set; } = Brushes.White;
    public Brush SecondaryText { get; private set; } = Brushes.LightGray;
    public Brush BrowserHoverBackground { get; private set; } = Brushes.Black;
    public Brush BrowserHeaderHoverBackground { get; private set; } = Brushes.Black;
    public Brush BrowserSelectionBackground { get; private set; } = Brushes.Black;
    public Brush BrowserSelectionForeground { get; private set; } = Brushes.White;
    public Brush BrowserSelectionBorderBrush { get; private set; } = Brushes.Gray;
    public Brush BrowserDividerBrush { get; private set; } = Brushes.Gray;
    public Brush ScrollThumbBackground { get; private set; } = Brushes.Gray;
    public Brush ScrollThumbHoverBackground { get; private set; } = Brushes.Gray;
    public Brush ScrollButtonForeground { get; private set; } = Brushes.Gray;

    private async void OpenFile_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog
        {
            Filter = "Audio files|*.wav;*.mp3;*.m4a;*.flac;*.ogg|All files|*.*",
            Title = "Open audio file"
        };

        if (dialog.ShowDialog(this) != true)
        {
            return;
        }

        await LoadFileAsync(dialog.FileName);
    }

    private async Task LoadFileAsync(string path)
    {
        var loadVersion = ++_loadVersion;
        _loadCancellation?.Cancel();
        var loadCancellation = new CancellationTokenSource();
        _loadCancellation = loadCancellation;
        var cancellationToken = loadCancellation.Token;

        if (!FfmpegService.IsSupported(path))
        {
            MessageBox.Show(this, "Supported formats: WAV, MP3, M4A, FLAC, OGG.", "Unsupported file", MessageBoxButton.OK, MessageBoxImage.Information);
            return;
        }

        var hadActivePlayback = _output is not null;
        _decodedPath = null;
        TrackFileName = $"Loading: {Path.GetFileName(path)}";
        TrackDurationText = "";
        TrackFormat = "Decoding and building waveform...";
        TrackFormatBadge = "";
        Peaks = [];
        WaveformChannelCount = 1;
        _rmsLevels = [];
        SetRmsLevels(0, 0);
        PlaybackProgress = 0;
        CurrentTime = "0:00.000";
        TotalTime = "0:00.000";
        if (!hadActivePlayback)
        {
            StopPlayback();
        }

        var playbackStarted = false;

        try
        {
            var directPlayback = await Task.Run(() => TryCreatePlaybackContext(path), cancellationToken);
            if (directPlayback is not null && loadVersion == _loadVersion && !cancellationToken.IsCancellationRequested)
            {
                ActivatePlayback(directPlayback, TimeSpan.Zero);
                playbackStarted = true;
                TrackFileName = Path.GetFileNameWithoutExtension(path);
                TrackDurationText = FormatTime(_trackDuration);
                TrackFormat = "Preparing waveform...";
                TrackFormatBadge = Path.GetExtension(path).TrimStart('.').ToUpperInvariant();
                TotalTime = FormatTime(_trackDuration);
                SetCurrentAudioPath(path);
                StartPlayback();
                _ = Dispatcher.BeginInvoke(() => UpdateBrowserForFile(path), DispatcherPriority.Background);
                await Task.Yield();
            }

            _ffmpeg ??= new FfmpegService();
            var decodedPathForLoad = await _ffmpeg.DecodeToPortableWavAsync(path, cancellationToken);
            if (loadVersion != _loadVersion || cancellationToken.IsCancellationRequested)
            {
                return;
            }

            _decodedPath = decodedPathForLoad;
            var info = _ffmpeg.ReadTrackInfo(path, _decodedPath);

            TrackFileName = Path.GetFileNameWithoutExtension(info.FileName);
            TrackDurationText = FormatTime(info.Duration);
            TrackFormat = info.FormatLine;
            TrackFormatBadge = info.FormatName;
            TotalTime = FormatTime(info.Duration);
            if (!playbackStarted)
            {
                CurrentTime = "0:00.000";
            }
            if (!playbackStarted)
            {
                var decodedPlayback = await Task.Run(() => TryCreatePlaybackContext(_decodedPath), cancellationToken);
                if (decodedPlayback is null)
                {
                    throw new InvalidOperationException("Could not initialize audio playback.");
                }

                ActivatePlayback(decodedPlayback, TimeSpan.Zero);
                SetCurrentAudioPath(path);
                UpdateBrowserForFile(path);
                playbackStarted = true;
            }

            var decodedPath = _decodedPath;
            var waveformTask = Task.Run(() =>
            {
                cancellationToken.ThrowIfCancellationRequested();
                return _ffmpeg.GetOrBuildWaveformData(path, decodedPath, cancellationToken);
            }, cancellationToken);
            var waveformData = await waveformTask;
            if (loadVersion != _loadVersion || decodedPath != _decodedPath || cancellationToken.IsCancellationRequested)
            {
                return;
            }

            Peaks = waveformData.Peaks;
            WaveformChannelCount = waveformData.PeakChannels;
            _rmsLevels = waveformData.RmsLevels;
            UpdateRmsMeters(PlaybackProgress);
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception ex)
        {
            if (loadVersion != _loadVersion)
            {
                return;
            }

            if (playbackStarted)
            {
                TrackFormat = $"Playback started. Waveform unavailable: {ex.Message}";
                return;
            }

            MessageBox.Show(this, ex.Message, "Could not load audio", MessageBoxButton.OK, MessageBoxImage.Error);
            TrackFileName = "Open an audio file";
            TrackDurationText = "";
            TrackFormat = "Put ffmpeg.exe in ./ffmpeg/ffmpeg.exe or add FFmpeg to PATH";
            TrackFormatBadge = "";
        }
    }

    private void PlayPause_Click(object sender, RoutedEventArgs e)
    {
        if (_reader is null && _decodedPath is null)
        {
            return;
        }

        if (_output is null && _decodedPath is not null)
        {
            var context = TryCreatePlaybackContext(_decodedPath);
            if (context is not null)
            {
                ActivatePlayback(context, GetCurrentVisualTime());
                PausePlayback();
            }
        }

        if (_output is null)
        {
            return;
        }

        if (_isPlaying)
        {
            PausePlayback();
            return;
        }

        StartPlayback();
    }

    private void StartPlayback()
    {
        if (_output is null)
        {
            return;
        }

        var resumeTime = GetCurrentVisualTime();
        _output.Play();
        SetPlaying(true);
        StartVisualClock(resumeTime);
    }

    private void PausePlayback()
    {
        if (_output is null)
        {
            return;
        }

        _output.Pause();
        SyncClockToVisualPosition();
        SetPlaying(false);
    }

    private void Stop_Click(object sender, RoutedEventArgs e) => StopPlayback();

    private async void PreviousTrack_Click(object sender, RoutedEventArgs e) => await PlayAdjacentTrackAsync(-1);

    private async void NextTrack_Click(object sender, RoutedEventArgs e) => await PlayNextTrackAsync();

    private void Restart_Click(object sender, RoutedEventArgs e)
    {
        SeekTo(0);
        if (_output is not null && !_isPlaying)
        {
            RefreshPlaybackPosition();
        }
    }

    private void TogglePlaybackOrder_Click(object sender, RoutedEventArgs e)
    {
        _playbackOrderMode = _playbackOrderMode switch
        {
            PlaybackOrderMode.Sequential => PlaybackOrderMode.RepeatOne,
            PlaybackOrderMode.RepeatOne => PlaybackOrderMode.Shuffle,
            _ => PlaybackOrderMode.Sequential
        };

        OnPropertyChanged(nameof(PlaybackOrderIcon));
        OnPropertyChanged(nameof(PlaybackOrderToolTip));
    }

    private void ToggleTheme_Click(object sender, RoutedEventArgs e) => IsDarkTheme = !IsDarkTheme;

    private void ToggleMute_Click(object sender, RoutedEventArgs e)
    {
        if (Volume > 0)
        {
            _previousVolume = Volume;
            Volume = 0;
            return;
        }

        Volume = _previousVolume > 0 ? _previousVolume : 0.80;
    }

    private void Waveform_SeekRequested(object? sender, double progress) => SeekTo(progress);

    private void Window_DragEnter(object sender, DragEventArgs e) => UpdateDragDropEffect(e);

    private void Window_DragOver(object sender, DragEventArgs e) => UpdateDragDropEffect(e);

    private void Window_DragLeave(object sender, DragEventArgs e)
    {
        DragDropOverlay.Visibility = Visibility.Collapsed;
        e.Handled = true;
    }

    private async void Window_Drop(object sender, DragEventArgs e)
    {
        var path = GetFirstSupportedDropFile(e);
        if (path is null)
        {
            e.Effects = DragDropEffects.None;
            DragDropOverlay.Visibility = Visibility.Collapsed;
            e.Handled = true;
            return;
        }

        e.Effects = DragDropEffects.Copy;
        DragDropOverlay.Visibility = Visibility.Collapsed;
        e.Handled = true;
        await LoadFileAsync(path);
    }

    private void UpdateDragDropEffect(DragEventArgs e)
    {
        var hasSupportedFile = GetFirstSupportedDropFile(e) is not null;
        e.Effects = hasSupportedFile ? DragDropEffects.Copy : DragDropEffects.None;
        DragDropOverlay.Visibility = hasSupportedFile ? Visibility.Visible : Visibility.Collapsed;
        e.Handled = true;
    }

    private static string? GetFirstSupportedDropFile(DragEventArgs e)
    {
        if (!e.Data.GetDataPresent(DataFormats.FileDrop))
        {
            return null;
        }

        return ((string[])e.Data.GetData(DataFormats.FileDrop)!)
            .FirstOrDefault(path => File.Exists(path) && FfmpegService.IsSupported(path));
    }

    private async void BrowserFileList_SelectionChanged(object sender, System.Windows.Controls.SelectionChangedEventArgs e)
    {
        if (_suppressBrowserSelection || BrowserFileList.SelectedItem is not AudioFileItem item)
        {
            return;
        }

        await LoadFileAsync(item.FullPath);
    }

    private void FolderTree_SelectedItemChanged(object sender, RoutedPropertyChangedEventArgs<object> e)
    {
        if (e.NewValue is FolderNode node)
        {
            LoadBrowserFiles(node.FullPath, _currentAudioPath);
        }
    }

    private void FolderTreeItem_Expanded(object sender, RoutedEventArgs e)
    {
        if (e.OriginalSource is System.Windows.Controls.TreeViewItem { DataContext: FolderNode node })
        {
            EnsureFolderChildrenLoaded(node);
        }
    }

    private void BrowserSplitter_DragDelta(object sender, System.Windows.Controls.Primitives.DragDeltaEventArgs e) =>
        UpdateBrowserRowLimit();

    private PlaybackContext? TryCreatePlaybackContext(string path)
    {
        try
        {
            var reader = CreatePlaybackReader(path);
            IWaveProvider provider = reader;
            IDisposable? providerDisposable = null;
            var output = new WaveOutEvent
            {
                Volume = (float)Volume,
                DesiredLatency = 60,
                NumberOfBuffers = 2
            };
            try
            {
                output.Init(provider);
            }
            catch
            {
                output.Dispose();
                output = new WaveOutEvent
                {
                    Volume = (float)Volume,
                    DesiredLatency = 60,
                    NumberOfBuffers = 2
                };
                provider = CreateResampledPlaybackProvider(reader, out providerDisposable);
                output.Init(provider);
            }

            output.PlaybackStopped += (sender, stoppedArgs) =>
            {
                if (!ReferenceEquals(sender, _output))
                {
                    return;
                }

                if (_reader is not null && _reader.Position >= _reader.Length)
                {
                    Dispatcher.BeginInvoke(HandlePlaybackCompleted, DispatcherPriority.Normal);
                }
            };
            return new PlaybackContext(reader, provider, providerDisposable, output, reader.TotalTime);
        }
        catch
        {
            return null;
        }
    }

    private void ActivatePlayback(PlaybackContext context, TimeSpan startTime)
    {
        var oldContext = CaptureCurrentPlaybackContext();

        _reader = context.Reader;
        _playbackProvider = context.Provider;
        _playbackProviderDisposable = context.ProviderDisposable;
        _output = context.Output;
        _trackDuration = context.Duration;
        OnPropertyChanged(nameof(TrackDuration));

        _reader.CurrentTime = ClampTime(startTime);
        _output.Play();
        SetPlaying(true);
        StartVisualClock(startTime);
        oldContext?.Dispose();
    }

    private PlaybackContext? CaptureCurrentPlaybackContext()
    {
        if (_reader is null || _output is null)
        {
            return null;
        }

        return new PlaybackContext(_reader, _playbackProvider ?? _reader, _playbackProviderDisposable, _output, _trackDuration);
    }

    private async Task HandlePlaybackCompleted()
    {
        SetPlaying(false);

        var nextPath = GetNextPlaybackPath();
        if (nextPath is null)
        {
            ResetPlaybackToStart();
            return;
        }

        await LoadFileAsync(nextPath);
    }

    private async Task PlayNextTrackAsync()
    {
        var nextPath = GetNextPlaybackPath() ?? GetAdjacentPlaybackPath(1);
        if (nextPath is not null)
        {
            await LoadFileAsync(nextPath);
        }
    }

    private async Task PlayAdjacentTrackAsync(int direction)
    {
        var path = GetAdjacentPlaybackPath(direction);
        if (path is not null)
        {
            await LoadFileAsync(path);
        }
    }

    private void ResetPlaybackToStart()
    {
        _clockAnchorTime = TimeSpan.Zero;
        _playbackClock.Reset();
        PlaybackProgress = 0;
        CurrentTime = "0:00.000";
        UpdateRmsMeters(0);

        if (_reader is not null)
        {
            _reader.CurrentTime = TimeSpan.Zero;
        }
    }

    private string? GetNextPlaybackPath()
    {
        if (_currentAudioPath is null)
        {
            return null;
        }

        if (_playbackOrderMode == PlaybackOrderMode.RepeatOne)
        {
            return _currentAudioPath;
        }

        if (BrowserFiles.Count == 0)
        {
            return null;
        }

        var currentIndex = BrowserFiles
            .Select((item, index) => new { item, index })
            .FirstOrDefault(entry => entry.item.FullPath.Equals(_currentAudioPath, StringComparison.OrdinalIgnoreCase))
            ?.index ?? -1;

        if (_playbackOrderMode == PlaybackOrderMode.Shuffle)
        {
            if (BrowserFiles.Count == 1)
            {
                return BrowserFiles[0].FullPath;
            }

            int nextIndex;
            do
            {
                nextIndex = Random.Shared.Next(BrowserFiles.Count);
            }
            while (nextIndex == currentIndex);

            return BrowserFiles[nextIndex].FullPath;
        }

        var sequentialIndex = currentIndex + 1;
        return sequentialIndex >= 0 && sequentialIndex < BrowserFiles.Count
            ? BrowserFiles[sequentialIndex].FullPath
            : null;
    }

    private string? GetAdjacentPlaybackPath(int direction)
    {
        if (BrowserFiles.Count == 0)
        {
            return null;
        }

        var currentIndex = _currentAudioPath is null
            ? -1
            : BrowserFiles
                .Select((item, index) => new { item, index })
                .FirstOrDefault(entry => entry.item.FullPath.Equals(_currentAudioPath, StringComparison.OrdinalIgnoreCase))
                ?.index ?? -1;

        var nextIndex = currentIndex < 0
            ? 0
            : (currentIndex + direction + BrowserFiles.Count) % BrowserFiles.Count;
        return BrowserFiles[nextIndex].FullPath;
    }

    private void SeekTo(double progress)
    {
        PlaybackProgress = progress;
        UpdateTaskbarPlaybackState();
        if (_trackDuration <= TimeSpan.Zero)
        {
            return;
        }

        var target = TimeSpan.FromTicks((long)(_trackDuration.Ticks * progress));
        StartVisualClock(target);
        CurrentTime = FormatTime(target);
        UpdateRmsMeters(progress);
        ApplyAudioSeek(target);
    }

    private void VolumeBar_MouseDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount > 1)
        {
            Volume = 0.80;
            e.Handled = true;
            return;
        }

        SetVolumeFromPoint(e.GetPosition((IInputElement)sender).X);
    }

    private void VolumeBar_MouseMove(object sender, MouseEventArgs e)
    {
        if (e.LeftButton == MouseButtonState.Pressed)
        {
            SetVolumeFromPoint(e.GetPosition((IInputElement)sender).X);
        }
    }

    private void VolumeBar_MouseWheel(object sender, MouseWheelEventArgs e)
    {
        var step = e.Delta > 0 ? 0.05 : -0.05;
        Volume += step;
        if (Volume > 0)
        {
            _previousVolume = Volume;
        }

        e.Handled = true;
    }

    private void SetVolumeFromPoint(double x)
    {
        Volume = Math.Clamp(x / VolumeBarWidth, 0, 1);
        if (Volume > 0)
        {
            _previousVolume = Volume;
        }
    }

    private void ApplyAudioSeek(TimeSpan target)
    {
        if (_reader is null || _output is null)
        {
            return;
        }

        var targetVolume = (float)Volume;
        _output.Volume = 0f;
        _reader.CurrentTime = ClampTime(target);
        _output.Volume = targetVolume;

        if (!_isPlaying)
        {
            _output.Play();
            SetPlaying(true);
            StartVisualClock(target);
        }
    }

    private void RefreshPlaybackPosition()
    {
        if (_trackDuration <= TimeSpan.Zero)
        {
            return;
        }

        var currentTime = GetCurrentVisualTime();
        PlaybackProgress = currentTime.TotalSeconds / _trackDuration.TotalSeconds;
        CurrentTime = FormatTime(currentTime);
        UpdateRmsMeters(PlaybackProgress);
        UpdateTaskbarPlaybackState();
    }

    private void UpdateRmsMeters(double progress)
    {
        if (_rmsLevels.Length < 2)
        {
            SetRmsLevels(0, 0);
            return;
        }

        var frames = _rmsLevels.Length / 2;
        var index = Math.Clamp((int)Math.Round(Math.Clamp(progress, 0, 1) * (frames - 1)), 0, frames - 1);
        SetRmsLevels(_rmsLevels[index * 2], _rmsLevels[index * 2 + 1]);
    }

    private void SetRmsLevels(double left, double right)
    {
        _leftRms = Math.Clamp(left, 0, 1);
        _rightRms = Math.Clamp(right, 0, 1);
        _leftRmsDb = RmsToDb(_leftRms);
        _rightRmsDb = RmsToDb(_rightRms);
        OnPropertyChanged(nameof(LeftRmsLevel));
        OnPropertyChanged(nameof(RightRmsLevel));
        OnPropertyChanged(nameof(RmsText));
        OnPropertyChanged(nameof(LeftRmsText));
        OnPropertyChanged(nameof(RightRmsText));
        OnPropertyChanged(nameof(LeftRmsNumberText));
        OnPropertyChanged(nameof(RightRmsNumberText));
    }

    private void SetCurrentAudioPath(string? path)
    {
        if (_currentAudioPath == path)
        {
            return;
        }

        _currentAudioPath = path;
        OnPropertyChanged(nameof(RmsVisibility));
    }

    private static double RmsToDb(double rms) => rms <= 0.000001 ? RmsFloorDb : 20 * Math.Log10(rms);

    private static double DbToMeter(double db)
    {
        var normalized = (db - RmsFloorDb + 2) / Math.Abs(RmsFloorDb);
        return Math.Clamp(normalized, 0, 1);
    }

    private static string FormatDb(double db) => db <= RmsFloorDb ? "-inf" : $"{db:0.0}dB";
    private static string FormatDbNumber(double db) => db <= RmsFloorDb ? "-inf" : $"{db:0}";

    private void UpdateBrowserForFile(string filePath)
    {
        var folder = Path.GetDirectoryName(filePath);
        if (string.IsNullOrWhiteSpace(folder) || !Directory.Exists(folder))
        {
            return;
        }

        if (FolderNodes.Count == 0)
        {
            InitializeBrowserRoots();
        }

        var folderNode = EnsureFolderPathVisible(folder);
        LoadBrowserFiles(folder, filePath);
        if (folderNode is not null)
        {
            Dispatcher.BeginInvoke(() => SelectFolderNode(folderNode), DispatcherPriority.Loaded);
        }
    }

    private FolderNode? EnsureFolderPathVisible(string folderPath)
    {
        var fullPath = Path.GetFullPath(folderPath).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var rootPath = Path.GetPathRoot(fullPath);
        if (string.IsNullOrWhiteSpace(rootPath))
        {
            return null;
        }

        var root = FolderNodes.FirstOrDefault(node =>
            node.FullPath.Equals(rootPath, StringComparison.OrdinalIgnoreCase));
        if (root is null)
        {
            root = new FolderNode(rootPath, rootPath);
            FolderNodes.Add(root);
        }

        var current = root;
        LoadFolderChildren(current, 1);

        var relative = Path.GetRelativePath(rootPath, fullPath);
        if (relative == "." || string.IsNullOrWhiteSpace(relative))
        {
            current.IsExpanded = true;
            return current;
        }

        foreach (var segment in relative.Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                     .Where(part => !string.IsNullOrWhiteSpace(part)))
        {
            current.IsExpanded = true;
            LoadFolderChildren(current, 1);

            var nextPath = Path.Combine(current.FullPath, segment);
            var next = current.Children.FirstOrDefault(node =>
                node.FullPath.Equals(nextPath, StringComparison.OrdinalIgnoreCase));
            if (next is null && Directory.Exists(nextPath) && IsVisibleFileSystemEntry(new DirectoryInfo(nextPath)))
            {
                next = CreateFolderNode(nextPath, 0);
                current.Children.Add(next);
            }

            if (next is null)
            {
                return current;
            }

            current = next;
        }

        current.IsExpanded = true;
        LoadFolderChildren(current, 1);
        return current;
    }

    private void SelectFolderNode(FolderNode node)
    {
        var container = FindTreeViewItem(FolderTree, node);
        if (container is null)
        {
            FolderTree.UpdateLayout();
            container = FindTreeViewItem(FolderTree, node);
        }

        if (container is null)
        {
            return;
        }

        container.IsSelected = true;
        container.BringIntoView();
    }

    private static System.Windows.Controls.TreeViewItem? FindTreeViewItem(System.Windows.Controls.ItemsControl parent, FolderNode node)
    {
        parent.UpdateLayout();
        foreach (var item in parent.Items)
        {
            var container = parent.ItemContainerGenerator.ContainerFromItem(item) as System.Windows.Controls.TreeViewItem;
            if (container is null)
            {
                continue;
            }

            if (ReferenceEquals(item, node))
            {
                return container;
            }

            var child = FindTreeViewItem(container, node);
            if (child is not null)
            {
                return child;
            }
        }

        return null;
    }

    private void InitializeBrowserRoots()
    {
        FolderNodes.Clear();
        foreach (var drive in DriveInfo.GetDrives().Where(drive => drive.IsReady))
        {
            var node = new FolderNode(drive.Name, drive.RootDirectory.FullName);
            LoadFolderChildren(node, 1);
            FolderNodes.Add(node);
        }
    }

    private FolderNode CreateFolderNode(string folderPath, int depth)
    {
        var name = Path.GetFileName(folderPath);
        if (string.IsNullOrWhiteSpace(name))
        {
            name = folderPath;
        }

        var node = new FolderNode(name, folderPath);
        LoadFolderChildren(node, depth);
        return node;
    }

    private void EnsureFolderChildrenLoaded(FolderNode node)
    {
        if (node.IsLoaded)
        {
            foreach (var child in node.Children)
            {
                LoadFolderChildren(child, 1);
            }

            return;
        }

        LoadFolderChildren(node, 1);
    }

    private void LoadFolderChildren(FolderNode node, int depth)
    {
        if (node.IsLoaded || depth <= 0)
        {
            return;
        }

        try
        {
            foreach (var childPath in Directory.EnumerateDirectories(node.FullPath)
                         .Where(path => IsVisibleFileSystemEntry(new DirectoryInfo(path)))
                         .OrderBy(path => Path.GetFileName(path)))
            {
                node.Children.Add(CreateFolderNode(childPath, depth - 1));
            }

            node.IsLoaded = true;
        }
        catch (UnauthorizedAccessException)
        {
        }
        catch (IOException)
        {
        }
    }

    private void LoadBrowserFiles(string folderPath, string? selectedPath)
    {
        BrowserFiles.Clear();
        if (!Directory.Exists(folderPath))
        {
            return;
        }

        AudioFileItem? selectedItem = null;
        try
        {
            foreach (var filePath in Directory.EnumerateFiles(folderPath)
                         .Where(FfmpegService.IsSupported)
                         .Where(path => IsVisibleFileSystemEntry(new FileInfo(path)))
                         .OrderBy(path => Path.GetFileName(path)))
            {
                var fileInfo = new FileInfo(filePath);
                var item = new AudioFileItem(
                    Path.GetFileName(filePath),
                    filePath,
                    Path.GetExtension(filePath).TrimStart('.').ToUpperInvariant(),
                    FormatFileSize(fileInfo.Length),
                    fileInfo.LastWriteTime.ToString("dd.MM.yyyy HH:mm"),
                    "\uE8A5");
                BrowserFiles.Add(item);

                if (selectedPath is not null && filePath.Equals(selectedPath, StringComparison.OrdinalIgnoreCase))
                {
                    selectedItem = item;
                }
            }
        }
        catch (UnauthorizedAccessException)
        {
        }
        catch (IOException)
        {
        }

        if (selectedItem is not null)
        {
            _suppressBrowserSelection = true;
            BrowserFileList.SelectedItem = selectedItem;
            BrowserFileList.ScrollIntoView(selectedItem);
            _suppressBrowserSelection = false;
        }
    }

    private static string FormatFileSize(long bytes)
    {
        string[] units = ["B", "KB", "MB", "GB"];
        var size = (double)bytes;
        var unit = 0;
        while (size >= 1024 && unit < units.Length - 1)
        {
            size /= 1024;
            unit++;
        }

        return unit == 0 ? $"{size:0} {units[unit]}" : $"{size:0.##} {units[unit]}";
    }

    private static bool IsVisibleFileSystemEntry(FileSystemInfo entry)
    {
        return (entry.Attributes & (FileAttributes.Hidden | FileAttributes.System)) == 0;
    }

    private static WaveStream CreatePlaybackReader(string path)
    {
        var extension = Path.GetExtension(path);
        if (extension.Equals(".wav", StringComparison.OrdinalIgnoreCase))
        {
            return new WaveFileReader(path);
        }

        if (extension.Equals(".mp3", StringComparison.OrdinalIgnoreCase))
        {
            return new Mp3FileReader(path);
        }

        return new MediaFoundationReader(path);
    }

    private static IWaveProvider CreateResampledPlaybackProvider(WaveStream source, out IDisposable providerDisposable)
    {
        var targetFormat = new WaveFormat(44100, 16, 2);
        var resampler = new MediaFoundationResampler(source, targetFormat)
        {
            ResamplerQuality = 60
        };
        providerDisposable = resampler;
        return resampler;
    }

    private void UpdateBrowserRowLimit()
    {
        const double reserve = 10;
        var available = RootLayout.ActualHeight
            - TopHeaderBar.ActualHeight
            - BottomControlsBar.ActualHeight
            - SplitterRow.ActualHeight
            - WaveRow.MinHeight
            - reserve;

        var maxHeight = Math.Max(BrowserRow.MinHeight, available);
        BrowserRow.MaxHeight = maxHeight;

        if (BrowserRow.ActualHeight > maxHeight)
        {
            BrowserRow.Height = new GridLength(maxHeight);
        }
    }

    private void StopPlayback()
    {
        SetPlaying(false);
        DisposePlayback();
        _trackDuration = TimeSpan.Zero;
        OnPropertyChanged(nameof(TrackDuration));
        _clockAnchorTime = TimeSpan.Zero;
        _playbackClock.Reset();
        PlaybackProgress = 0;
        CurrentTime = "0:00.000";
        _rmsLevels = [];
        SetRmsLevels(0, 0);
        UpdateTaskbarPlaybackState();
    }

    private void DisposePlayback()
    {
        CaptureCurrentPlaybackContext()?.Dispose();
        _output = null;
        _playbackProvider = null;
        _playbackProviderDisposable = null;
        _reader = null;
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        var handle = new WindowInteropHelper(this).Handle;
        if (handle != IntPtr.Zero)
        {
            DragAcceptFiles(handle, false);
        }

        _hwndSource?.RemoveHook(WndProc);
        _loadCancellation?.Cancel();
        _loadCancellation?.Dispose();
        _ffmpeg?.ClearWaveformCache();
        StopPlayback();
        base.OnClosing(e);
    }

    private void SetPlaying(bool value)
    {
        if (_isPlaying == value)
        {
            return;
        }

        _isPlaying = value;
        if (_isPlaying)
        {
            CompositionTarget.Rendering += OnRendering;
        }
        else
        {
            CompositionTarget.Rendering -= OnRendering;
        }

        OnPropertyChanged(nameof(PlayButtonIcon));
        OnPropertyChanged(nameof(IsPlaybackActive));
        OnPropertyChanged(nameof(PlaybackStateText));
        UpdateTaskbarPlayPauseButton();
        UpdateTaskbarPlaybackState();
    }

    private void UpdateTaskbarPlaybackState()
    {
        if (TaskbarItemInfo is null)
        {
            return;
        }

        TaskbarItemInfo.ProgressState = _isPlaying && !IsActive
            ? TaskbarItemProgressState.Normal
            : TaskbarItemProgressState.None;
        TaskbarItemInfo.ProgressValue = Math.Clamp(PlaybackProgress, 0, 1);
    }

    private void OnRendering(object? sender, EventArgs e) => RefreshPlaybackPosition();

    private void StartVisualClock(TimeSpan anchorTime)
    {
        _clockAnchorTime = ClampTime(anchorTime);

        if (_isPlaying)
        {
            _playbackClock.Restart();
        }
        else
        {
            _playbackClock.Reset();
        }
    }

    private void SyncClockToVisualPosition()
    {
        _clockAnchorTime = GetCurrentVisualTime();
        _playbackClock.Reset();
    }

    private TimeSpan GetCurrentVisualTime()
    {
        var time = _clockAnchorTime;
        if (_isPlaying)
        {
            time += _playbackClock.Elapsed;
        }

        return ClampTime(time);
    }

    private TimeSpan ClampTime(TimeSpan time)
    {
        if (time < TimeSpan.Zero)
        {
            return TimeSpan.Zero;
        }

        return _trackDuration > TimeSpan.Zero && time > _trackDuration
            ? _trackDuration
            : time;
    }

    private void ApplyTheme()
    {
        if (IsDarkTheme)
        {
            WindowBackground = Brush("#101216");
            TopBarBackground = Brush("#171a20");
            BottomBarBackground = Brush("#171a20");
            WaveShellBackground = Brush("#0f1115");
            PrimaryText = Brush("#eef2f6");
            SecondaryText = Brush("#98a4b3");
            BrowserHoverBackground = Brush("#252b33");
            BrowserHeaderHoverBackground = Brush("#20252c");
            BrowserSelectionBackground = Brush("#343b45");
            BrowserSelectionForeground = Brush("#eef2f6");
            BrowserSelectionBorderBrush = Brush("#4b5664");
            BrowserDividerBrush = Brush("#343b45");
            ScrollThumbBackground = Brush("#58616d");
            ScrollThumbHoverBackground = Brush("#6c7785");
            ScrollButtonForeground = Brush("#7f8b99");
        }
        else
        {
            WindowBackground = Brush("#f4f6f8");
            TopBarBackground = Brush("#ffffff");
            BottomBarBackground = Brush("#ffffff");
            WaveShellBackground = Brush("#eef1f5");
            PrimaryText = Brush("#18202a");
            SecondaryText = Brush("#657181");
            BrowserHoverBackground = Brush("#edf2f7");
            BrowserHeaderHoverBackground = Brush("#e7edf3");
            BrowserSelectionBackground = Brush("#d9e0e8");
            BrowserSelectionForeground = Brush("#18202a");
            BrowserSelectionBorderBrush = Brush("#aeb9c6");
            BrowserDividerBrush = Brush("#c8d1dc");
            ScrollThumbBackground = Brush("#aeb9c6");
            ScrollThumbHoverBackground = Brush("#8895a3");
            ScrollButtonForeground = Brush("#7d8a98");
        }

        OnPropertyChanged(nameof(WindowBackground));
        OnPropertyChanged(nameof(TopBarBackground));
        OnPropertyChanged(nameof(BottomBarBackground));
        OnPropertyChanged(nameof(WaveShellBackground));
        OnPropertyChanged(nameof(PrimaryText));
        OnPropertyChanged(nameof(SecondaryText));
        OnPropertyChanged(nameof(BrowserHoverBackground));
        OnPropertyChanged(nameof(BrowserHeaderHoverBackground));
        OnPropertyChanged(nameof(BrowserSelectionBackground));
        OnPropertyChanged(nameof(BrowserSelectionForeground));
        OnPropertyChanged(nameof(BrowserSelectionBorderBrush));
        OnPropertyChanged(nameof(BrowserDividerBrush));
        OnPropertyChanged(nameof(ScrollThumbBackground));
        OnPropertyChanged(nameof(ScrollThumbHoverBackground));
        OnPropertyChanged(nameof(ScrollButtonForeground));
        ApplyWindowCaptionTheme();
    }

    protected override void OnSourceInitialized(EventArgs e)
    {
        base.OnSourceInitialized(e);
        var handle = new WindowInteropHelper(this).Handle;
        if (handle != IntPtr.Zero)
        {
            DragAcceptFiles(handle, true);
            _hwndSource = HwndSource.FromHwnd(handle);
            _hwndSource?.AddHook(WndProc);
            UpdateTaskbarPlaybackState();
        }

        ApplyWindowCaptionTheme();
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg != WmDropFiles)
        {
            return IntPtr.Zero;
        }

        var path = GetFirstSupportedShellDropFile(wParam);
        DragFinish(wParam);
        handled = true;

        if (path is not null)
        {
            _ = Dispatcher.BeginInvoke(async () =>
            {
                RestoreForExternalDrop();
                await LoadFileAsync(path);
            }, DispatcherPriority.Normal);
        }

        return IntPtr.Zero;
    }

    private void RestoreForExternalDrop()
    {
        if (WindowState == WindowState.Minimized)
        {
            WindowState = WindowState.Normal;
        }

        Show();
        Activate();
    }

    private static string? GetFirstSupportedShellDropFile(IntPtr dropHandle)
    {
        var fileCount = DragQueryFile(dropHandle, DragQueryAllFiles, null, 0);
        for (uint i = 0; i < fileCount; i++)
        {
            var length = DragQueryFile(dropHandle, i, null, 0);
            if (length == 0)
            {
                continue;
            }

            var path = new StringBuilder((int)length + 1);
            DragQueryFile(dropHandle, i, path, (uint)path.Capacity);
            var filePath = path.ToString();
            if (File.Exists(filePath) && FfmpegService.IsSupported(filePath))
            {
                return filePath;
            }
        }

        return null;
    }

    private void MainWindow_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key != Key.Space)
        {
            return;
        }

        PlayPause_Click(this, new RoutedEventArgs());
        e.Handled = true;
    }

    private void ApplyWindowCaptionTheme()
    {
        var handle = new WindowInteropHelper(this).Handle;
        if (handle == IntPtr.Zero)
        {
            return;
        }

        var darkMode = IsDarkTheme ? 1 : 0;
        DwmSetWindowAttribute(handle, DwmWindowAttribute.UseImmersiveDarkMode, ref darkMode, sizeof(int));

        var captionColor = IsDarkTheme ? 0x001a1717 : 0x00ffffff;
        var textColor = IsDarkTheme ? 0x00f6f2ee : 0x002a2018;
        DwmSetWindowAttribute(handle, DwmWindowAttribute.CaptionColor, ref captionColor, sizeof(int));
        DwmSetWindowAttribute(handle, DwmWindowAttribute.TextColor, ref textColor, sizeof(int));
    }

    private void InitializeTaskbarControls()
    {
        var previousButton = new ThumbButtonInfo
        {
            Description = "Previous track",
            ImageSource = CreateTaskbarIcon(TaskbarIconKind.Previous)
        };
        previousButton.Click += async (_, _) => await PlayAdjacentTrackAsync(-1);

        _taskbarPlayPauseButton = new ThumbButtonInfo
        {
            Description = "Play",
            ImageSource = CreateTaskbarIcon(TaskbarIconKind.Play)
        };
        _taskbarPlayPauseButton.Click += (_, _) => PlayPause_Click(this, new RoutedEventArgs());

        var nextButton = new ThumbButtonInfo
        {
            Description = "Next track",
            ImageSource = CreateTaskbarIcon(TaskbarIconKind.Next)
        };
        nextButton.Click += async (_, _) => await PlayNextTrackAsync();

        TaskbarItemInfo = new TaskbarItemInfo();
        TaskbarItemInfo.ThumbButtonInfos.Add(previousButton);
        TaskbarItemInfo.ThumbButtonInfos.Add(_taskbarPlayPauseButton);
        TaskbarItemInfo.ThumbButtonInfos.Add(nextButton);
        UpdateTaskbarPlayPauseButton();
    }

    private void UpdateTaskbarPlayPauseButton()
    {
        if (_taskbarPlayPauseButton is null)
        {
            return;
        }

        _taskbarPlayPauseButton.Description = _isPlaying ? "Pause" : "Play";
        _taskbarPlayPauseButton.ImageSource = CreateTaskbarIcon(_isPlaying ? TaskbarIconKind.Pause : TaskbarIconKind.Play);
    }

    private static DrawingImage CreateTaskbarIcon(TaskbarIconKind kind)
    {
        var group = new DrawingGroup();
        using (var context = group.Open())
        {
            var brush = Brushes.White;
            switch (kind)
            {
                case TaskbarIconKind.Previous:
                    context.DrawRectangle(brush, null, new Rect(7, 7, 3, 18));
                    context.DrawGeometry(brush, null, Geometry.Parse("M 24 7 L 11 16 L 24 25 Z"));
                    break;
                case TaskbarIconKind.Pause:
                    context.DrawRectangle(brush, null, new Rect(10, 7, 4, 18));
                    context.DrawRectangle(brush, null, new Rect(18, 7, 4, 18));
                    break;
                case TaskbarIconKind.Next:
                    context.DrawGeometry(brush, null, Geometry.Parse("M 8 7 L 21 16 L 8 25 Z"));
                    context.DrawRectangle(brush, null, new Rect(22, 7, 3, 18));
                    break;
                default:
                    context.DrawGeometry(brush, null, Geometry.Parse("M 11 7 L 24 16 L 11 25 Z"));
                    break;
            }
        }

        group.Freeze();
        var image = new DrawingImage(group);
        image.Freeze();
        return image;
    }

    private static SolidColorBrush Brush(string color)
    {
        var brush = new SolidColorBrush((Color)ColorConverter.ConvertFromString(color));
        brush.Freeze();
        return brush;
    }

    private static string FormatTime(TimeSpan value) => $"{(int)value.TotalMinutes}:{value.Seconds:00}.{value.Milliseconds:000}";

    public sealed class FolderNode(string name, string fullPath) : INotifyPropertyChanged
    {
        private bool _isExpanded;

        public event PropertyChangedEventHandler? PropertyChanged;
        public string Name { get; } = name;
        public string FullPath { get; } = fullPath;
        public string Icon { get; } = Path.GetPathRoot(fullPath)?.Equals(fullPath, StringComparison.OrdinalIgnoreCase) == true
            ? "\uE8B7"
            : "\uE8B7";
        public ObservableCollection<FolderNode> Children { get; } = [];
        public bool IsLoaded { get; set; }
        public bool IsExpanded
        {
            get => _isExpanded;
            set
            {
                if (_isExpanded == value)
                {
                    return;
                }

                _isExpanded = value;
                PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(IsExpanded)));
            }
        }
    }

    public sealed record AudioFileItem(string Name, string FullPath, string Format, string Size, string Modified, string Icon);

    private enum PlaybackOrderMode
    {
        Sequential,
        RepeatOne,
        Shuffle
    }

    private enum TaskbarIconKind
    {
        Previous,
        Play,
        Pause,
        Next
    }

    private sealed class PlaybackContext(
        WaveStream reader,
        IWaveProvider provider,
        IDisposable? providerDisposable,
        WaveOutEvent output,
        TimeSpan duration) : IDisposable
    {
        public WaveStream Reader { get; } = reader;
        public IWaveProvider Provider { get; } = provider;
        public IDisposable? ProviderDisposable { get; } = providerDisposable;
        public WaveOutEvent Output { get; } = output;
        public TimeSpan Duration { get; } = duration;

        public void Dispose()
        {
            Output.Stop();
            Output.Dispose();
            ProviderDisposable?.Dispose();
            Reader.Dispose();
        }
    }

    private bool SetField<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return false;
        }

        field = value;
        OnPropertyChanged(propertyName);
        return true;
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null) =>
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));

    [DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, DwmWindowAttribute attribute, ref int attributeValue, int attributeSize);

    [DllImport("shell32.dll")]
    private static extern void DragAcceptFiles(IntPtr hwnd, bool accept);

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern uint DragQueryFile(IntPtr hDrop, uint iFile, StringBuilder? lpszFile, uint cch);

    [DllImport("shell32.dll")]
    private static extern void DragFinish(IntPtr hDrop);

    private enum DwmWindowAttribute
    {
        UseImmersiveDarkMode = 20,
        CaptionColor = 35,
        TextColor = 36
    }
}
