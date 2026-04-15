# Under the Hood

This page provides in-depth information about the low-level implementation details of CHEM, including network protocols and high-performance optimizations.

## UDP Streaming & Fragmentation

CHEM uses a custom UDP-based protocol to stream high-bandwidth IQ data between VUSRP nodes and the emulator. This layer handles fragmentation, reassembly, and cross-channel time alignment to ensure that MIMO data remains synchronized during transport.

![UDP Fragmentation & MIMO Interleaving](assets/UDPFragmentation.gif)

### Data Layout & Interleaving

To maintain strict time-alignment across multiple MIMO channels, CHEM **interleaves** data from all channels within each UDP fragment. 

Instead of sending all samples for Channel 0 followed by all samples for Channel 1, each fragment contains a temporal "chunk" of data from all active channels. This design choice allows the receiver (`UDPServer`) to begin reassembling the waveform as soon as the first fragment arrives, and ensures that even in the case of partial packet loss, the temporal alignment between channels is preserved for the data that did arrive.

### Fragmentation (`UDPClient`)

The `UDPClient` manages the decomposition of large signal bursts that exceed the network MTU:

*   **Burst Encapsulation:** The first fragment of a burst includes the primary `Header` metadata (sample rate, original timestamp, etc.).
*   **UDP Control Header:** Every fragment is prefixed with a `UDPHeader` containing a `seq_number` for loss detection and a `flag` field:
    *   `0x1` (Start): Indicates the beginning of a signal burst.
    *   `0x2` (Data): Intermediate data fragments.
    *   `0x4` (End): The final fragment of the burst.
*   **Batch Transmission:** It utilizes the `sendmmsg` system call and `iovec` scatter/gather arrays to transmit multiple fragments in a single kernel transition.

### Reassembly (`UDPServer`)

The `UDPServer` reassembles interleaved fragments into contiguous, per-channel buffers:

*   **Ordered Assembly:** Fragments are tracked by `seq_number`. If a gap or out-of-order packet is detected, the signal burst is partially reassembled up to the last contiguous fragment.
*   **Zero-Copy De-interleaving:** As fragments are pulled using `recvmmsg`, the server calculates the destination stride and copies data directly into its final position in the reassembled buffer. This effectively de-interleaves the channels during the reassembly phase without requiring a separate memory pass.
*   **Adaptive Batching:** The server dynamically adjusts its `recvmmsg` batch size based on the remaining expected fragments in a burst to optimize for both throughput and latency.

## SIMD Optimization

Core signal processing kernels in CHEM are optimized using the [VOLK](https://github.com/gnuradio/volk) (Vector-Optimized Library of Kernels) library. VOLK auto-detects the best SIMD path at runtime (AVX2, AVX-512, NEON, etc.) via `volk_profile`, providing hardware-optimized implementations transparently.
