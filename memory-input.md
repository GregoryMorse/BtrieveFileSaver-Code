# Memory input and bounded processing

The existing BF_OPEN, BF_GET_REC and BF_CLOSE entry points remain available. Recompile clients against the updated header: CLIENT_STRUCT has grown, so this is not a drop-in binary ABI update.

- BF_OPEN_MEMORY(client, bytes, length, options) borrows read-only input until BF_CLOSE. It never opens a temporary file.
- BF_OPEN_WITH_OPTIONS(client, path, options) reads a file using the same options.
- BF_READ_OPTIONS.maximum_file_bytes rejects oversized input before page discovery and allocation. Zero preserves the unrestricted policy, within the reader's addressable range.
- BF_READ_OPTIONS.progress(context, units) returns nonzero to continue, zero to abort with PROCESSING_LIMIT. It is called during reads, page discovery/sorting, record iteration, fragment-table traversal and cycle tracking.

The caller owns the input memory and any decrypted material. Keep it alive and unchanged until the client is closed, then wipe it if required. Work units are deterministic estimates, not elapsed time. A callback may enforce a deadline or cancellation token. The embedding application must also cap aggregate decoded output and discard partial results on errors. A callback cannot interrupt a blocked operating-system read.

A zero page/fragment pair denotes an absent variable portion, not the FCR. Other invalid pointers, out-of-page fragments and cycles remain errors. Cycle tracking mixes all pointer bits rather than clustering fragments that share an index.

The on-disk FCR remains byte-packed, while RAM-only client state explicitly uses native alignment. The regression checks the disk offsets and alignment of client members whose addresses are used by the page tracker.

## Standalone regression

Compile BtrieveFileSaverLib.c and tests/memory_input_test.c together. Pass one disposable output filename to the executable. The test creates and removes that file. It checks file/memory parity, unchanged input, size limits, callback cancellation, cleanup/reuse, null variable pointers and malformed chains.

Clang/GCC sanitizer build:

    cc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer BtrieveFileSaverLib.c tests/memory_input_test.c -o memory_input_test
    ./memory_input_test disposable-test.btr

MSVC x64 developer prompt:

    cl /nologo /O1 /Zi /fsanitize=address BtrieveFileSaverLib.c tests/memory_input_test.c /Fe:memory_input_test.exe
    memory_input_test.exe disposable-test.btr
