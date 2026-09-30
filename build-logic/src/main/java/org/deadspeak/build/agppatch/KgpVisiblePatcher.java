package org.deadspeak.build.agppatch;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Collections;
import java.util.ArrayList;
import java.util.Map;
import java.util.TreeMap;
import java.util.jar.Attributes;
import java.util.jar.Manifest;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;
import org.objectweb.asm.ClassReader;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.tree.AbstractInsnNode;
import org.objectweb.asm.tree.ClassNode;
import org.objectweb.asm.tree.InsnNode;
import org.objectweb.asm.tree.MethodInsnNode;
import org.objectweb.asm.tree.MethodNode;

/** Fail-closed bytecode patch for the KGP variant selected by the pinned AGP/Gradle pair. */
public final class KgpVisiblePatcher {
    public static final String UPSTREAM_VERSION = "2.2.10";
    public static final String UPSTREAM_VARIANT = "gradle813";
    public static final String UPSTREAM_JAR_SHA256 =
            "bd6b97bd9e563bb57db55a98eaad4306e3f0b39c8a0f9be76e476b22ad51f3b8";
    public static final String UPSTREAM_POM_SHA256 =
            "2efac5be4709583777b2505ea2603ba8e1d7bcf9fa633f2ed7054d6511b1c4a5";
    public static final String PATCHED_GROUP = "org.deadspeak.build-patches";
    public static final String PATCHED_MODULE = "kotlin-gradle-plugin";
    public static final String PATCHED_VERSION = "2.2.10-deadspeak.2";
    public static final String PATCHED_COORDINATE =
            PATCHED_GROUP + ":" + PATCHED_MODULE + ":" + PATCHED_VERSION;
    public static final String CLASS_ENTRY =
            "org/jetbrains/kotlin/gradle/plugin/DefaultKotlinBasePlugin.class";

    private static final String CONFIGURATION = "org/gradle/api/artifacts/Configuration";
    private static final String SET_VISIBLE_DESCRIPTOR =
            "(Z)Lorg/gradle/api/artifacts/Configuration;";
    private static final Map<String, Integer> EXPECTED_CALLS;

