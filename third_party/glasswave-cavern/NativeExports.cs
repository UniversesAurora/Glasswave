using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Cavern;
using Cavern.Channels;
using Cavern.Format.Decoders;
using Cavern.Format.Renderers;
using Cavern.Format.Utilities;
using Cavern.Virtualizer;

namespace Glasswave.Cavern;

internal static unsafe class NativeExports {
    private const uint AbiVersion = 1;
    private const uint Float32Pcm = 1;
    private const uint HasJocObjects = 1;
    private const int UpdateFramesDefault = 1024;

    [UnmanagedCallersOnly(EntryPoint = "gw_atmos_abi_version",
                           CallConvs = new[] { typeof(CallConvCdecl) })]
    public static uint AbiVersionExport() => AbiVersion;

    [UnmanagedCallersOnly(EntryPoint = "gw_atmos_create",
                           CallConvs = new[] { typeof(CallConvCdecl) })]
    public static uint Create(uint abiVersion, uint outputMode, uint layout, uint updateFrames,
                              nint userData, nint readFunction, void** outHandle) {
        if (outHandle == null || readFunction == 0 || abiVersion != AbiVersion ||
            outputMode > (uint)OutputMode.Headphones ||
            layout > (uint)OutputLayout.Speakers7_1 ||
            (outputMode == (uint)OutputMode.Headphones &&
             layout != (uint)OutputLayout.Stereo2_0)) {
            return (uint)Status.InvalidArgument;
        }
        *outHandle = null;

        RendererSession? session = null;
        GCHandle handle = default;
        try {
            session = new RendererSession((OutputMode)outputMode, (OutputLayout)layout,
                                          updateFrames == 0 ? UpdateFramesDefault : (int)updateFrames,
                                          userData, readFunction);
            handle = GCHandle.Alloc(session);
            *outHandle = GCHandle.ToIntPtr(handle).ToPointer();
            return (uint)Status.Ok;
        } catch {
            if (handle.IsAllocated) handle.Free();
            session?.Dispose();
            return (uint)Status.InitializationError;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "gw_atmos_get_info",
                           CallConvs = new[] { typeof(CallConvCdecl) })]
    public static uint GetInfo(void* rawHandle, GwAtmosInfo* output) {
        if (rawHandle == null || output == null) return (uint)Status.InvalidArgument;
        try {
            RendererSession session = Resolve(rawHandle);
            return session.GetInfo(output);
        } catch {
            return (uint)Status.InternalError;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "gw_atmos_render",
                           CallConvs = new[] { typeof(CallConvCdecl) })]
    public static uint Render(void* rawHandle, float* output, uint maxFrames,
                              uint* writtenFrames, int* atEnd) {
        if (rawHandle == null || output == null || maxFrames == 0 || writtenFrames == null ||
            atEnd == null) {
            return (uint)Status.InvalidArgument;
        }
        *writtenFrames = 0;
        *atEnd = 0;
        try {
            RendererSession session = Resolve(rawHandle);
            return session.Render(output, maxFrames, writtenFrames, atEnd);
        } catch {
            return (uint)Status.InternalError;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "gw_atmos_cancel",
                           CallConvs = new[] { typeof(CallConvCdecl) })]
    public static uint Cancel(void* rawHandle) {
        if (rawHandle == null) return (uint)Status.InvalidArgument;
        try {
            Resolve(rawHandle).Cancel();
            return (uint)Status.Ok;
        } catch {
            return (uint)Status.InternalError;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "gw_atmos_destroy",
                           CallConvs = new[] { typeof(CallConvCdecl) })]
    public static uint Destroy(void* rawHandle) {
        if (rawHandle == null) return (uint)Status.InvalidArgument;
        try {
            var handle = GCHandle.FromIntPtr((nint)rawHandle);
            if (handle.Target is RendererSession session) session.Dispose();
            handle.Free();
            return (uint)Status.Ok;
        } catch {
            return (uint)Status.InternalError;
        }
    }

