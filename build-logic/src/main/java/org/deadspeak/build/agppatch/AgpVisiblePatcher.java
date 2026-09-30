package org.deadspeak.build.agppatch;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.StandardCopyOption;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.Collections;
import java.util.Map;
import java.util.TreeMap;
import java.util.jar.Attributes;
import java.util.jar.JarInputStream;
import java.util.jar.Manifest;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;
import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.tree.AbstractInsnNode;
import org.objectweb.asm.tree.ClassNode;
import org.objectweb.asm.tree.MethodInsnNode;
import org.objectweb.asm.tree.MethodNode;

/** Narrow, fail-closed bytecode patch for the pinned AGP 9.4.1 artifact. */
public final class AgpVisiblePatcher {
    public static final String UPSTREAM_VERSION = "9.4.1";
    public static final String UPSTREAM_JAR_SHA256 =
            "4021fa6ea76b6fac076dd0bdacbb3e1b76c31f33b6c392e1a49a2e42072196bf";
    public static final String UPSTREAM_POM_SHA256 =
            "ee7d29e7d41cb389dc65b02518ffa8c806c522ca565510694eb8ee8dad7f938a";

    private static final String CONFIGURATION = "org/gradle/api/artifacts/Configuration";
    private static final String SET_VISIBLE = "setVisible";
    private static final String SET_VISIBLE_DESCRIPTOR =
            "(Z)Lorg/gradle/api/artifacts/Configuration;";
    private static final String PATCHED_GROUP = "org.deadspeak.build-patches";
    private static final String PATCHED_MODULE = "agp-gradle";
    private static final String PATCHED_VERSION = "9.4.1-deadspeak.4";
    public static final String PATCHED_COORDINATE =
            PATCHED_GROUP + ":" + PATCHED_MODULE + ":" + PATCHED_VERSION;
    private static final String PATCHED_JAR =
            PATCHED_MODULE + "-" + PATCHED_VERSION + ".jar";

    private static final Map<String, Integer> EXPECTED_CALLS;

    static {
        Map<String, Integer> expected = new TreeMap<>();
        expected.put("com/android/build/gradle/internal/TaskManager$Companion$createCoreLibraryDesugaringConfig$1.class", 1);
        expected.put("com/android/build/gradle/internal/coverage/JacocoConfigurations.class", 1);
        expected.put("com/android/build/gradle/internal/dependency/SourceSetManager.class", 1);
        expected.put("com/android/build/gradle/internal/dependency/VariantDependencies$Companion.class", 1);
        expected.put("com/android/build/gradle/internal/dependency/VariantDependenciesBuilder.class", 8);
        expected.put("com/android/build/gradle/internal/lint/AndroidLintInputs.class", 1);
        expected.put("com/android/build/gradle/internal/plugins/BasePlugin$Companion$createAndroidJdkImageConfiguration$1.class", 1);
        expected.put("com/android/build/gradle/internal/plugins/BasePlugin$Companion.class", 3);
        expected.put("com/android/build/gradle/internal/plugins/FusedLibraryPlugin$maybePublishToMaven$runtimePublication$1.class", 1);
        expected.put("com/android/build/gradle/internal/plugins/FusedLibraryPlugin$maybePublishToMaven$runtimeSourcePublication$1.class", 1);
        expected.put("com/android/build/gradle/internal/tasks/creationconfig/BaseJavaPreCompileTaskCreationConfig.class", 1);
        expected.put("com/android/build/gradle/internal/tasks/factory/KmpGlobalTaskCreationConfigImpl.class", 2);
        expected.put("com/android/build/gradle/internal/testing/utp/UtpTestUtilsKt$maybeCreateUtpConfigurations$1$1.class", 1);
        expected.put("com/android/build/gradle/internal/testsuites/impl/TestSuiteDependenciesBuilder.class", 3);
        expected.put("com/android/build/gradle/tasks/GetPrefabFromMavenKt$getPrefabFromMaven$config$1.class", 1);
        expected.put("com/android/build/gradle/tasks/KaptCreationAction.class", 1);
        EXPECTED_CALLS = Collections.unmodifiableMap(expected);
    }

    private AgpVisiblePatcher() {}