    static {
        Map<String, Integer> expected = new TreeMap<>();
        addExpected(expected, "org/jetbrains/kotlin/gradle/internal/Kapt3GradleSubplugin.class",
                "createKaptKotlinTask", "(Lorg/jetbrains/kotlin/gradle/internal/Kapt3GradleSubplugin$Kapt3SubpluginContext;Lorg/gradle/api/tasks/TaskProvider;)Lorg/gradle/api/tasks/TaskProvider;", 1);
        addExpected(expected, CLASS_ENTRY, "apply", "(Lorg/gradle/api/Project;)V", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/plugin/KotlinBasePluginWrapper.class",
                "apply", "(Lorg/gradle/api/Project;)V", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/plugin/abi/UtilsKt.class",
                "prepareAbiClasspath", "(Lorg/gradle/api/Project;)Lorg/gradle/api/artifacts/Configuration;", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/plugin/mpp/KotlinAndroidTarget.class",
                "createSourcesElementsIfNeeded", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Lorg/gradle/api/artifacts/Configuration;", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/plugin/mpp/KotlinTargetSoftwareComponentImplKt$KotlinTargetSoftwareComponent$1$1$configuration$1.class",
                "invoke", "(Lorg/gradle/api/artifacts/Configuration;)V", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/plugin/mpp/apple/swiftexport/internal/SwiftExportInitKt$exportedSwiftExportApiConfiguration$1.class",
                "invoke", "(Lorg/gradle/api/artifacts/Configuration;)V", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/plugin/mpp/compilationImpl/factory/KotlinCompilationDependencyConfigurationsFactoriesKt.class",
                "KotlinCompilationDependencyConfigurationsContainer", "(Lorg/jetbrains/kotlin/gradle/plugin/KotlinTarget;Ljava/lang/String;ZZLorg/jetbrains/kotlin/gradle/plugin/mpp/compilationImpl/factory/ConfigurationNaming;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Lorg/jetbrains/kotlin/gradle/plugin/mpp/compilationImpl/KotlinCompilationConfigurationsContainer;", 10);
        addExpected(expected, "org/jetbrains/kotlin/gradle/plugin/mpp/external/CreateExternalKotlinTargetKt.class",
                "createExternalKotlinTarget$notVisible", "(Lorg/gradle/api/artifacts/Configuration;)Lorg/gradle/api/artifacts/Configuration;", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/plugin/sources/DefaultKotlinSourceSetFactory.class",
                "setUpSourceSetDefaults", "(Lorg/jetbrains/kotlin/gradle/plugin/sources/DefaultKotlinSourceSet;)V", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/scripting/internal/ScriptingGradleSubplugin$Companion$configureForSourceSet$discoveryConfiguration$1.class",
                "invoke", "(Lorg/gradle/api/artifacts/Configuration;)V", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/targets/CreateTargetConfigurationsSideEffectKt$CreateTargetConfigurationsSideEffect$1$4.class",
                "invoke", "(Lorg/jetbrains/kotlin/gradle/plugin/mpp/resources/KotlinTargetResourcesPublicationImpl$TargetResources;)V", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/targets/CreateTargetConfigurationsSideEffectKt$CreateTargetConfigurationsSideEffect$1.class",
                "invoke", "(Lorg/jetbrains/kotlin/gradle/plugin/KotlinTarget;)V", 4);
        addExpected(expected, "org/jetbrains/kotlin/gradle/targets/js/npm/resolver/KotlinCompilationNpmResolver$createAggregatedConfiguration$1.class",
                "invoke", "(Lorg/gradle/api/artifacts/Configuration;)V", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/targets/js/npm/resolver/KotlinCompilationNpmResolver$createPublicPackageJsonConfiguration$1.class",
                "invoke", "(Lorg/gradle/api/artifacts/Configuration;)V", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/targets/native/ConfigureFrameworkExportSideEffectKt$ConfigureFrameworkExportSideEffect$1$1.class",
                "execute", "(Lorg/jetbrains/kotlin/gradle/plugin/mpp/AbstractNativeLibrary;)V", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/targets/native/internal/CInteropCommonizerConfigurationsKt.class",
                "locateOrCreateCommonizedCInteropDependencyConfiguration", "(Lorg/gradle/api/Project;Lorg/jetbrains/kotlin/gradle/plugin/KotlinSourceSet;Lkotlin/coroutines/Continuation;)Ljava/lang/Object;", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/targets/native/internal/CInteropConfigurationsKt.class",
                "locateOrCreateCInteropDependencyConfiguration", "(Lorg/gradle/api/Project;Lorg/jetbrains/kotlin/gradle/plugin/mpp/KotlinNativeCompilation;)Lorg/gradle/api/artifacts/Configuration;", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/targets/native/tasks/artifact/KotlinArtifactKt.class",
                "registerExportDependencies", "(Lorg/gradle/api/Project;Lorg/jetbrains/kotlin/konan/target/KonanTarget;Ljava/lang/String;Ljava/util/Set;)Ljava/lang/String;", 1);
        addExpected(expected, "org/jetbrains/kotlin/gradle/targets/native/tasks/artifact/KotlinArtifactKt.class",
                "registerLibsDependencies", "(Lorg/gradle/api/Project;Lorg/jetbrains/kotlin/konan/target/KonanTarget;Ljava/lang/String;Ljava/util/Set;)Ljava/lang/String;", 1);
        EXPECTED_CALLS = Collections.unmodifiableMap(expected);
    }

    private static void addExpected(
            Map<String, Integer> expected, String classEntry, String methodName,
            String methodDescriptor, int count) {
        expected.put(classEntry + "#" + methodName + methodDescriptor, count);
    }

    private KgpVisiblePatcher() {}