    private static RendererSession Resolve(void* rawHandle) {
        var handle = GCHandle.FromIntPtr((nint)rawHandle);
        return handle.Target as RendererSession ??
               throw new InvalidOperationException("Invalid Glasswave Cavern handle.");
    }
}

internal enum Status : uint {
    Ok = 0,
    Pending = 1,
    End = 2,
    InvalidArgument = 3,
    Unsupported = 4,
    Busy = 5,
    IoError = 6,
    DecodeError = 7,
    Cancelled = 8,
    InitializationError = 9,
    InternalError = 10
}

internal enum OutputMode : uint {
    Speakers = 0,
    Headphones = 1
}

internal enum OutputLayout : uint {
    Stereo2_0 = 0,
    Speakers5_1 = 1,
    Speakers7_1 = 2
}

[StructLayout(LayoutKind.Sequential)]
internal struct GwAtmosInfo {
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

internal sealed unsafe class RendererSession : IDisposable {
    private const int MaxBufferedFrames = 48000 * 30;
    private const float JocCalibrationGain = .707f;
    private const float HeadphoneHeadroomGain = .50118723f; // -6 dB
    private readonly OutputMode outputMode;
    private readonly OutputLayout outputLayout;
    private readonly int updateFrames;
    private readonly PullStream input;
    private readonly Thread worker;
    private readonly object gate = new();
    private readonly Queue<float[]> pcm = new();
    private volatile bool cancelled;
    private long bufferedFrames;
    private long producedFrames;
    private Status status = Status.Pending;
    private GwAtmosInfo info;
    private bool infoReady;
    private bool disposed;
    private int outputChannels;
    private VirtualChannel[]? headphoneFilters;

    public RendererSession(OutputMode mode, OutputLayout layout, int updateFrames,
                           nint userData, nint readFunction) {
        if (updateFrames < 1024 || updateFrames > 16384)
            throw new ArgumentOutOfRangeException(nameof(updateFrames));
        outputMode = mode;
        outputLayout = layout;
        this.updateFrames = updateFrames;
        input = new PullStream(userData, readFunction);
        worker = new Thread(Worker) {
            IsBackground = true,
            Name = "Glasswave Cavern JOC renderer"
        };
        worker.Start();
    }

    public uint GetInfo(GwAtmosInfo* output) {
        lock (gate) {
            if (status is Status.IoError or Status.DecodeError or Status.Cancelled or
                Status.InitializationError or Status.InternalError)
                return (uint)status;
            if (!infoReady) return (uint)Status.Pending;
            *output = info;
            return (uint)Status.Ok;
        }
    }

    public uint Render(float* output, uint maxFrames, uint* writtenFrames, int* atEnd) {
        lock (gate) {
            if (status is Status.IoError or Status.DecodeError or Status.Cancelled or
                Status.InitializationError or Status.InternalError)
                return (uint)status;
            if (!infoReady) return (uint)Status.Pending;

            int channels = (int)info.Channels;
            long availableFrames = bufferedFrames;
            int frames = (int)Math.Min(maxFrames, availableFrames);
            int floats = frames * channels;
            int copied = 0;
            while (copied < floats) {
                float[] chunk = pcm.Peek();
                int count = Math.Min(floats - copied, chunk.Length - chunkOffset);
                new ReadOnlySpan<float>(chunk, chunkOffset, count)
                    .CopyTo(new Span<float>(output + copied, count));
                copied += count;
                chunkOffset += count;
                if (chunkOffset == chunk.Length) {
                    pcm.Dequeue();
                    chunkOffset = 0;
                }
            }
            bufferedFrames -= frames;
            *writtenFrames = (uint)frames;
            Monitor.PulseAll(gate);

            if (frames != 0) return (uint)Status.Ok;
            if (status == Status.End && bufferedFrames == 0) {
                *atEnd = 1;
                return (uint)Status.End;
            }
            return (uint)Status.Pending;
        }
    }

