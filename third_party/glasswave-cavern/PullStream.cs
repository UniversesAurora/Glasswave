using System.Runtime.InteropServices;

namespace Glasswave.Cavern;

internal sealed unsafe class PullStream : Stream {
    private readonly nint userData;
    private readonly nint readFunction;
    private readonly object gate = new();
    private bool cancelled;
    private bool ended;

    public PullStream(nint userData, nint readFunction) {
        this.userData = userData;
        this.readFunction = readFunction;
    }

    public override bool CanRead => true;
    public override bool CanSeek => false;
    public override bool CanWrite => false;
    public override long Length => throw new NotSupportedException();

    public override long Position {
        get => throw new NotSupportedException();
        set => throw new NotSupportedException();
    }

    public void Cancel() {
        lock (gate) {
            cancelled = true;
            Monitor.PulseAll(gate);
        }
    }

    public override int Read(byte[] buffer, int offset, int count) {
        ArgumentNullException.ThrowIfNull(buffer);
        ArgumentOutOfRangeException.ThrowIfNegative(offset);
        ArgumentOutOfRangeException.ThrowIfNegative(count);
        if (offset > buffer.Length || count > buffer.Length - offset)
            throw new ArgumentOutOfRangeException(nameof(count));
        if (count == 0) return 0;

        lock (gate) {
            if (cancelled) throw new OperationCanceledException();
            if (ended) return 0;
        }

        int received;
        fixed (byte* destination = buffer) {
            var callback = (delegate* unmanaged[Cdecl]<nint, byte*, nuint, nint>)readFunction;
            nint result = callback(userData, destination + offset, (nuint)count);
            if (result < 0) {
                lock (gate) {
                    if (cancelled) throw new OperationCanceledException();
                }
                throw new IOException("The native EC-3 pull callback failed.");
            }
            if ((nuint)result > (nuint)count)
                throw new IOException("The native EC-3 pull callback returned an invalid byte count.");
            received = (int)result;
        }

        lock (gate) {
            if (received == 0) ended = true;
        }
        return received;
    }

    public override void Flush() {}
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
}