    public static void main(String[] args) throws IOException {
        if (args.length != 2 || !"--scan".equals(args[0])) {
            throw new IllegalArgumentException("Usage: KgpVisiblePatcher --scan <KGP JAR>");
        }
        TreeMap<String, byte[]> entries = readEntries(Files.readAllBytes(Path.of(args[1])));
        ArrayList<String> callSites = new ArrayList<>();
        for (Map.Entry<String, byte[]> entry : entries.entrySet()) {
            if (!entry.getKey().endsWith(".class")) continue;
            ClassNode node = readClass(entry.getValue());
            for (MethodNode method : node.methods) {
                for (AbstractInsnNode instruction : method.instructions) {
                    if (instruction instanceof MethodInsnNode call
                            && CONFIGURATION.equals(call.owner)
                            && "setVisible".equals(call.name)) {
                        AbstractInsnNode previous = previousExecutableInstruction(call);
                        AbstractInsnNode next = nextExecutableInstruction(call);
                        callSites.add(entry.getKey() + "#" + method.name + method.desc
                                + " opcode=" + call.getOpcode() + " itf=" + call.itf
                                + " desc=" + call.desc
                                + " prev=" + (previous == null ? "none" : previous.getOpcode())
                                + " next=" + (next == null ? "none" : next.getOpcode()));
                    }
                }
            }
        }
        Collections.sort(callSites);
        callSites.forEach(System.out::println);
        System.out.println("TOTAL=" + callSites.size());
    }

    public static void verifyPinnedArtifact(byte[] jarBytes, byte[] pomBytes) throws IOException {
        requireHash(jarBytes, UPSTREAM_JAR_SHA256, "KGP 2.2.10 " + UPSTREAM_VARIANT + " JAR");
        requireHash(pomBytes, UPSTREAM_POM_SHA256, "KGP 2.2.10 POM");
        verifyManifest(jarBytes);
    }

    /** Replace the obsolete call with POP while retaining its false operand and trailing POP. */
    public static byte[] transformJar(byte[] originalJar) throws IOException {
        TreeMap<String, byte[]> entries = readEntries(originalJar);
        Map<String, Integer> discovered = discoverCallSites(entries);
        if (!EXPECTED_CALLS.equals(discovered)) {
            throw new IllegalArgumentException(
                    "Unknown KGP Configuration.setVisible call map. Expected "
                            + EXPECTED_CALLS + " but found " + discovered + ". Refusing to patch.");
        }
        for (Map.Entry<String, byte[]> entry : entries.entrySet()) {
            if (!entry.getKey().endsWith(".class")) {
                continue;
            }
            byte[] transformed = patchClass(entry.getKey(), entry.getValue());
            if (transformed != entry.getValue()) {
                entry.setValue(transformed);
            }
        }
        byte[] output = writeEntries(entries);
        if (countSetVisibleCalls(output) != 0) {
            throw new IllegalStateException("Patched KGP still contains Configuration.setVisible invocations.");
        }
        return output;
    }

