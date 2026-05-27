using System.Diagnostics;
using System.IO;
using System.Security.Cryptography;
using NAudio.Wave;
using AudioWavePlayer.Models;

namespace AudioWavePlayer.Services;

public sealed class FfmpegService
{
    private static readonly string[] SupportedExtensions = [".wav", ".mp3", ".m4a", ".flac", ".ogg"];
    private const string DecodeCacheVersion = "pcm-s16le-44100-stereo-v3";
    private const string WaveformCacheVersion = "waveform-rms-v1";
    private const int WaveformCacheMagic = 0x46574150;
    private readonly string _ffmpegPath;

    public FfmpegService()
    {
        _ffmpegPath = ResolveExecutable("ffmpeg.exe")
            ?? throw new FileNotFoundException("ffmpeg.exe was not found. Put it near the app in ./ffmpeg/ffmpeg.exe or add FFmpeg to PATH.");
    }

    public static bool IsSupported(string path)
    {
        var extension = Path.GetExtension(path);
        return SupportedExtensions.Contains(extension, StringComparer.OrdinalIgnoreCase);
    }

    public async Task<string> DecodeToPortableWavAsync(string sourcePath, CancellationToken cancellationToken)
    {
        var outputPath = GetCachePath(sourcePath);
        if (File.Exists(outputPath) && IsPlaybackCompatibleWav(outputPath))
        {
            return outputPath;
        }

        if (File.Exists(outputPath))
        {
            File.Delete(outputPath);
        }

        Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);

        var result = await RunAsync(
            _ffmpegPath,
            ["-hide_banner", "-y", "-i", sourcePath, "-vn", "-map_metadata", "-1", "-ac", "2", "-ar", "44100", "-acodec", "pcm_s16le", "-f", "wav", outputPath],
            cancellationToken);
        if (result.ExitCode != 0 || !File.Exists(outputPath))
        {
            throw new InvalidOperationException($"FFmpeg failed to decode the file.{Environment.NewLine}{result.Error}");
        }