    public static void main(String[] args) throws IOException {
        if (args.length != 5) {
            throw new IllegalArgumentException(
                    "Expected AGP JAR/POM, KGP JAR/POM, and output repository paths.");
        }
        prepareMavenRepository(
                Path.of(args[0]), Path.of(args[1]), Path.of(args[2]), Path.of(args[3]), Path.of(args[4]));
    }

    /** Validates, patches, and writes only beneath the supplied project-local repository. */
    public static void prepareMavenRepository(
            Path sourceJar, Path sourcePom, Path kgpJar, Path kgpPom, Path repository)
            throws IOException {
        byte[] originalJar = Files.readAllBytes(sourceJar);
        byte[] originalPom = Files.readAllBytes(sourcePom);
        byte[] originalKgpJar = Files.readAllBytes(kgpJar);
        byte[] originalKgpPom = Files.readAllBytes(kgpPom);
        requireHash(originalJar, UPSTREAM_JAR_SHA256, "AGP 9.4.1 JAR");
        requireHash(originalPom, UPSTREAM_POM_SHA256, "AGP 9.4.1 POM");
        verifyPluginVersion(originalJar);
        KgpVisiblePatcher.verifyPinnedArtifact(originalKgpJar, originalKgpPom);

        byte[] patchedOnce = transformJar(originalJar);
        byte[] patchedTwice = transformJar(originalJar);
        if (!Arrays.equals(patchedOnce, patchedTwice)) {
            throw new IllegalStateException("AGP transformation is not byte-for-byte reproducible.");
        }
        if (countSetVisibleCalls(patchedOnce) != 0) {
            throw new IllegalStateException("Patched AGP still contains Configuration.setVisible invocations.");
        }
        byte[] patchedKgpOnce = KgpVisiblePatcher.transformJar(originalKgpJar);
        byte[] patchedKgpTwice = KgpVisiblePatcher.transformJar(originalKgpJar);
        if (!Arrays.equals(patchedKgpOnce, patchedKgpTwice)) {
            throw new IllegalStateException("KGP transformation is not byte-for-byte reproducible.");
        }
        if (KgpVisiblePatcher.countSetVisibleCalls(patchedKgpOnce) != 0) {
            throw new IllegalStateException("Patched KGP still contains Configuration.setVisible invocations.");
        }

        Path versionDirectory = repository.resolve(
                PATCHED_GROUP.replace('.', '/') + "/" + PATCHED_MODULE + "/" + PATCHED_VERSION);
        Path kgpVersionDirectory = repository.resolve(
                KgpVisiblePatcher.PATCHED_GROUP.replace('.', '/') + "/"
                        + KgpVisiblePatcher.PATCHED_MODULE + "/" + KgpVisiblePatcher.PATCHED_VERSION);
        Files.createDirectories(versionDirectory);
        Files.createDirectories(kgpVersionDirectory);
        writeIfChangedAtomically(versionDirectory.resolve(PATCHED_JAR), patchedOnce);
        writeIfChangedAtomically(
                kgpVersionDirectory.resolve(KgpVisiblePatcher.PATCHED_MODULE + "-"
                        + KgpVisiblePatcher.PATCHED_VERSION + "-"
                        + KgpVisiblePatcher.UPSTREAM_VARIANT + ".jar"),
                patchedKgpOnce);
        // Publish both POMs after both JARs, with AGP last as the repository entrypoint.
        writeIfChangedAtomically(
                kgpVersionDirectory.resolve(KgpVisiblePatcher.PATCHED_MODULE + "-"
                        + KgpVisiblePatcher.PATCHED_VERSION + ".pom"),
                recoordinatePom(originalKgpPom, "org.jetbrains.kotlin", "kotlin-gradle-plugin",
                        KgpVisiblePatcher.UPSTREAM_VERSION, KgpVisiblePatcher.PATCHED_GROUP,
                        KgpVisiblePatcher.PATCHED_MODULE, KgpVisiblePatcher.PATCHED_VERSION));
        writeIfChangedAtomically(
                versionDirectory.resolve(PATCHED_MODULE + "-" + PATCHED_VERSION + ".pom"),
                patchedAgpPom(originalPom));
    }

