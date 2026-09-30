# Project-local AGP/KGP `Configuration.setVisible` patch

This included build applies the upstream Android Gradle Plugin 9.4.1 through a small
namespaced delegating plugin. Before Gradle resolves `com.android.application`, the
settings plugin creates a deterministic local Maven artifact and redirects that plugin
request to it. Its rewritten POM points to a project-local patched KGP artifact.

For AGP 9.4.1, the patch checks the upstream manifest version, JAR/POM SHA-256,
all 28 expected callsites and per-class counts, the `ICONST_0` operand, and that no
matching invocations remain. Calls are replaced with `POP`, retaining the Boolean
operand and receiver stack shape, including intervening stack-map frames.

The remaining invocations are in the exact KGP 2.2.10 `gradle813` classifier selected
by this AGP/Gradle combination. A whole-JAR ASM scan found 32 calls in 19 class
entries (20 class/method targets), including `DefaultKotlinBasePlugin.apply(Project)`
and Kapt, multiplatform, scripting, and native helper code. Every call has the exact
`ICONST_0 / INVOKEINTERFACE Configuration.setVisible(Z) / POP` shape. The patch pins
the JAR and POM by SHA-256 and manifest identity, validates the complete class/method
count map and instruction shape, then replaces every invocation with `POP`. Thus the
false argument and returned receiver are discarded in sequence and the deprecated
property mutation is removed. The upstream KGP POM dependency graph is retained; the
AGP POM redirects only this dependency to the private coordinate and specifies the
same `gradle813` classifier explicitly.

Both patched modules are served from one exclusive, project-local Maven repository
configured to use POM metadata only. This avoids publishing or depending on generated
`.module` metadata: the pinned classifier is explicit in AGP's patched POM, while
KGP's ordinary POM retains its transitive dependencies. The patchers fail closed on
unknown versions, checksums, targets, instruction shapes, or remaining calls.

The app is C/Zig/native-only. Its supported `android { enableKotlin = false }` setting
avoids Kotlin compilation and the automatic Kotlin standard-library dependency; it
does not disable Kotlin DSL for Gradle build scripts. See Android's [module-level
Kotlin opt-out documentation](https://developer.android.com/build/migrate-to-built-in-kotlin#option_to_selectively_disable_built_in_kotlin).
Gradle documents `Configuration.isVisible` as legacy and states its value does not
affect dependency resolution or inter-project visibility; see the [Gradle 9.8 API
documentation](https://docs.gradle.org/9.8.0/javadoc/org/gradle/api/artifacts/Configuration.html).

The original AGP JAR is read from Gradle's normal dependency cache and is never
modified. Gradle may continue to download/cache normal upstream dependencies and
derived transformed artifacts. The staged inputs, generated Maven repository, and
verification output are written beneath the project's ignored top-level `build/`
directory; no binary JAR is checked into Git.

The generated coordinates are cache identities. Whenever patcher implementation,
transformation behavior, or generated output changes, increment the matching
`deadspeak` revision (`9.4.1-deadspeak.N` for AGP and
`2.2.10-deadspeak.N` for KGP). Review upstream checksums, KGP's selected Gradle
classifier, and bytecode targets alongside each upgrade.

Run the independent patcher checks with:

```sh
./gradlew -p build-logic test verifyAgpPatch
```

The tests cover exact targets/counts, changed operands and invocation shapes,
receiver/stack-map behavior, unchanged entries, late manifest ordering, and
byte-for-byte repeatability.
`verifyAgpPatch` validates checksums and transforms the real cached AGP/KGP artifacts
twice, checking identical output and no remaining calls.

Use `scanKgpVisibleCalls` to print every pinned upstream KGP callsite and neighboring
executable opcodes before reviewing an upgrade or changing the expected call map.

To restore the original plugin wiring, remove `includeBuild("build-logic")` and the
`org.deadspeak.agp-visible-patch` settings plugin from `settings.gradle.kts`, then
change the app plugin declaration back to `com.android.application` (the root plugin
declaration continues to pin AGP 9.4.1). The app's `enableKotlin = false` is an
independent native-module setting and can remain. Generated artifacts are ignored
build outputs; the original Maven-cached JARs are never altered.

When AGP or KGP is upgraded, review the official artifact checksums, selected KGP
classifier, POM dependencies, and source/bytecode; update the pinned versions and
expected targets/counts together. Then run the checks above and the application's
`assembleDebug` and `lintDebug` tasks with `--warning-mode fail`. This patch is
version-specific and is not a replacement for upstream fixes.
