# DeadSpeak

DeadSpeak is a native Android fieldwork and evidence-recording app for paranormal investigations. It measures device sensors and records observations; it does not claim to prove or communicate with spirits. Sensor readings and generated words can have ordinary environmental causes, so treat them as leads to investigate rather than conclusions.

## Current features

- Native C23 Android UI using OpenGL ES 2, built with Zig's `zig cc` frontend with zero Java/Kotlin/JVM dependencies.
- Adaptive portrait, landscape, and resizable-window layouts with shared drawing/touch geometry, scrollable evidence logs and field notes, and viewport-sized Explore pagination.
- **Formant Voice Synthesizer, Tuneable Presets & Audio Player**: Pure C23 formant resonator synthesizer outputting through low-latency native `AAudio`, with live tuning in the `GUIDE` tab (`60–300 Hz` pitch, `0.50–2.50X` speed, interactive `"DEADSPEAK SHADOW"` test) and a `VOX GATE` self-speech suppression guard (`VOX GATED (HP)` HUD badge). Features four acoustic voicing profiles:
  - `CLASSIC`: Resonant vocal tract glottal impulse filtering.
  - `WHISPER`: Unvoiced spectral noise shaped through resonant oral cavities.
  - `DYNAMIC`: Reactive acoustic modulation dynamically shifting pitch and playback rate according to real-time magnetic anomaly intensity ($|\Delta\mu\text{T}|$).
  - `SPIRIT BOX`: Swept formant modulation combined with burst radio static.
- **3D Adaptive Baseline & Environmental Drift Tracking**: Continuously monitors ambient geomagnetic flux across all 3 spatial axes ($B_x, B_y, B_z$) using an exponential moving average (EWMA, $\alpha = 0.015, \tau \approx 7\,\text{s}$). Naturally absorbs slow room drift, building settling, and temperature fluctuations without requiring constant manual calibration.
- **Event-Driven Transient Anomaly Trigger**: Completely silent in calm ambient environments (no random or periodic timer-based word spitting). Anomalies require a sustained transient flux deviation ($\ge 200\,\text{ms}$) exceeding the threshold. Fires exactly once at the peak of the anomaly, then disarms until the field returns below reset threshold ($0.5\times\text{Threshold}$) and a $2.5\,\text{s}$ cooldown window elapses, preventing repetitive loops near stationary metal or electrical fixtures.
- **Deterministic 3D Magnetic Vector Dictionary Mapping**: Maps spatial perturbation vectors $(\Delta B_x, \Delta B_y, \Delta B_z)$ deterministically to the dictionary using 64-bit spatial FNV-1a hashing across quantized spherical coordinates (horizontal azimuth $\theta$, vertical elevation $\phi$, and flux magnitude $\Delta||\vec{B}||$). Zero clock-based RNG or sample counters; identical physical field perturbations yield identical dictionary words.
- **Adjustable Sensitivity Presets**: Cycle live in the sensor details overlay:
  - `SENS: HI` ($4.0\,\mu\text{T}$ threshold): Maximum sensitivity for magnetically quiet, remote fieldwork locations.
  - `SENS: MED` ($8.0\,\mu\text{T}$ threshold, default): Balanced signal-to-noise ratio for standard paranormal investigations.
  - `SENS: LO` ($15.0\,\mu\text{T}$ threshold): Robust filtering for locations with high background electromagnetic interference (EMI).