    /** Patches an AGP jar after validating the exact known call sites and instruction shapes. */
    public static byte[] transformJar(byte[] originalJar) throws IOException {
        TreeMap<String, byte[]> entries = readEntries(originalJar);
        Map<String, Integer> observed = new TreeMap<>();
        for (Map.Entry<String, byte[]> entry : entries.entrySet()) {
            if (!entry.getKey().endsWith(".class")) {
                continue;
            }
            byte[] patchedClass = patchClass(entry.getKey(), entry.getValue(), observed);
            if (patchedClass != entry.getValue()) {
                entry.setValue(patchedClass);
            }
        }
        if (!EXPECTED_CALLS.equals(observed)) {
            throw new IllegalArgumentException(
                    "Unknown AGP Configuration.setVisible bytecode. Expected "
                            + EXPECTED_CALLS + " but found " + observed + ". Refusing to patch.");
        }

        byte[] output = writeEntries(entries);
        int remaining = countSetVisibleCalls(output);
        if (remaining != 0) {
            throw new IllegalStateException("Patch verification found " + remaining + " remaining invocations.");
        }
        return output;
    }

    /** Counts exact Configuration.setVisible calls in all classes in a jar. */
    public static int countSetVisibleCalls(byte[] jarBytes) throws IOException {
        TreeMap<String, byte[]> entries = readEntries(jarBytes);
        int total = 0;
        for (Map.Entry<String, byte[]> entry : entries.entrySet()) {
            if (!entry.getKey().endsWith(".class")) {
                continue;
            }
            ClassNode node = readClass(entry.getValue());
            for (MethodNode method : node.methods) {
                for (AbstractInsnNode instruction : method.instructions) {
                    if (instruction instanceof MethodInsnNode call
                            && CONFIGURATION.equals(call.owner)
                            && SET_VISIBLE.equals(call.name)) {
                        total++;
                    }
                }
            }
        }
        return total;
    }

    static Map<String, Integer> expectedCalls() {
        return EXPECTED_CALLS;
    }

    private static byte[] patchClass(
            String entryName, byte[] classBytes, Map<String, Integer> observed) {
        ClassNode node = readClass(classBytes);
        int classCalls = 0;
        for (MethodNode method : node.methods) {
            for (AbstractInsnNode instruction = method.instructions.getFirst();
                    instruction != null; ) {
                AbstractInsnNode next = instruction.getNext();
                if (instruction instanceof MethodInsnNode call
                        && CONFIGURATION.equals(call.owner)
                        && SET_VISIBLE.equals(call.name)) {
                    if (!SET_VISIBLE_DESCRIPTOR.equals(call.desc)
                            || call.getOpcode() != Opcodes.INVOKEINTERFACE
                            || !call.itf) {
                        throw new IllegalArgumentException(
                                "Unexpected setVisible invocation in " + entryName + ": " + call);
                    }
                    AbstractInsnNode argument = previousExecutableInstruction(call);
                    if (argument == null || argument.getOpcode() != Opcodes.ICONST_0) {
                        throw new IllegalArgumentException(
                                "Expected the pinned AGP call to push false immediately before "
                                        + "Configuration.setVisible in " + entryName + ".");
                    }
                    // Keep the false operand and discard it with POP. This preserves stack-map
                    // frames between the operand and call, while leaving the receiver on stack.
                    method.instructions.set(call, new org.objectweb.asm.tree.InsnNode(Opcodes.POP));
                    classCalls++;
                }
                instruction = next;
            }
        }
        if (classCalls == 0) {
            return classBytes;
        }
        Integer expected = EXPECTED_CALLS.get(entryName);
        if (expected == null || expected != classCalls) {
            throw new IllegalArgumentException(
                    "Unexpected Configuration.setVisible target/count: "
                            + entryName + " has " + classCalls + " calls; expected " + expected + ".");
        }
        observed.put(entryName, classCalls);
        ClassWriter writer = new ClassWriter(0);
        node.accept(writer);
        return writer.toByteArray();
    }