    public static int countSetVisibleCalls(byte[] jarBytes) throws IOException {
        int total = 0;
        for (Map.Entry<String, byte[]> entry : readEntries(jarBytes).entrySet()) {
            if (!entry.getKey().endsWith(".class")) {
                continue;
            }
            ClassNode node = readClass(entry.getValue());
            for (MethodNode method : node.methods) {
                for (AbstractInsnNode instruction : method.instructions) {
                    if (instruction instanceof MethodInsnNode call
                            && CONFIGURATION.equals(call.owner)
                            && "setVisible".equals(call.name)) {
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

    private static Map<String, Integer> discoverCallSites(Map<String, byte[]> entries) {
        Map<String, Integer> discovered = new TreeMap<>();
        for (Map.Entry<String, byte[]> entry : entries.entrySet()) {
            if (!entry.getKey().endsWith(".class")) continue;
            ClassNode node = readClass(entry.getValue());
            for (MethodNode method : node.methods) {
                String key = entry.getKey() + "#" + method.name + method.desc;
                for (AbstractInsnNode instruction : method.instructions) {
                    if (instruction instanceof MethodInsnNode call
                            && CONFIGURATION.equals(call.owner)
                            && "setVisible".equals(call.name)) {
                        discovered.merge(key, 1, Integer::sum);
                    }
                }
            }
        }
        return discovered;
    }

    private static byte[] patchClass(String entryName, byte[] classBytes) {
        ClassNode node = readClass(classBytes);
        int classCalls = 0;
        for (MethodNode method : node.methods) {
            int methodCalls = 0;
            for (AbstractInsnNode instruction = method.instructions.getFirst();
                    instruction != null; ) {
                AbstractInsnNode next = instruction.getNext();
                if (instruction instanceof MethodInsnNode call
                        && CONFIGURATION.equals(call.owner)
                        && "setVisible".equals(call.name)) {
                    String key = entryName + "#" + method.name + method.desc;
                    if (!EXPECTED_CALLS.containsKey(key)
                            || !SET_VISIBLE_DESCRIPTOR.equals(call.desc)
                            || call.getOpcode() != Opcodes.INVOKEINTERFACE
                            || !call.itf) {
                        throw new IllegalArgumentException(
                                "Unexpected KGP setVisible invocation in " + entryName + "#"
                                        + method.name + method.desc + ": " + call);
                    }
                    AbstractInsnNode argument = previousExecutableInstruction(call);
                    AbstractInsnNode discardResult = nextExecutableInstruction(call);
                    if (argument == null || argument.getOpcode() != Opcodes.ICONST_0
                            || discardResult == null || discardResult.getOpcode() != Opcodes.POP) {
                        throw new IllegalArgumentException(
                            "Expected pinned KGP ICONST_0, setVisible, POP sequence in "
                                        + entryName + ".");
                    }
                    // Consuming false here leaves the original receiver for KGP's following POP.
                    // This preserves stack-map frames and leaves no setter or state mutation.
                    method.instructions.set(call, new InsnNode(Opcodes.POP));
                    classCalls++;
                    methodCalls++;
                }
                instruction = next;
            }
            if (methodCalls > 0) {
                String key = entryName + "#" + method.name + method.desc;
                Integer expected = EXPECTED_CALLS.get(key);
                if (expected == null || expected != methodCalls) {
                    throw new IllegalArgumentException(
                            "Unexpected KGP Configuration.setVisible target/count: " + key
                                    + " has " + methodCalls + " calls; expected " + expected + ".");
                }
            }
        }
        if (classCalls == 0) {
            return classBytes;
        }
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

    private static AbstractInsnNode nextExecutableInstruction(AbstractInsnNode instruction) {
        AbstractInsnNode next = instruction.getNext();
        while (next != null && next.getOpcode() < 0) {
            next = next.getNext();
        }
        return next;
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

    static void verifyManifest(byte[] jarBytes) throws IOException {
        byte[] manifestBytes = readEntries(jarBytes).get("META-INF/MANIFEST.MF");
        if (manifestBytes == null) {
            throw new IllegalArgumentException("KGP JAR has no META-INF/MANIFEST.MF entry.");
        }
        Manifest manifest = new Manifest(new ByteArrayInputStream(manifestBytes));
        Attributes attributes = manifest.getMainAttributes();
        String title = attributes.getValue("Implementation-Title");
        String version = attributes.getValue("Implementation-Version");
        if (!"kotlin-gradle-plugin".equals(title)
                || !"2.2.10-release-430".equals(version)) {
            throw new IllegalArgumentException(
                    "Unexpected KGP manifest identity: " + title + " " + version + ".");
        }
    }

    private static void requireHash(byte[] bytes, String expected, String description) {
        String actual;
        try {
            actual = java.util.HexFormat.of().formatHex(
                    MessageDigest.getInstance("SHA-256").digest(bytes));
        } catch (NoSuchAlgorithmException impossible) {
            throw new AssertionError("SHA-256 is required by the Java platform.", impossible);
        }
        if (!expected.equals(actual)) {
            throw new IllegalArgumentException(
                    description + " checksum mismatch: expected " + expected + ", found " + actual + ".");
        }
    }
}