- **Acoustic EMF Geiger Ticks**: Scaled live click telemetry through `AAudio` when magnetic field deviations occur above baseline, accelerating dynamically with anomaly intensity ($\Delta\text{EMF}$) for hands-free audible tracking.
- **DSP Schroeder Reverb & Multi-Tap Echo**: Atmospheric spatial resonance, 3-tap parallel comb delay lines, and all-pass diffusion for decaying ghost echoes on synthesized words.
- **6-DOF IMU Motion Rejection Filter**: Low-pass filtered angular velocity ($\text{rad/s}$) and linear acceleration deviation from $1g$ ($9.80665\,\text{m/s}^2$). Rejects false magnetic anomalies caused by user hand movement, instantly flushes partial transient accumulation, and enforces a 1.2-second settling hold before re-enabling baseline triggers.
- **Multi-Mode OpenGL ES 2 Camera Shaders & Anomaly Distortion FX**: Universal GPU-accelerated video filtering across six selectable modes, plus toggleable `FX GLITCH` chromatic aberration and scanline modulation during magnetic, proximity, and sub-bass anomalies:
  - `NIGHT`: Phosphor green night-vision amplification.
  - `THERMAL`: Ironbow false-color heatmap LUT.
  - `EDGE`: 3×3 Sobel spectral contour detection.
  - `MONO`: High-contrast infrared grayscale.
  - `SEPIA`: Vintage tone map.
  - `RAW`: Unfiltered native sensor stream.
- **Camera Hardware Controls, Zoom & Self-Timer**:
  - `TORCH` / `TORCH ON`: Live Camera2 flashlight toggle on lenses reporting `ACAMERA_FLASH_INFO_AVAILABLE` without tearing down the session.
  - `REAR` / `FRONT`: Idle lens switching with facing-aware orientation and horizontal selfie mirror across both live preview and encoded video.
  - `1.0X` / `1.5X` / `2.0X` / `3.0X`: Digital zoom shared identically across live GL preview, MP4 encoding, and BMP snapshots.
  - `SNAP`: Captures an oriented, filtered 24-bit BMP snapshot (`SNAP_*.bmp`) atomically into `files/evidence`.
  - `TMR OFF` / `5S` / `10S` & `CHIME ON` / `QUIET`: Capture countdown self-timer with pure-synthesized `AAudio` tick, start/stop cues, and debounced EMF anomaly chimes.
  - Aspect presets (`16:9`, `9:16`, `4:5`) and toggleable `HUD` / `CLEAN` burn-in overlay for MP4 recording.
- **Viewfinder Gestures & Minimal HUD**:
  - Horizontal swipe across camera viewfinder or tap the bottom-left filter label to cycle through all 6 video shader filters.
  - Single tap on camera viewport toggles Minimal HUD mode to maximize viewfinder area.
  - Live camera HUD includes a 3-word rolling history feed, graduated EMF meter with analog needle and peak-hold indicator, and a flashing `EMF ALERT` banner during elevated spikes.
- **HUD Theme Modes, GLSL Glow Wave Shader & 7-Color Gradient Tint**:
  - `WAVE SHADER`: OpenGL ES 2 multi-curve glowing harmonic waveform shader driven in real time by live spectrum bands and RMS amplitude (toggleable in `GUIDE`).
  - `#ORIGINAL`: Classic 12-segment stacked hardware LED equalizer blocks with authentic tri-zone hardware color grading (Rows 0..7 Mint Green, Rows 8..9 Amber Warning, Rows 10..11 Red Peak) and live rolling phosphor-green oscilloscope trace.
  - `ABDUCTION`: Neon bio-luminescent alien curved waveform visualizer.
  - `AI`: 16-node digital matrix waveform visualizer.
  - `TINT`: Piecewise linear 7-color gradient hue interpolation (`ds_interpolate_hud_accent`) across White, Red, Yellow, Green, Cyan, Blue, and Magenta stops for universal HUD tinting.
- **Sub-Bass EVP Evaluator (80–300 Hz) & Hum-Reduction Filter (`HPF`)**:
  - 6-bin Goertzel FFT filter bank monitoring sub-bass acoustic power (80, 120, 160, 200, 250, 300 Hz) with Hann windowing and speech-band rejection against running acoustic baselines.
  - Toggleable 2nd-order Butterworth 80 Hz high-pass + 60 Hz notch IIR filter (`HPF ON/OFF`) for reducing handling rumble and mains hum during live capture and Log WAV playback.
  - Latches a 2000 ms `[SUB-BASS EVP BURST 80-300Hz]` alert banner and automatically emits Tag 2 into the session companion tags file.
