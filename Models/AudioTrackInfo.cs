namespace AudioWavePlayer.Models;

public sealed record AudioTrackInfo(
    string FileName,
    string Extension,
    long SizeBytes,
    TimeSpan Duration,
    int SampleRate,
    int Channels,
    int BitsPerSample,
    int ApproxBitrateKbps,
    bool IsVariableBitrate)
{
    public string FormatName => Extension.TrimStart('.').ToUpperInvariant();

    public string FormatLine
    {
        get
        {
            var channelText = Channels switch
            {
                1 => "Mono",
                2 => "Stereo",
                _ => $"{Channels} ch"
            };

            var quality = Extension.Equals(".wav", StringComparison.OrdinalIgnoreCase) ||
                          Extension.Equals(".flac", StringComparison.OrdinalIgnoreCase)
                ? "Lossless"
                : "Lossy";

            var bitrateText = IsVariableBitrate ? "VBR" : $"{ApproxBitrateKbps} kbps";
            var sampleSizeText = Extension.Equals(".wav", StringComparison.OrdinalIgnoreCase)
                ? $", {BitsPerSample}-bit"
                : "";
            return $"{SampleRate} Hz, {channelText}, {quality}, {bitrateText}{sampleSizeText}";
        }
    }
}