    private static AbstractInsnNode previousExecutableInstruction(AbstractInsnNode instruction) {
        AbstractInsnNode previous = instruction.getPrevious();
        while (previous != null && previous.getOpcode() < 0) {
            previous = previous.getPrevious();
        }
        return previous;
    }

    private static ClassNode readClass(byte[] bytes) {
        ClassNode node = new ClassNode(Opcodes.ASM9);
        new ClassReader(bytes).accept(node, 0);
        return node;
    }

    private static TreeMap<String, byte[]> readEntries(byte[] jarBytes) throws IOException {
        TreeMap<String, byte[]> entries = new TreeMap<>();
        try (ZipInputStream input = new ZipInputStream(new ByteArrayInputStream(jarBytes))) {
            ZipEntry entry;
            while ((entry = input.getNextEntry()) != null) {
                if (entry.isDirectory()) {
                    continue;
                }
                String upperName = entry.getName().toUpperCase(java.util.Locale.ROOT);
                if (upperName.startsWith("META-INF/")
                        && (upperName.endsWith(".SF") || upperName.endsWith(".RSA")
                                || upperName.endsWith(".DSA") || upperName.endsWith(".EC"))) {
                    throw new IllegalArgumentException("Refusing to rewrite signed JAR entry " + entry.getName());
                }
                ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                input.transferTo(bytes);
                if (entries.put(entry.getName(), bytes.toByteArray()) != null) {
                    throw new IllegalArgumentException("Duplicate JAR entry: " + entry.getName());
                }
            }
        }
        if (entries.isEmpty()) {
            throw new IllegalArgumentException("Input is not a readable JAR.");
        }
        return entries;
    }

