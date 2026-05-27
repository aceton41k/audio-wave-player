using System.Globalization;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace AudioWavePlayer.Controls;

public sealed class WaveformControl : FrameworkElement
{
    private const double MinChannelHeight = 96;
    private RenderTargetBitmap? _baseBitmap;
    private RenderTargetBitmap? _playedBitmap;
    private Size _bitmapSize;
    private bool _bitmapTheme;
    private float[]? _bitmapPeaks;
    private int _bitmapChannelCount;
    private double _hoverProgress;
    private bool _isHovering;
    private bool _isPointerDown;
    private DateTime _ignoreSeekUntilUtc = DateTime.MinValue;
    private readonly Typeface _labelTypeface = new("Consolas");
    private readonly SolidColorBrush _darkBackground = FrozenBrush(18, 20, 24);
    private readonly SolidColorBrush _lightBackground = FrozenBrush(246, 247, 249);
    private readonly SolidColorBrush _darkGrid = FrozenBrush(44, 48, 55);
    private readonly SolidColorBrush _lightGrid = FrozenBrush(220, 224, 230);
    private readonly SolidColorBrush _darkWave = FrozenBrush(124, 138, 152);
    private readonly SolidColorBrush _lightWave = FrozenBrush(95, 109, 125);
    private readonly SolidColorBrush _darkPlayed = FrozenBrush(204, 216, 224);
    private readonly SolidColorBrush _lightPlayed = FrozenBrush(60, 76, 94);
    private readonly SolidColorBrush _accent = FrozenBrush(255, 122, 61);
    private readonly SolidColorBrush _hover = FrozenBrush(86, 168, 255);
    private readonly SolidColorBrush _labelText = FrozenBrush(255, 255, 255);

    public static readonly DependencyProperty PeaksProperty =
        DependencyProperty.Register(nameof(Peaks), typeof(float[]), typeof(WaveformControl),
            new FrameworkPropertyMetadata(Array.Empty<float>(), FrameworkPropertyMetadataOptions.AffectsRender, OnPeaksChanged));