- **Hardware Proximity Radar**:
  - Concentric radar ripple pulse animations during proximity triggers (`< 4.0 cm`).
  - Strict hardware sensor validation rejecting virtual, software, and vendor-emulated gesture devices.
- **Dual-Tier DictionaryView Word Announcer & 10-Second Hold**:
  - High-visibility primary detected word card with secondary real-time telemetry subtitle ($\mu\text{T}$, baseline delta, coordinates, and whisper indicator).
  - 10-second hold display window with graceful automatic reversion to "LISTENING..." and 3-second pulsing cyan anomaly border during active spikes.
- **Companion File Triad & Interactive Scrubbable Tag Timeline**:
  - During recording, automatically generates 3 synchronized companion files alongside media:
    - `<recording>.txt`: Chronological list of words triggered during the session.
    - `<recording>_tags.txt`: Anomaly event CSV (`<timestamp_ms>,<tag_id>`) with Tag 1 (Word trigger, Green), Tag 2 (Anomaly alert: EMF spike, Sub-bass EVP burst, Proximity trigger, Orange), and Tag 3 (Manual bookmark via `[+TAG]` HUD button, Purple).
    - `<recording>_waveform.txt`: RMS audio amplitude telemetry sampled every 250 ms.
    - `<recording>.srt`: 1-second timestamped subtitle telemetry cue points tracking $\mu\text{T}$, baseline delta, IMU stability, and triggered words.
  - In-app Log gallery displays companion status badges (`[TXT] [TAGS] [WAVE] [SRT]`), inline BMP snapshot viewer (`ZOOM: 1X/2X`), inline hardware-decoded MP4 video + AAC audio playback, and atomic delete (`[DEL]`) unlinking primary captures and all 4 companion files synchronously.
  - Interactive scrubbable player with real-time waveform progress bar, tag marker dots along the timeline, `[<< TAG]` / `[TAG >>]` instantaneous tag seeking, `[SPD]` variable speed (`0.5X` / `0.75X` / `1.0X` / `1.5X`), `[LOOP A / LOOP B / CLR A-B]` segment looping, and `[+BOOKMARK]` live Tag 3 insertion.
- **Dynamic Voice Modulation**:
  - Dynamically modulates voice pitch multiplier $[0.50, 1.30]$ and speed $[0.80, 1.10]$ per triggered word, combined with microtonal vibrato and Schroeder comb-filter atmospheric reverb.
- **HUD Live Controls & Bookmarking**:
  - `+TAG` one-tap manual bookmarking button during active recording to tag anomalous investigation events.
  - Transient toast status banners for export and bookmark confirmation.
- **External Session Log Export**:
  - One-tap `EXPORT LOGS` in the Log tab creating pipe-delimited session exports (`DeadSpeakWordHistory.txt`, `log.txt` circular 100-entry buffer, and `DeadSpeakSessionExport.txt`) in external `Movies/` storage.
- **Custom Word Bank (`.dswb`)**: Local binary word bank manager supporting on-device phrase authoring via an on-screen keyboard, numbered list pagination, and atomic persistence.
- **Bundled Word Bank**: Binary `words.bin` asset containing over 5,000 unique entries across English and 22 transliterated language groups.
- Real magnetometer, accelerometer, gyroscope, hardware proximity, ambient light (`LUX`), and barometric pressure (`HPA`) readings. Sensor registration errors and absent, blocked, waiting, paused, invalid, and stale readings are reported distinctly; missing data never produces a substitute reading. Sensors stop on pause and re-enable on resume.
- Native AAudio EVP recording to mono PCM16 WAV alongside Camera2 + OpenGL ES 2 + `AMediaCodec`/`AMediaMuxer` muxed H.264/AAC MP4 video recording (`VIDEO_*.mp4`) and filtered 24-bit BMP snapshots (`SNAP_*.bmp`).
- Atomic persistent configuration (`files/evidence/settings.cfg`) preserving voice mode/pitch/speed, filter, aspect ratio, HUD burn-in, sensitivity, theme/tint, lens selection, zoom, anomaly FX, self-timer, EMF chimes, HPF, VOX gate, and wave shader settings across launches.
- A native Explore directory sourced from Wikipedia's community-maintained reported-haunted-location categories, enriched with linked Wikidata item IDs and coordinates.
- Persistent sensor-word log and saved evidence directory in private storage (`files/evidence`).