    public void Cancel() {
        if (cancelled) return;
        cancelled = true;
        input.Cancel();
        lock (gate) Monitor.PulseAll(gate);
    }

    public void Dispose() {
        if (disposed) return;
        disposed = true;
        Cancel();
        if (worker.IsAlive && Thread.CurrentThread != worker) worker.Join();
    }

    private int chunkOffset;

    private void Worker() {
        EnhancedAC3Renderer? renderer = null;
        Listener? listener = null;
        try {
            BlockBuffer<byte> blocks =
                BlockBuffer<byte>.CreateForConstantPacketSize(input, 4096);
            var decoder = new EnhancedAC3Decoder(blocks);
            if (outputMode == OutputMode.Headphones)
                headphoneFilters = VirtualizerFilter.CreateDefaultFilters();

            using (ListenerAdmission.Enter(outputMode, outputLayout, decoder.SampleRate,
                                           headphoneFilters)) {
                renderer = new EnhancedAC3Renderer(decoder);
                if (!renderer.HasObjects)
                    throw new InvalidDataException("The EC-3 stream contains no JOC objects.");

                listener = new Listener(false) {
                    SampleRate = decoder.SampleRate,
                    UpdateRate = updateFrames,
                    AudioQuality = QualityModes.Perfect,
                    Volume = JocCalibrationGain *
                             (outputMode == OutputMode.Headphones
                                  ? HeadphoneHeadroomGain
                                  : 1f)
                };
                listener.AttachSources(renderer.Objects);

                ReferenceChannel[] outputLayoutChannels = LayoutReferences(outputLayout);
                outputChannels = outputLayoutChannels.Length;
                lock (gate) {
                    info = new GwAtmosInfo {
                        StructSize = (uint)sizeof(GwAtmosInfo),
                        AbiVersion = 1,
                        SampleRate = (uint)decoder.SampleRate,
                        Channels = (uint)outputChannels,
                        ChannelMask = LayoutMask(outputLayout),
                        OutputMode = (uint)outputMode,
                        Layout = (uint)outputLayout,
                        PcmFormat = 1,
                        Flags = 1,
                        DynamicObjects = (uint)renderer.DynamicObjects
                    };
                    infoReady = true;
                    status = Status.Pending;
                    Monitor.PulseAll(gate);
                }
            }

            while (!cancelled) {
                float[] rendered;
                int renderedChannels;
                bool finished;
                long decodedFrames;
                int frameSize;
                bool traceTail = false;
                using (ListenerAdmission.Enter(outputMode, outputLayout, decoder.SampleRate,
                                               headphoneFilters)) {
                    rendered = listener.Render();
                    renderedChannels = Listener.Channels.Length;
                    finished = decoder.Finished;
                    decodedFrames = decoder.DecodedFrames;
                    frameSize = decoder.FrameSize;
                    traceTail = Environment.GetEnvironmentVariable("GLASSWAVE_CAVERN_DEBUG") == "1";
                }
                if (rendered == null || rendered.Length != updateFrames * renderedChannels)
                    throw new InvalidDataException("Cavern returned an invalid render block.");
                if (renderedChannels != outputChannels) {
                    if (outputChannels != 2)
                        throw new InvalidDataException("Cavern changed an unsupported speaker layout.");
                    var stereo = new float[updateFrames * 2];
                    for (int frame = 0; frame < updateFrames; ++frame) {
                        stereo[frame * 2] = rendered[frame * renderedChannels];
                        stereo[frame * 2 + 1] = rendered[frame * renderedChannels + 1];
                    }
                    rendered = stereo;
                }
                if (finished) {
                    long available = checked(decodedFrames * frameSize - producedFrames);
                    int validFrames = (int)Math.Clamp(available, 0, updateFrames);
                    if (validFrames < updateFrames)
                        Array.Resize(ref rendered, validFrames * outputChannels);
                }
                if (!Enqueue(rendered)) break;
                if (finished && traceTail) {
                    Console.Error.WriteLine(
                        $"tail: decoded={decodedFrames * frameSize} produced={producedFrames} " +
                        $"buffered={decoder.HasBufferedSamples} ended={decoder.DecodedDataEnded}");
                }
                if (finished && decodedFrames * frameSize <= producedFrames) break;
            }

            lock (gate) {
                status = cancelled ? Status.Cancelled : Status.End;
                Monitor.PulseAll(gate);
            }
        } catch (OperationCanceledException) {
            lock (gate) {
                status = Status.Cancelled;
                Monitor.PulseAll(gate);
            }
        } catch (IOException) {
            lock (gate) {
                status = Status.IoError;
                Monitor.PulseAll(gate);
            }
        } catch (Exception error) {
            lock (gate) {
                status = Status.DecodeError;
                Monitor.PulseAll(gate);
                if (Environment.GetEnvironmentVariable("GLASSWAVE_CAVERN_DEBUG") == "1")
                    Console.Error.WriteLine($"renderer-error: {error}");
            }
        } finally {
            if (listener != null) listener.DetachAllSources();
            renderer?.Dispose();
        }
    }

