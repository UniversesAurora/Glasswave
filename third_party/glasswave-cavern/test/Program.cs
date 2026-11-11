using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;

internal static unsafe class Program {
    private static FileStream?[] inputs = Array.Empty<FileStream?>();

    private static int Main(string[] args) {
        string? dllPath = null;
        string? ec3Path = null;
        uint layout = 0;
        uint layoutB = uint.MaxValue;
        uint mode = 0;
        int sessions = 2;
        long? expectedFrames = null;
        for (int i = 0; i < args.Length; ++i) {
            if (args[i] == "--dll" && i + 1 < args.Length) dllPath = args[++i];
            else if (args[i] == "--ec3" && i + 1 < args.Length) ec3Path = args[++i];
            else if (args[i] == "--layout" && i + 1 < args.Length) layout = uint.Parse(args[++i]);
            else if (args[i] == "--layout-b" && i + 1 < args.Length) layoutB = uint.Parse(args[++i]);
            else if (args[i] == "--mode" && i + 1 < args.Length) mode = uint.Parse(args[++i]);
            else if (args[i] == "--sessions" && i + 1 < args.Length) sessions = int.Parse(args[++i]);
            else if (args[i] == "--expected-frames" && i + 1 < args.Length)
                expectedFrames = long.Parse(args[++i]);
        }
        if (dllPath == null || ec3Path == null || layout > 2 ||
            layoutB != uint.MaxValue && layoutB > 2 || mode > 1 ||
            mode == 1 && (layout != 0 || layoutB != uint.MaxValue) ||
            sessions < 1 || sessions > 4) {
            Console.Error.WriteLine(
                "Usage: Glasswave.Cavern.Probe --dll <renderer> --ec3 <file> " +
                "[--layout 0|1|2] [--sessions 1..4] [--expected-frames N]");
            return 2;
        }

        inputs = Enumerable.Range(0, sessions + 1)
            .Select(_ => new FileStream(ec3Path, FileMode.Open, FileAccess.Read, FileShare.Read,
                                        64 * 1024, FileOptions.SequentialScan))
            .Cast<FileStream?>()
            .ToArray();
        using NativeLibraryScope library = NativeLibraryScope.Load(dllPath);
        var version = (delegate* unmanaged[Cdecl]<uint>)library.Export("gw_atmos_abi_version");
        var create = (delegate* unmanaged[Cdecl]<uint, uint, uint, uint, nint, nint, void**, uint>)
            library.Export("gw_atmos_create");
        var getInfo = (delegate* unmanaged[Cdecl]<void*, Info*, uint>)library.Export("gw_atmos_get_info");
        var render = (delegate* unmanaged[Cdecl]<void*, float*, uint, uint*, int*, uint>)
            library.Export("gw_atmos_render");
        var cancel = (delegate* unmanaged[Cdecl]<void*, uint>)library.Export("gw_atmos_cancel");
        var destroy = (delegate* unmanaged[Cdecl]<void*, uint>)library.Export("gw_atmos_destroy");

        if (version() != 1) throw new InvalidOperationException("Unexpected renderer ABI version.");

        var runs = new List<Run>();
        try {
            for (int i = 0; i < sessions; ++i) {
                uint sessionLayout = i == 0 || layoutB == uint.MaxValue ? layout : layoutB;
                void* handle = null;
                uint created = create(1, mode, sessionLayout, 1024, i + 1,
                                      (nint)(delegate* unmanaged[Cdecl]<nint, byte*, nuint, nint>)&Read,
                                      &handle);
                if (created != 0 || handle == null)
                    throw new InvalidOperationException(
                        $"gw_atmos_create session {i} failed with code {created}.");
                runs.Add(new Run(handle, sessionLayout));
            }

            void* invalidHandle = null;
            uint invalidStatus = create(1, mode, 99, 1024, sessions + 2,
                                        (nint)(delegate* unmanaged[Cdecl]<nint, byte*, nuint, nint>)&Read,
                                        &invalidHandle);
            if (invalidStatus != 3 || invalidHandle != null)
                throw new InvalidOperationException(
                    $"Invalid context unexpectedly initialized: status {invalidStatus}.");

            foreach (Run run in runs) {
                Info info = default;
                while (getInfo(run.Handle, &info) == 1) Thread.Sleep(2);
                run.Info = info;
                ValidateInfo(info, run.Layout, mode);
            }

            void* cancelledHandle = null;
            try {
                uint created = create(1, mode, layout, 1024, sessions + 1,
                                      (nint)(delegate* unmanaged[Cdecl]<nint, byte*, nuint, nint>)&Read,
                                      &cancelledHandle);
                if (created != 0 || cancelledHandle == null)
                    throw new InvalidOperationException(
                        $"gw_atmos_create cancellation context failed with code {created}.");
                Info cancelledInfo = default;
                while (getInfo(cancelledHandle, &cancelledInfo) == 1) Thread.Sleep(2);
                ValidateInfo(cancelledInfo, layout, mode);
                if (cancel(cancelledHandle) != 0)
                    throw new InvalidOperationException("gw_atmos_cancel failed.");
            } finally {
                if (cancelledHandle != null) {
                    cancel(cancelledHandle);
                    destroy(cancelledHandle);
                }
            }

            int running = runs.Count;
            long frameLimit = expectedFrames ?? 48000L * 60 * 60 * 12;
            while (running != 0) {
                foreach (Run run in runs) {
                    if (!run.Running) continue;
                    float[] pcm = run.Pcm;
                    uint written = 0;
                    int atEnd = 0;
                    uint result;
                    fixed (float* samples = pcm) {
                        result = render(run.Handle, samples, 4096, &written, &atEnd);
                    }
                    if (result == 1) continue;
                    if (result == 2 && atEnd != 0) {
                        run.Running = false;
                        --running;
                        continue;
                    }
                    if (result != 0)
                        throw new InvalidOperationException(
                            $"gw_atmos_render failed with code {result}.");
                    if (run.Frames + written > frameLimit)
                        throw new InvalidOperationException(
                            $"Renderer exceeded the expected PCM length: {run.Frames + written} > {frameLimit}.");
                    run.Accumulate(pcm, written, (int)run.Info.Channels);
                }
            }

            long commonFrames = runs[0].Frames;
            if (runs.Any(run => run.Frames != commonFrames))
                throw new InvalidOperationException("Interleaved decoder contexts produced different lengths.");
            if (runs.Select(run => run.Layout).Distinct().Count() == 1) {
                double referenceRms =
                    Math.Sqrt(runs[0].SumSquares / Math.Max(1L, runs[0].Frames * runs[0].Info.Channels));
                foreach (Run run in runs.Skip(1)) {
                    double rms =
                        Math.Sqrt(run.SumSquares / Math.Max(1L, run.Frames * run.Info.Channels));
                    if (Math.Abs(run.Peak - runs[0].Peak) > 1e-5 ||
                        Math.Abs(rms - referenceRms) > 1e-7)
                        throw new InvalidOperationException(
                            "Interleaved contexts produced different PCM statistics.");
                }
            }
            foreach (Run run in runs) run.FinishHash();

            var report = new {
                ok = true,
                mode,
                layout,
                sessions = runs.Count,
                sampleRate = runs[0].Info.SampleRate,
                channels = runs[0].Info.Channels,
                channelMask = runs[0].Info.ChannelMask,
                hasJocObjects = (runs[0].Info.Flags & 1) != 0,
                dynamicObjects = runs[0].Info.DynamicObjects,
                frames = runs[0].Frames,
                expectedFrames,
                decodedSeconds = runs[0].Frames / 48000.0,
                runs = runs.Select(run => new {
                    layout = run.Layout,
                    channels = run.Info.Channels,
                    channelMask = run.Info.ChannelMask,
                    frames = run.Frames,
                    pcmSha256 = run.PcmSha256,
                    peak = run.Peak,
                    rms = Math.Sqrt(run.SumSquares / Math.Max(1L, run.Frames * run.Info.Channels))
                })
            };
            Console.WriteLine(JsonSerializer.Serialize(report));
            bool lengthValid = expectedFrames.HasValue ?
                runs.All(run => run.Frames == expectedFrames.Value) :
                runs[0].Frames / 48000.0 >= 180;
            return lengthValid ? 0 : 1;
        } finally {
            foreach (Run run in runs) {
                cancel(run.Handle);
                destroy(run.Handle);
                run.Dispose();
            }
            foreach (FileStream? input in inputs) input?.Dispose();
            inputs = Array.Empty<FileStream?>();
        }
    }