    public static readonly DependencyProperty ProgressProperty =
        DependencyProperty.Register(nameof(Progress), typeof(double), typeof(WaveformControl),
            new FrameworkPropertyMetadata(0d, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty ChannelCountProperty =
        DependencyProperty.Register(nameof(ChannelCount), typeof(int), typeof(WaveformControl),
            new FrameworkPropertyMetadata(1, FrameworkPropertyMetadataOptions.AffectsRender, OnChannelCountChanged));

    public static readonly DependencyProperty IsDarkThemeProperty =
        DependencyProperty.Register(nameof(IsDarkTheme), typeof(bool), typeof(WaveformControl),
            new FrameworkPropertyMetadata(true, FrameworkPropertyMetadataOptions.AffectsRender, OnThemeChanged));

    public static readonly DependencyProperty DurationProperty =
        DependencyProperty.Register(nameof(Duration), typeof(TimeSpan), typeof(WaveformControl),
            new FrameworkPropertyMetadata(TimeSpan.Zero, FrameworkPropertyMetadataOptions.AffectsRender));

    public static readonly DependencyProperty IsPlayingProperty =
        DependencyProperty.Register(nameof(IsPlaying), typeof(bool), typeof(WaveformControl),
            new FrameworkPropertyMetadata(false, FrameworkPropertyMetadataOptions.AffectsRender));

    public float[] Peaks
    {
        get => (float[])GetValue(PeaksProperty);
        set => SetValue(PeaksProperty, value);
    }

    public double Progress
    {
        get => (double)GetValue(ProgressProperty);
        set => SetValue(ProgressProperty, Math.Clamp(value, 0, 1));
    }

    public int ChannelCount
    {
        get => (int)GetValue(ChannelCountProperty);
        set => SetValue(ChannelCountProperty, Math.Max(1, value));
    }

    public bool IsDarkTheme
    {
        get => (bool)GetValue(IsDarkThemeProperty);
        set => SetValue(IsDarkThemeProperty, value);
    }

    public TimeSpan Duration
    {
        get => (TimeSpan)GetValue(DurationProperty);
        set => SetValue(DurationProperty, value);
    }

    public bool IsPlaying
    {
        get => (bool)GetValue(IsPlayingProperty);
        set => SetValue(IsPlayingProperty, value);
    }

    public event EventHandler<double>? SeekRequested;

    public WaveformControl()
    {
        Focusable = true;
        Cursor = Cursors.Hand;
        SnapsToDevicePixels = true;
        RenderOptions.SetEdgeMode(this, EdgeMode.Aliased);
        RenderOptions.SetBitmapScalingMode(this, BitmapScalingMode.NearestNeighbor);
        MouseDown += OnMouseSeek;
        MouseMove += OnMouseMove;
        MouseLeave += OnMouseLeave;
    }

    public void SuppressSeekFor(TimeSpan duration)
    {
        _ignoreSeekUntilUtc = DateTime.UtcNow + duration;
        _isPointerDown = false;
        if (IsMouseCaptured)
        {
            ReleaseMouseCapture();
        }
    }

    protected override void OnRenderSizeChanged(SizeChangedInfo sizeInfo)
    {
        ClearRenderCache();
        base.OnRenderSizeChanged(sizeInfo);
    }

    protected override void OnRender(DrawingContext drawingContext)
    {
        var rect = new Rect(0, 0, ActualWidth, ActualHeight);
        if (rect.Width <= 0 || rect.Height <= 0)
        {
            return;
        }

        if (Peaks.Length < 2)
        {
            drawingContext.DrawRectangle(IsDarkTheme ? _darkBackground : _lightBackground, null, rect);
            DrawGrid(drawingContext, rect);
            DrawEmptyState(drawingContext, rect);
            return;
        }

        EnsureBitmaps(rect);
        if (_baseBitmap is null)
        {
            return;
        }

        drawingContext.DrawImage(_baseBitmap, rect);

        var playedWidth = Math.Round(rect.Width * Progress);
        if (playedWidth > 0 && _playedBitmap is not null)
        {
            drawingContext.PushClip(new RectangleGeometry(new Rect(0, 0, playedWidth, rect.Height)));
            drawingContext.DrawImage(_playedBitmap, rect);
            drawingContext.Pop();
        }

        DrawSeeker(drawingContext, rect, Progress, _accent, true);

        if (_isHovering)
        {
            DrawSeeker(drawingContext, rect, _hoverProgress, _hover, false);
        }
    }

    private void EnsureBitmaps(Rect rect)
    {
        var currentSize = new Size(Math.Max(1, Math.Round(rect.Width)), Math.Max(1, Math.Round(rect.Height)));
        if (_baseBitmap is not null &&
            _playedBitmap is not null &&
            _bitmapSize == currentSize &&
            _bitmapTheme == IsDarkTheme &&
            _bitmapChannelCount == ChannelCount &&
            ReferenceEquals(_bitmapPeaks, Peaks))
        {
            return;
        }

        _bitmapSize = currentSize;
        _bitmapTheme = IsDarkTheme;
        _bitmapChannelCount = ChannelCount;
        _bitmapPeaks = Peaks;
        _baseBitmap = RenderWaveBitmap(currentSize, IsDarkTheme ? _darkWave : _lightWave);
        _playedBitmap = RenderWaveBitmap(currentSize, IsDarkTheme ? _darkPlayed : _lightPlayed);
    }

    private RenderTargetBitmap RenderWaveBitmap(Size size, Brush waveBrush)
    {
        var pixelWidth = Math.Max(1, (int)size.Width);
        var pixelHeight = Math.Max(1, (int)size.Height);
        var visual = new DrawingVisual();
        RenderOptions.SetEdgeMode(visual, EdgeMode.Aliased);

        using (var context = visual.RenderOpen())
        {
            var drawRect = new Rect(0, 0, pixelWidth, pixelHeight);
            context.DrawRectangle(IsDarkTheme ? _darkBackground : _lightBackground, null, drawRect);
            DrawGrid(context, drawRect);
            DrawWaveColumns(context, drawRect, waveBrush);
        }

        var bitmap = new RenderTargetBitmap(pixelWidth, pixelHeight, 96, 96, PixelFormats.Pbgra32);
        bitmap.Render(visual);
        bitmap.Freeze();
        return bitmap;
    }

    private void DrawWaveColumns(DrawingContext drawingContext, Rect rect, Brush waveBrush)
    {
        var channelCount = GetVisibleChannelCount(rect);
        var storedChannels = Math.Max(1, ChannelCount);
        var valuesPerPoint = storedChannels * 2;
        if (Peaks.Length < valuesPerPoint)
        {
            return;
        }

        var points = Peaks.Length / valuesPerPoint;
        var width = Math.Max(1, (int)Math.Round(rect.Width));

        for (var channel = 0; channel < channelCount; channel++)
        {
            var channelRect = GetChannelRect(rect, channel, channelCount);
            var centerY = Math.Round(channelRect.Top + channelRect.Height / 2);
            var amplitude = channelRect.Height * 0.44;
            var tops = new double[width];
            var bottoms = new double[width];

            for (var x = 0; x < width; x++)
            {
                var startIndex = (int)Math.Floor(x * points / rect.Width);
                var endIndex = (int)Math.Ceiling((x + 1) * points / rect.Width);
                startIndex = Math.Clamp(startIndex, 0, points - 1);
                endIndex = Math.Clamp(endIndex, startIndex + 1, points);

                var min = 0f;
                var max = 0f;
                for (var i = startIndex; i < endIndex; i++)
                {
                    var offset = i * valuesPerPoint + channel * 2;
                    min = Math.Min(min, Peaks[offset]);
                    max = Math.Max(max, Peaks[offset + 1]);
                }

                tops[x] = centerY - max * amplitude;
                bottoms[x] = centerY - min * amplitude;
            }

            SmoothEnvelope(tops);
            SmoothEnvelope(bottoms);

            for (var x = 0; x < width; x++)
            {
                var top = Math.Round(tops[x]);
                var bottom = Math.Round(bottoms[x]);
                var height = Math.Max(1, bottom - top);
                drawingContext.DrawRectangle(waveBrush, null, new Rect(x, top, 1, height));
            }
        }
    }

    private int GetVisibleChannelCount(Rect rect)
    {
        var channelCount = Math.Max(1, ChannelCount);
        return channelCount > 1 && rect.Height / channelCount < MinChannelHeight
            ? 1
            : channelCount;
    }

    private static Rect GetChannelRect(Rect rect, int channel, int channelCount)
    {
        if (channelCount <= 1)
        {
            return rect;
        }

        var gap = 5d;
        var channelHeight = (rect.Height - gap * (channelCount - 1)) / channelCount;
        return new Rect(rect.Left, rect.Top + channel * (channelHeight + gap), rect.Width, channelHeight);
    }

    private static void SmoothEnvelope(double[] values)
    {
        if (values.Length < 3)
        {
            return;
        }

        var copy = (double[])values.Clone();
        for (var i = 0; i < values.Length; i++)
        {
            var sum = 0d;
            var weight = 0d;
            for (var offset = -1; offset <= 1; offset++)
            {
                var index = Math.Clamp(i + offset, 0, values.Length - 1);
                var currentWeight = offset == 0 ? 2 : 1;
                sum += copy[index] * currentWeight;
                weight += currentWeight;
            }

            values[i] = sum / weight;
        }
    }

    private void DrawGrid(DrawingContext drawingContext, Rect rect)
    {
        var gridPen = new Pen(IsDarkTheme ? _darkGrid : _lightGrid, 1);
        for (var i = 1; i < 8; i++)
        {
            var x = Math.Round(rect.Width * i / 8) + 0.5;
            drawingContext.DrawLine(gridPen, new Point(x, 0), new Point(x, rect.Height));
        }

        var channelCount = GetVisibleChannelCount(rect);
        for (var channel = 0; channel < channelCount; channel++)
        {
            var channelRect = GetChannelRect(rect, channel, channelCount);
            var centerY = Math.Round(channelRect.Top + channelRect.Height / 2) + 0.5;
            drawingContext.DrawLine(gridPen, new Point(0, centerY), new Point(rect.Width, centerY));
        }

        for (var channel = 1; channel < channelCount; channel++)
        {
            var y = Math.Round(rect.Height * channel / channelCount) + 0.5;
            drawingContext.DrawLine(gridPen, new Point(0, y), new Point(rect.Width, y));
        }
    }

    private void DrawSeeker(DrawingContext drawingContext, Rect rect, double progress, Brush brush, bool labelBelow)
    {
        var x = Math.Round(rect.Width * Math.Clamp(progress, 0, 1)) + 0.5;
        drawingContext.DrawLine(new Pen(brush, 1), new Point(x, 0), new Point(x, rect.Height));

        if (Duration <= TimeSpan.Zero)
        {
            return;
        }

        var text = labelBelow && !IsPlaying
            ? "Paused"
            : FormatTime(TimeSpan.FromTicks((long)(Duration.Ticks * Math.Clamp(progress, 0, 1))));
        var formatted = new FormattedText(
            text,
            CultureInfo.InvariantCulture,
            FlowDirection.LeftToRight,
            _labelTypeface,
            13,
            _labelText,
            VisualTreeHelper.GetDpi(this).PixelsPerDip);

        var paddingX = 6d;
        var paddingY = 3d;
        var labelWidth = Math.Ceiling(formatted.Width + paddingX * 2);
        var labelHeight = Math.Ceiling(formatted.Height + paddingY * 2);
        var labelX = Math.Clamp(x - labelWidth / 2, 4, Math.Max(4, rect.Width - labelWidth - 4));
        var labelY = labelBelow ? rect.Height - labelHeight - 7 : 7;
        var labelRect = new Rect(labelX, labelY, labelWidth, labelHeight);

        drawingContext.DrawRectangle(brush, null, labelRect);
        drawingContext.DrawText(formatted, new Point(labelX + paddingX, labelY + paddingY - 1));
    }

    private void DrawEmptyState(DrawingContext drawingContext, Rect rect)
    {
        var pen = new Pen(IsDarkTheme ? FrozenBrush(58, 63, 72) : FrozenBrush(200, 206, 214), 1);
        var centerY = rect.Height / 2;
        for (var x = 24d; x < rect.Width - 24; x += 22)
        {
            var height = 10 + 24 * Math.Abs(Math.Sin(x * 0.04));
            drawingContext.DrawLine(pen, new Point(Math.Round(x) + 0.5, Math.Round(centerY - height)), new Point(Math.Round(x) + 0.5, Math.Round(centerY + height)));
        }
    }

    private void OnMouseSeek(object sender, MouseButtonEventArgs e)
    {
        if (e.ClickCount > 1 || IsSeekSuppressed)
        {
            return;
        }

        CaptureMouse();
        _isPointerDown = true;
        UpdateHover(e.GetPosition(this).X);
    }

    private void OnMouseMove(object sender, MouseEventArgs e)
    {
        UpdateHover(e.GetPosition(this).X);

        if (e.LeftButton != MouseButtonState.Pressed && IsMouseCaptured)
        {
            ReleaseMouseCapture();
            _isPointerDown = false;
        }
    }

    private void OnMouseLeave(object sender, MouseEventArgs e)
    {
        _isHovering = false;
        InvalidateVisual();
    }

    protected override void OnMouseUp(MouseButtonEventArgs e)
    {
        if (_isPointerDown && !IsSeekSuppressed)
        {
            UpdateHover(e.GetPosition(this).X);
            RaiseSeek(_hoverProgress);
        }

        _isPointerDown = false;
        if (IsMouseCaptured)
        {
            ReleaseMouseCapture();
        }

        base.OnMouseUp(e);
    }

    private void UpdateHover(double x)
    {
        if (ActualWidth <= 0)
        {
            return;
        }

        _isHovering = true;
        _hoverProgress = Math.Clamp(x / ActualWidth, 0, 1);
        InvalidateVisual();
    }

    private void RaiseSeek(double progress) => SeekRequested?.Invoke(this, Math.Clamp(progress, 0, 1));

    private bool IsSeekSuppressed => DateTime.UtcNow < _ignoreSeekUntilUtc;

    private static void OnPeaksChanged(DependencyObject dependencyObject, DependencyPropertyChangedEventArgs eventArgs)
    {
        var control = (WaveformControl)dependencyObject;
        control.ClearRenderCache();
    }

    private static void OnThemeChanged(DependencyObject dependencyObject, DependencyPropertyChangedEventArgs eventArgs)
    {
        var control = (WaveformControl)dependencyObject;
        control.ClearRenderCache();
    }

    private static void OnChannelCountChanged(DependencyObject dependencyObject, DependencyPropertyChangedEventArgs eventArgs)
    {
        var control = (WaveformControl)dependencyObject;
        control.ClearRenderCache();
    }

    private void ClearRenderCache()
    {
        _baseBitmap = null;
        _playedBitmap = null;
        _bitmapPeaks = null;
        _bitmapChannelCount = 0;
    }

    private static string FormatTime(TimeSpan value) => $"{(int)value.TotalMinutes}:{value.Seconds:00}.{value.Milliseconds:000}";

    private static SolidColorBrush FrozenBrush(byte r, byte g, byte b)
    {
        var brush = new SolidColorBrush(Color.FromRgb(r, g, b));
        brush.Freeze();
        return brush;
    }
}