    private bool Enqueue(float[] rendered) {
        int frames = rendered.Length / outputChannels;
        if (frames == 0) return true;
        lock (gate) {
            while (!cancelled && bufferedFrames + frames > MaxBufferedFrames)
                Monitor.Wait(gate);
            if (cancelled) return false;
            pcm.Enqueue((float[])rendered.Clone());
            bufferedFrames += frames;
            producedFrames += frames;
            Monitor.PulseAll(gate);
            return true;
        }
    }

    internal static ReferenceChannel[] LayoutReferences(OutputLayout layout) => layout switch {
        OutputLayout.Stereo2_0 => ChannelPrototype.ref200,
        OutputLayout.Speakers5_1 => ChannelPrototype.ref510,
        OutputLayout.Speakers7_1 => ChannelPrototype.ref710,
        _ => throw new ArgumentOutOfRangeException(nameof(layout))
    };

    private static uint LayoutMask(OutputLayout layout) => layout switch {
        OutputLayout.Stereo2_0 => 0x00003, // FL, FR
        OutputLayout.Speakers5_1 => 0x0060F, // FL, FR, FC, LFE, SL, SR
        OutputLayout.Speakers7_1 => 0x0063F, // FL, FR, FC, LFE, BL, BR, SL, SR
        _ => throw new ArgumentOutOfRangeException(nameof(layout))
    };
}

internal static class ListenerAdmission {
    private static readonly object gate = new();

    public static Lease Enter(OutputMode mode, OutputLayout layout, int sampleRate,
                              VirtualChannel[]? headphoneFilters = null) {
        // Cavern keeps its active SourceRenderer and output layout in static fields. Sessions
        // own their Listener, sources, and queues, but the render call must be process-serialized.
        Monitor.Enter(gate);
        try {
            Listener.HeadphoneVirtualizer = mode == OutputMode.Headphones;
            if (mode == OutputMode.Headphones) {
                VirtualizerFilter.Override(
                    headphoneFilters ?? throw new InvalidOperationException(
                        "A headphone context requires an independent HRIR filter set."),
                    sampleRate);
            }
            Listener.EnvironmentType = Environments.Home;
            Listener.ReplaceChannels(ChannelPrototype.ToLayout(
                RendererSession.LayoutReferences(layout)));
            return new Lease();
        } catch {
            Monitor.Exit(gate);
            throw;
        }
    }

    public static void Exit() {
        Monitor.Exit(gate);
    }

    internal readonly struct Lease : IDisposable {
        public void Dispose() => Exit();
    }
}