Audio (`EVP_*.wav`), muxed filtered A/V video (`VIDEO_*.mp4`), and filtered snapshots (`SNAP_*.bmp`) stay strictly local in `files/evidence` (with optional user-initiated companion text export to app-external `Movies/`). Explore operates entirely locally once cached for 30 days. No paranormal app or sensor reading has been scientifically established as a means of communicating with spirits.

## Requirements and language boundary

DeadSpeak and its build contain no Java/Kotlin code, bytecode, DEX, JARs, or JVM-based tools. Application logic is C23 and Zig; binary assets contain data, not disguised bytecode. Android's operating-system framework remains a platform dependency: C uses JNI for system permissions, window APIs, and platform network/JSON services. No app-defined Java classes are compiled or packaged. The manifest explicitly sets `android:hasCode="false"`.

- Linux build host with Zig, CMake 3.25+, Ninja, pkg-config, libarchive, libxml2, and OpenSSL 3 development headers.
- Installed Android NDK 30.0.16248370, SDK platform 37.2 (including its raw `data/res` directory), and native build tools 37.0.0. Versions are pinned in `tools/build_config.h`.
- The latest installed Zig and its matching standard library. All project C tools and Android C sources are compiled through `zig cc`; the signal engine stays Zig. Native NDK tools link against Android system libraries. There is no alternate compiler or signal-engine fallback.
- Android 11 or newer, with camera/microphone for recording; magnetometer optional.
- An internet connection only for initially downloading or refreshing public Wikipedia/Wikidata Explore data. There are no user accounts or evidence uploads.

Configure these machine-local paths in ignored `local.properties`:

```properties
sdk.dir=/absolute/path/to/Android/Sdk
deadspeak.zigLibDir=/absolute/path/to/matching/zig/lib
```

`DS_SDK_DIR` and `ZIG_LIB_DIR` can override those paths. A `compiler-version.txt` beside the configured Zig `lib/` directory is checked against `zig version`. Libraries must match the compiler's exact revision.

## Build

The small shell launcher bootstraps the C build driver; build orchestration, APK assembly, signing, and verification are native C. CMake/Ninja handle Android native compilation. A C XML scanner extracts the framework resource IDs referenced by the app from the SDK's published XML definitions, retaining attribute types and enum/flag values. AAPT2 compiles that symbol-only package: no `android.jar`, Java resource classes, Gradle, AGP, D8, or `apksigner` are used. The framework symbol package is regenerated each build and is never shipped in the app; framework style and colour references resolve to Android's actual resources, not substituted app values. New or unknown resource references fail the build rather than guessing IDs.

Choose a signing identity explicitly before building. To retain an existing Android debug identity stored in **PKCS#12**, import it using native OpenSSL (the standard debug password is shown here):

```sh
DS_PKCS12_PASSWORD=android sh tools/build.sh --import-pkcs12 /path/to/debug.keystore
```

Alternatively, create a new local RSA signing identity:

```sh
sh tools/build.sh --init-key
```

These commands never overwrite an existing identity. The private key and certificate are saved with restricted permissions in ignored `.signing/`; back up that directory privately and never commit it. Java keystore formats are deliberately unsupported. Changing the signing identity prevents updating a previously installed app signed by another key; do not uninstall an app with evidence merely to work around a mismatch.

Build all three supported ABIs, or only the attached arm64 phone's ABI:

```sh
sh tools/build.sh
# Or:
sh tools/build.sh --abi arm64-v8a
```

The output is `build/deadspeak-debug.apk`. The native C signer implements Android's [APK Signature Scheme v2](https://source.android.com/docs/security/features/apksigning/v2) using OpenSSL, with no v1 fallback. The build verifies the signature, checks 16 KiB native-library alignment, and rejects packaged Java/DEX/JAR files or disguised bytecode. All native libraries and host tools are hardened with stack canaries (`-fstack-protector-strong`), position-independent execution (`-fPIE -pie`), Full RELRO (`-Wl,-z,relro -Wl,-z,now`), non-executable stacks (`NX`), and strict bounded string copies. Native C compilation rejects deprecated declarations.