    private static byte[] writeEntries(TreeMap<String, byte[]> entries) throws IOException {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream output = new ZipOutputStream(bytes)) {
            output.setLevel(9);
            for (Map.Entry<String, byte[]> entry : entries.entrySet()) {
                ZipEntry zipEntry = new ZipEntry(entry.getKey());
                zipEntry.setTime(0L);
                output.putNextEntry(zipEntry);
                output.write(entry.getValue());
                output.closeEntry();
            }
        }
        return bytes.toByteArray();
    }

    private static void verifyPluginVersion(byte[] jarBytes) throws IOException {
        try (JarInputStream input = new JarInputStream(new ByteArrayInputStream(jarBytes))) {
            Manifest manifest = input.getManifest();
            if (manifest == null) {
                throw new IllegalArgumentException("AGP JAR has no manifest.");
            }
            Attributes attributes = manifest.getMainAttributes();
            String pluginVersion = attributes.getValue("Plugin-Version");
            if (!UPSTREAM_VERSION.equals(pluginVersion)) {
                throw new IllegalArgumentException(
                        "Expected AGP Plugin-Version " + UPSTREAM_VERSION + ", found " + pluginVersion + ".");
            }
        }
    }

    private static byte[] patchedAgpPom(byte[] pomBytes) {
        String pom = new String(pomBytes, StandardCharsets.UTF_8);
        String patched = recoordinatePom(pom, "com.android.tools.build", "gradle", UPSTREAM_VERSION,
                PATCHED_GROUP, PATCHED_MODULE, PATCHED_VERSION);
        patched = rewriteKgpDependency(patched);
        if (patched.equals(pom) || !patched.contains("<groupId>" + PATCHED_GROUP + "</groupId>")) {
            throw new IllegalArgumentException("Could not rewrite the pinned AGP POM coordinates.");
        }
        return patched.getBytes(StandardCharsets.UTF_8);
    }

    private static byte[] recoordinatePom(
            byte[] pom, String oldGroup, String oldModule, String oldVersion,
            String newGroup, String newModule, String newVersion) {
        return recoordinatePom(new String(pom, StandardCharsets.UTF_8),
                oldGroup, oldModule, oldVersion, newGroup, newModule, newVersion)
                .getBytes(StandardCharsets.UTF_8);
    }

    private static String recoordinatePom(
            String pom, String oldGroup, String oldModule, String oldVersion,
            String newGroup, String newModule, String newVersion) {
        String patched = replaceFirst(pom,
                "<groupId>" + oldGroup + "</groupId>", "<groupId>" + newGroup + "</groupId>");
        patched = replaceFirst(patched,
                "<artifactId>" + oldModule + "</artifactId>", "<artifactId>" + newModule + "</artifactId>");
        patched = replaceFirst(patched,
                "<version>" + oldVersion + "</version>", "<version>" + newVersion + "</version>");
        if (patched.equals(pom)) {
            throw new IllegalArgumentException("Could not rewrite POM coordinates for " + oldGroup + ":" + oldModule);
        }
        return patched;
    }

    private static String rewriteKgpDependency(String pom) {
        java.util.regex.Pattern dependencyPattern = java.util.regex.Pattern.compile(
                "(?s)<dependency>.*?</dependency>");
        java.util.regex.Matcher matcher = dependencyPattern.matcher(pom);
        StringBuffer output = new StringBuffer();
        int matches = 0;
        while (matcher.find()) {
            String dependency = matcher.group();
            if (dependency.contains("<groupId>org.jetbrains.kotlin</groupId>")
                    && dependency.contains("<artifactId>kotlin-gradle-plugin</artifactId>")
                    && dependency.contains("<version>" + KgpVisiblePatcher.UPSTREAM_VERSION + "</version>")) {
                if (++matches > 1) {
                    throw new IllegalArgumentException("AGP POM contains duplicate KGP dependencies.");
                }
                dependency = replaceFirst(dependency,
                        "<groupId>org.jetbrains.kotlin</groupId>",
                        "<groupId>" + KgpVisiblePatcher.PATCHED_GROUP + "</groupId>");
                dependency = replaceFirst(dependency,
                        "<version>" + KgpVisiblePatcher.UPSTREAM_VERSION + "</version>",
                        "<version>" + KgpVisiblePatcher.PATCHED_VERSION + "</version>");
                dependency = replaceFirst(dependency,
                        "<version>" + KgpVisiblePatcher.PATCHED_VERSION + "</version>",
                        "<version>" + KgpVisiblePatcher.PATCHED_VERSION + "</version>\n"
                                + "      <classifier>" + KgpVisiblePatcher.UPSTREAM_VARIANT + "</classifier>");
            }
            matcher.appendReplacement(output, java.util.regex.Matcher.quoteReplacement(dependency));
        }
        matcher.appendTail(output);
        if (matches != 1) {
            throw new IllegalArgumentException(
                    "Expected exactly one pinned KGP dependency in AGP POM; found " + matches + ".");
        }
        return output.toString();
    }

    private static String replaceFirst(String input, String from, String to) {
        int at = input.indexOf(from);
        if (at < 0) {
            throw new IllegalArgumentException("Expected POM element was not found: " + from);
        }
        return input.substring(0, at) + to + input.substring(at + from.length());
    }

    private static void requireHash(byte[] bytes, String expected, String description) {
        String actual;
        try {
            actual = hex(MessageDigest.getInstance("SHA-256").digest(bytes));
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError("SHA-256 is required by the Java platform.", impossible);
        }
        if (!expected.equals(actual)) {
            throw new IllegalArgumentException(
                    description + " checksum mismatch: expected " + expected + ", found " + actual + ".");
        }
    }

    /** Replaces one generated repository file atomically, without touching identical content. */
    static boolean writeIfChangedAtomically(Path destination, byte[] contents) throws IOException {
        if (Files.exists(destination) && Arrays.equals(Files.readAllBytes(destination), contents)) {
            return false;
        }
        Path parent = destination.toAbsolutePath().getParent();
        Files.createDirectories(parent);
        Path temporary = Files.createTempFile(parent, "." + destination.getFileName() + ".", ".tmp");
        try {
            Files.write(temporary, contents);
            try {
                Files.move(temporary, destination,
                        StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
            } catch (AtomicMoveNotSupportedException exception) {
                throw new IOException("Atomic replacement is not supported for " + destination, exception);
            }
        } finally {
            Files.deleteIfExists(temporary);
        }
        return true;
    }

    private static String hex(byte[] bytes) {
        StringBuilder result = new StringBuilder(bytes.length * 2);
        for (byte value : bytes) {
            result.append(String.format("%02x", value & 0xff));
        }
        return result.toString();
    }
}