    private static void ValidateInfo(Info info, uint layout, uint mode) {
        uint expectedChannels = layout switch { 0 => 2u, 1 => 6u, 2 => 8u, _ => 0 };
        uint expectedMask = layout switch { 0 => 0x3u, 1 => 0x60Fu, 2 => 0x63Fu, _ => 0 };
        if (info.SampleRate != 48000 || info.Channels != expectedChannels ||
            info.ChannelMask != expectedMask || info.OutputMode != mode || info.Layout != layout ||
            info.PcmFormat != 1 || (info.Flags & 1) == 0) {
            throw new InvalidOperationException(
                $"Unexpected renderer format: {info.SampleRate} Hz, {info.Channels} ch, " +
                $"mask 0x{info.ChannelMask:X}, flags 0x{info.Flags:X}.");
        }
    }

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvCdecl) })]
    private static nint Read(nint userData, byte* destination, nuint capacity) {
        try {
            int index = checked((int)userData - 1);
            int count = (int)Math.Min(capacity, 1u << 20);
            return inputs[index]!.Read(new Span<byte>(destination, count));
        } catch {
            return -1;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Info {
        public uint StructSize;
        public uint AbiVersion;
        public uint SampleRate;
        public uint Channels;
        public uint ChannelMask;
        public uint OutputMode;
        public uint Layout;
        public uint PcmFormat;
        public uint Flags;
        public uint DynamicObjects;
    }

    private sealed class Run : IDisposable {
        private readonly IncrementalHash hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        public Run(void* handle, uint layout) {
            Handle = handle;
            Layout = layout;
            Pcm = new float[4096 * 8];
        }

        public void* Handle { get; }
        public uint Layout { get; }
        public Info Info;
        public float[] Pcm { get; }
        public string? PcmSha256 { get; private set; }
        public long Frames;
        public double SumSquares;
        public float Peak;
        public bool Running { get; set; } = true;

        public void Accumulate(float[] pcm, uint frames, int channels) {
            int samples = checked((int)frames * channels);
            for (int i = 0; i < samples; ++i) {
                float sample = pcm[i];
                Peak = Math.Max(Peak, Math.Abs(sample));
                SumSquares += sample * (double)sample;
            }
            hash.AppendData(MemoryMarshal.AsBytes(pcm.AsSpan(0, samples)));
            Frames += frames;
        }

        public void FinishHash() => PcmSha256 ??= Convert.ToHexString(hash.GetHashAndReset());

        public void Dispose() => hash.Dispose();
    }

    private sealed class NativeLibraryScope : IDisposable {
        private readonly nint handle;
        private NativeLibraryScope(nint handle) => this.handle = handle;

        public static NativeLibraryScope Load(string path) =>
            new(NativeLibrary.Load(Path.GetFullPath(path)));

        public nint Export(string name) =>
            NativeLibrary.GetExport(handle, name);

        // NativeAOT runtime initialization makes unloading unsafe. The probe is a
        // short-lived process, so leave the shared library resident until exit.
        public void Dispose() {}
    }
}