Run native signing regressions (no phone or JVM needed):

```sh
sh tests/signing_test.sh
```

For the full three-ABI build plus signing-identity permissions, path and ELF-architecture guards, sensor-state validation, camera UI layout geometry, custom words binary storage, 3D adaptive baseline and event-driven word triggers, deterministic vector dictionary mapping, formant voice synthesis, IMU motion filtering, C23 typography, atomic settings persistence, EVP cue/hum-filter/BMP helpers, and signing regressions:

```sh
sh tools/test-build.sh
```

Sensor tests cover denied access, pause/re-enable, stale and invalid samples, monotonic timestamps, and independent magnetic/motion vectors. Test fixtures never supply readings to the app.

The app uses `WindowInsetsController` through native JNI instead of deprecated fullscreen flags. Backup and device-transfer rules keep evidence local; the Android 11 backup opt-out flags remain necessary for the supported platform.

With an arm64 Android device connected, run the native C regression tests:

```sh
sh tools/test-native.sh
```

These exercise on-device native integration across three test suites:
- `explore_test`: JNI thread ownership and local-reference cleanup, complete index replacement (including updated and removed pages), concurrent readers, cache persistence, responsive geometry across 900 window sizes, pagination at every supported page size, and all sixteen sensor/display orientation combinations.
- `voice_player_test`: Native `AAudio` stream lifecycle, dynamic Geiger click rate scaling, synthesized speech and hardware cue queueing, recorded 48 kHz WAV playback downsampling, variable speed, A-B looping, hum filtering, and immediate pause/stop cancellation.
- `media_player_test`: Native `AMediaExtractor`/`AMediaCodec` MP4 playback lifecycle, seek/progress clamping, variable playback speed, A-B loop bounds, pause/stop guards, and clean teardown.

## Security audit and code quality

The DeadSpeak codebase is continuously verified using local security and analysis tools:

- **Binary Hardening (`checksec`)**: Verified Full RELRO, Stack Canary protection (`-fstack-protector-strong`), non-executable stack (`NX`), Position-Independent Executables (`PIE`), and W^X memory segment protection across `libdeadspeak.so` and all host binaries.
- **Static Analysis**: Verified clean with `cppcheck --enable=all --inconclusive`, `flawfinder`, `semgrep`, and `clang-tidy` under standard C23.
- **Dynamic Memory Analysis & Sanitizers**: Zero memory leaks and zero invalid reads/writes validated under `valgrind` memcheck; host test suites verified clean with AddressSanitizer (ASan) and UndefinedBehaviorSanitizer (UBSan) (`-fsanitize=address,undefined`).
- **Shell Script Validation**: All build orchestrators and test harnesses pass `shellcheck` with zero warnings.

## Install

```sh
adb install -r build/deadspeak-debug.apk
adb shell am start -n org.deadspeak/android.app.NativeActivity
```

Recordings and the word log are stored in the app's private `files/evidence` directory. Grant camera and microphone permissions when prompted. The app does not upload recordings.

On GrapheneOS, also allow DeadSpeak's Sensors permission to use the magnetometer. [GrapheneOS's sensor-permission control](https://grapheneos.org/features#sensors-permission-toggle) can block readings independently of whether sensor hardware is present. The manifest declares that permission explicitly; recording permissions remain separate.

## Repositories

- Public GitLab backup: https://gitlab.com/exalted6165/DeadSpeak
- Source repository (Codeberg): https://codeberg.org/Exalted6165/DeadSpeak

## License

DeadSpeak is free software licensed under the **GNU General Public License Version 3 or later (GPL-3.0-or-later)** combined with the **GNU Affero General Public License Version 3 or later (AGPL-3.0-or-later)** pursuant to Section 13. See the [LICENSE](LICENSE) file for complete terms and conditions.
