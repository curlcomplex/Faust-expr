# Frozen checkpoint21 native compilation fixture

This losslessly compressed source-only fixture contains twelve files copied byte-for-byte from the delivered `checkpoint21-balanced-streaming-source.zip`. The original archive SHA256 is `6f788570908bd6ec39962dcbaf050c05652e07307d1fcf5e2993447892bea589`.

Packet SHA256: `ce05ec79603e11fd8a85902a89b4b5920141352fc662f7413d42de7c3d49ed83` (26,136 bytes). Git blob: `807c28b9dffb1a5b1f4afd55a5f8e01237e39404`.

Includes the actual persistent native core, balanced scheduler, mathematical coefficient sources, Core Audio adapter, portable host and worker-hook test. There are no precompiled binaries, shell substitutions, source recordings or numerical fitted model arrays in this packet. The Linux-only optional glibc accelerator is excluded; macOS uses its existing platform-libm path.

The CI job extracts this exact source into the build directory, compiles it on Apple Silicon, and exercises an explicitly synthetic 16-component fixture. The fixture checks block partitioning, worker scheduling, overlap, contact and worker lifecycle. It is NOT a reduced candidate cymbal and does not establish the throughput, sound or real-time behaviour of the complete 7,260-component instrument.

The complete source/data archive remains required for full-instrument Mac benchmarks. No original production DSP is changed by this fixture.