        return outputPath;
    }

    public AudioTrackInfo ReadTrackInfo(string originalPath, string decodedWavPath)
    {
        var infoPath = Path.GetExtension(originalPath).Equals(".wav", StringComparison.OrdinalIgnoreCase)
            ? originalPath
            : decodedWavPath;
        using var reader = new WaveFileReader(infoPath);
        var duration = reader.TotalTime;
        var sampleRate = reader.WaveFormat.SampleRate;
        var channels = reader.WaveFormat.Channels;
        var bits = reader.WaveFormat.BitsPerSample;
        var sizeBytes = new FileInfo(originalPath).Length;
        var bitrate = duration.TotalSeconds <= 0
            ? 0
            : (int)Math.Round(sizeBytes * 8d / duration.TotalSeconds / 1000d);

        return new AudioTrackInfo(
            Path.GetFileName(originalPath),
            Path.GetExtension(originalPath),
            sizeBytes,
            duration,
            sampleRate,
            channels,
            bits,
            bitrate,
            IsVariableBitrateMp3(originalPath));
    }

    public float[] BuildWaveformPeaks(string decodedWavPath, int targetPoints = 16000)
    {
        using var reader = new WaveFileReader(decodedWavPath);
        EnsureSupportedWaveformFormat(reader.WaveFormat);

        var bytesPerSample = reader.WaveFormat.BitsPerSample / 8;
        var channels = reader.WaveFormat.Channels;
        var totalFrames = reader.Length / reader.BlockAlign;
        var framesPerPoint = Math.Max(1, totalFrames / targetPoints);
        var peaks = new List<float>((int)Math.Min(targetPoints, totalFrames) * 2);
        var buffer = new byte[Math.Min(reader.BlockAlign * framesPerPoint, 1024 * 256)];

        long framesInBucket = 0;
        float min = 0;
        float max = 0;

        int read;
        while ((read = reader.Read(buffer, 0, buffer.Length)) > 0)
        {
            var framesRead = read / reader.BlockAlign;
            for (var frame = 0; frame < framesRead; frame++)
            {
                float mixed = 0;
                for (var channel = 0; channel < channels; channel++)
                {
                    var offset = frame * reader.BlockAlign + channel * bytesPerSample;
                    var sample = ReadSample(buffer, offset, reader.WaveFormat);
                    mixed += sample;
                }

                mixed /= channels;
                min = Math.Min(min, mixed);
                max = Math.Max(max, mixed);
                framesInBucket++;

                if (framesInBucket >= framesPerPoint)
                {
                    peaks.Add(min);
                    peaks.Add(max);
                    min = 0;
                    max = 0;
                    framesInBucket = 0;
                }
            }
        }

        if (framesInBucket > 0)
        {
            peaks.Add(min);
            peaks.Add(max);
        }

        return peaks.ToArray();
    }

    public WaveformCacheData GetOrBuildWaveformData(string sourcePath, string decodedWavPath, CancellationToken cancellationToken)
    {
        var cachePath = GetWaveformCachePath(sourcePath);
        if (TryReadWaveformCache(cachePath, out var cached))
        {
            return cached;
        }

        cancellationToken.ThrowIfCancellationRequested();
        var peaks = BuildWaveformPeaks(decodedWavPath);
        cancellationToken.ThrowIfCancellationRequested();
        var rmsLevels = BuildRmsLevels(decodedWavPath);
        var data = new WaveformCacheData(peaks, rmsLevels);
        WriteWaveformCache(cachePath, data);
        return data;
    }

    public float[] BuildRmsLevels(string decodedWavPath, int targetPoints = 48000)
    {
        using var reader = new WaveFileReader(decodedWavPath);
        EnsureSupportedWaveformFormat(reader.WaveFormat);

        var channels = Math.Min(2, reader.WaveFormat.Channels);
        var sourceChannels = reader.WaveFormat.Channels;
        var bytesPerSample = reader.WaveFormat.BitsPerSample / 8;
        var totalFrames = reader.Length / reader.BlockAlign;
        var framesPerPoint = Math.Max(1, totalFrames / targetPoints);
        var levels = new List<float>((int)Math.Min(targetPoints, totalFrames) * 2);
        var buffer = new byte[Math.Min(reader.BlockAlign * framesPerPoint, 1024 * 256)];

        var sums = new double[2];
        long framesInBucket = 0;

        int read;
        while ((read = reader.Read(buffer, 0, buffer.Length)) > 0)
        {
            var framesRead = read / reader.BlockAlign;
            for (var frame = 0; frame < framesRead; frame++)
            {
                for (var channel = 0; channel < channels; channel++)
                {
                    var offset = frame * reader.BlockAlign + channel * bytesPerSample;
                    var sample = ReadSample(buffer, offset, reader.WaveFormat);
                    sums[channel] += sample * sample;
                }

                if (sourceChannels == 1)
                {
                    sums[1] = sums[0];
                }

                framesInBucket++;
                if (framesInBucket >= framesPerPoint)
                {
                    AddRmsLevel(levels, sums, framesInBucket);
                    Array.Clear(sums);
                    framesInBucket = 0;
                }
            }
        }

        if (framesInBucket > 0)
        {
            AddRmsLevel(levels, sums, framesInBucket);
        }

        return levels.ToArray();
    }

    private static void AddRmsLevel(List<float> levels, double[] sums, long frames)
    {
        var left = Math.Sqrt(sums[0] / frames);
        var right = Math.Sqrt(sums[1] / frames);
        levels.Add((float)Math.Clamp(left, 0, 1));
        levels.Add((float)Math.Clamp(right, 0, 1));
    }

    private static void EnsureSupportedWaveformFormat(WaveFormat format)
    {
        var isPcm = format.Encoding is WaveFormatEncoding.Pcm or WaveFormatEncoding.Extensible;
        var isFloat = format.Encoding == WaveFormatEncoding.IeeeFloat;
        var supportedBits = format.BitsPerSample is 16 or 24 or 32;

        if ((!isPcm && !isFloat) || !supportedBits)
        {
            throw new InvalidOperationException($"Unsupported decoded WAV format: {format.Encoding}, {format.BitsPerSample}-bit.");
        }
    }

    private static float ReadSample(byte[] buffer, int offset, WaveFormat format)
    {
        if (format.Encoding == WaveFormatEncoding.IeeeFloat && format.BitsPerSample == 32)
        {
            return Math.Clamp(BitConverter.ToSingle(buffer, offset), -1f, 1f);
        }

        return format.BitsPerSample switch
        {
            16 => BitConverter.ToInt16(buffer, offset) / 32768f,
            24 => ReadInt24(buffer, offset) / 8388608f,
            32 => BitConverter.ToInt32(buffer, offset) / 2147483648f,
            _ => 0f
        };
    }

    private static int ReadInt24(byte[] buffer, int offset)
    {
        var value = buffer[offset] | (buffer[offset + 1] << 8) | (buffer[offset + 2] << 16);
        return (value & 0x800000) != 0 ? value | unchecked((int)0xFF000000) : value;
    }

    private static string GetCachePath(string sourcePath)
    {
        var fileInfo = new FileInfo(sourcePath);
        var input = $"{DecodeCacheVersion}|{fileInfo.FullName}|{fileInfo.Length}|{fileInfo.LastWriteTimeUtc.Ticks}";
        var hash = Convert.ToHexString(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(input)))[..16];
        var cacheRoot = ResolveCacheRoot();
        return Path.Combine(cacheRoot, $"{hash}.wav");
    }

    private static string GetWaveformCachePath(string sourcePath)
    {
        var fileInfo = new FileInfo(sourcePath);
        var input = $"{WaveformCacheVersion}|{fileInfo.FullName}|{fileInfo.Length}|{fileInfo.LastWriteTimeUtc.Ticks}";
        var hash = Convert.ToHexString(SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(input)))[..16];
        return Path.Combine(ResolveWaveformCacheRoot(), $"{hash}.awf");
    }

    private static bool TryReadWaveformCache(string cachePath, out WaveformCacheData data)
    {
        data = new WaveformCacheData([], []);
        try
        {
            if (!File.Exists(cachePath))
            {
                return false;
            }

            using var stream = File.OpenRead(cachePath);
            using var reader = new BinaryReader(stream);
            if (reader.ReadInt32() != WaveformCacheMagic)
            {
                return false;
            }

            var version = reader.ReadString();
            if (version != WaveformCacheVersion)
            {
                return false;
            }

            var peaks = ReadFloatArray(reader);
            var rmsLevels = ReadFloatArray(reader);
            data = new WaveformCacheData(peaks, rmsLevels);
            return peaks.Length > 0 && rmsLevels.Length > 0;
        }
        catch
        {
            return false;
        }
    }

    private static void WriteWaveformCache(string cachePath, WaveformCacheData data)
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(cachePath)!);
            using var stream = File.Create(cachePath);
            using var writer = new BinaryWriter(stream);
            writer.Write(WaveformCacheMagic);
            writer.Write(WaveformCacheVersion);
            WriteFloatArray(writer, data.Peaks);
            WriteFloatArray(writer, data.RmsLevels);
        }
        catch
        {
            // Waveform cache is an optimization; playback must not depend on it.
        }
    }

    private static float[] ReadFloatArray(BinaryReader reader)
    {
        var length = reader.ReadInt32();
        if (length <= 0 || length > 2_000_000)
        {
            throw new InvalidDataException("Invalid waveform cache length.");
        }

        var values = new float[length];
        for (var i = 0; i < values.Length; i++)
        {
            values[i] = reader.ReadSingle();
        }

        return values;
    }

    private static void WriteFloatArray(BinaryWriter writer, float[] values)
    {
        writer.Write(values.Length);
        foreach (var value in values)
        {
            writer.Write(value);
        }
    }

    private static bool IsPlaybackCompatibleWav(string path)
    {
        try
        {
            using var reader = new WaveFileReader(path);
            return reader.WaveFormat.Encoding == WaveFormatEncoding.Pcm &&
                   reader.WaveFormat.BitsPerSample == 16 &&
                   reader.WaveFormat.Channels == 2 &&
                   reader.WaveFormat.SampleRate == 44100;
        }
        catch
        {
            return false;
        }
    }

    private static bool IsVariableBitrateMp3(string path)
    {
        if (!Path.GetExtension(path).Equals(".mp3", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        try
        {
            var data = File.ReadAllBytes(path);
            var offset = SkipId3v2(data);
            for (var i = offset; i < data.Length - 4; i++)
            {
                if (data[i] != 0xFF || (data[i + 1] & 0xE0) != 0xE0)
                {
                    continue;
                }

                var versionBits = (data[i + 1] >> 3) & 0x03;
                var layerBits = (data[i + 1] >> 1) & 0x03;
                if (versionBits == 1 || layerBits != 1)
                {
                    continue;
                }

                var channelMode = (data[i + 3] >> 6) & 0x03;
                var isMpeg1 = versionBits == 3;
                var sideInfoSize = isMpeg1
                    ? channelMode == 3 ? 17 : 32
                    : channelMode == 3 ? 9 : 17;
                var xingOffset = i + 4 + sideInfoSize;

                if (HasAscii(data, xingOffset, "Xing") || HasAscii(data, xingOffset, "Info"))
                {
                    return HasAscii(data, xingOffset, "Xing");
                }

                if (HasAscii(data, i + 4 + 32, "VBRI"))
                {
                    return true;
                }

                return false;
            }
        }
        catch
        {
            return false;
        }

        return false;
    }

    private static int SkipId3v2(byte[] data)
    {
        if (data.Length < 10 ||
            data[0] != 'I' ||
            data[1] != 'D' ||
            data[2] != '3')
        {
            return 0;
        }

        var size =
            ((data[6] & 0x7F) << 21) |
            ((data[7] & 0x7F) << 14) |
            ((data[8] & 0x7F) << 7) |
            (data[9] & 0x7F);
        return Math.Min(data.Length, 10 + size);
    }

    private static bool HasAscii(byte[] data, int offset, string value)
    {
        if (offset < 0 || offset + value.Length > data.Length)
        {
            return false;
        }

        for (var i = 0; i < value.Length; i++)
        {
            if (data[offset + i] != value[i])
            {
                return false;
            }
        }

        return true;
    }

    private static string ResolveCacheRoot()
    {
        var portableCache = Path.Combine(AppContext.BaseDirectory, "cache", "decoded");
        try
        {
            Directory.CreateDirectory(portableCache);
            return portableCache;
        }
        catch
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "AudioWavePlayer", "decoded");
        }
    }

    public void ClearWaveformCache()
    {
        var cacheRoot = ResolveWaveformCacheRoot();
        try
        {
            if (Directory.Exists(cacheRoot))
            {
                Directory.Delete(cacheRoot, true);
            }
        }
        catch
        {
            // Best effort cleanup on app shutdown.
        }
    }

    private static string ResolveWaveformCacheRoot()
    {
        var portableCache = Path.Combine(AppContext.BaseDirectory, "cache", "waveforms");
        try
        {
            Directory.CreateDirectory(portableCache);
            return portableCache;
        }
        catch
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "AudioWavePlayer", "waveforms");
        }
    }

    private static string? ResolveExecutable(string fileName)
    {
        var baseDirectory = AppContext.BaseDirectory;
        var candidates = new[]
        {
            Path.Combine(baseDirectory, "ffmpeg", fileName),
            Path.Combine(baseDirectory, fileName),
            Path.Combine(Environment.CurrentDirectory, "ffmpeg", fileName),
            Path.Combine(Environment.CurrentDirectory, fileName)
        };

        foreach (var candidate in candidates)
        {
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        var path = Environment.GetEnvironmentVariable("PATH");
        if (string.IsNullOrWhiteSpace(path))
        {
            return null;
        }

        foreach (var directory in path.Split(Path.PathSeparator))
        {
            try
            {
                var candidate = Path.Combine(directory, fileName);
                if (File.Exists(candidate))
                {
                    return candidate;
                }
            }
            catch
            {
                // Ignore malformed PATH entries.
            }
        }

        return null;
    }

    private static async Task<ProcessResult> RunAsync(string executable, IReadOnlyList<string> arguments, CancellationToken cancellationToken)
    {
        var startInfo = new ProcessStartInfo
        {
            FileName = executable,
            UseShellExecute = false,
            RedirectStandardError = true,
            RedirectStandardOutput = true,
            CreateNoWindow = true
        };

        foreach (var argument in arguments)
        {
            startInfo.ArgumentList.Add(argument);
        }

        using var process = Process.Start(startInfo) ?? throw new InvalidOperationException("Could not start FFmpeg.");
        var stdoutTask = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var stderrTask = process.StandardError.ReadToEndAsync(cancellationToken);

        await process.WaitForExitAsync(cancellationToken);
        return new ProcessResult(process.ExitCode, await stdoutTask, await stderrTask);
    }

    private sealed record ProcessResult(int ExitCode, string Output, string Error);

    public sealed record WaveformCacheData(float[] Peaks, float[] RmsLevels);
}
