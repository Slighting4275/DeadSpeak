package org.deadspeak.build.agppatch;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertThrows;

import java.io.ByteArrayOutputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Map;
import java.util.TreeMap;
import java.util.jar.Manifest;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;
import org.gradle.api.artifacts.Configuration;
import org.junit.Test;
import org.junit.Rule;
import org.junit.rules.TemporaryFolder;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.ClassReader;
import org.objectweb.asm.MethodVisitor;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.tree.AbstractInsnNode;
import org.objectweb.asm.tree.ClassNode;
import org.objectweb.asm.tree.FrameNode;
import org.objectweb.asm.tree.InsnNode;

public final class AgpVisiblePatcherTest {
    @Rule
    public final TemporaryFolder temporaryFolder = new TemporaryFolder();

    @Test
    public void removesEveryPinnedInvocationAndPreservesTheReceiver() throws Exception {
        byte[] source = fixtureJar(AgpVisiblePatcher.expectedCalls(), false, false, true);

        byte[] patched = AgpVisiblePatcher.transformJar(source);

        assertEquals(0, AgpVisiblePatcher.countSetVisibleCalls(patched));
        assertStackMapFrameAndPopRemain(patched);
        invokeFirstFixtureMethod(patched);
    }

    @Test
    public void transformationIsByteForByteDeterministic() throws Exception {
        byte[] source = fixtureJar(AgpVisiblePatcher.expectedCalls(), false);

        assertArrayEquals(
                AgpVisiblePatcher.transformJar(source),
                AgpVisiblePatcher.transformJar(source));
    }

    @Test
    public void transformationIsIndependentOfTheMachineTimeZone() throws Exception {
        byte[] source = fixtureJar(AgpVisiblePatcher.expectedCalls(), false);
        java.util.TimeZone original = java.util.TimeZone.getDefault();
        try {
            java.util.TimeZone.setDefault(java.util.TimeZone.getTimeZone("Pacific/Honolulu"));
            byte[] western = AgpVisiblePatcher.transformJar(source);
            java.util.TimeZone.setDefault(java.util.TimeZone.getTimeZone("Pacific/Kiritimati"));
            assertArrayEquals(western, AgpVisiblePatcher.transformJar(source));
        } finally {
            java.util.TimeZone.setDefault(original);
        }
    }

    @Test
    public void rejectsAnUnrecognizedCallTargetOrCount() throws Exception {
        Map<String, Integer> unexpected = new TreeMap<>(AgpVisiblePatcher.expectedCalls());
        unexpected.put("com/android/build/gradle/internal/plugins/BasePlugin$Companion.class", 2);

        assertThrows(
                IllegalArgumentException.class,
                () -> AgpVisiblePatcher.transformJar(fixtureJar(unexpected, false)));
    }

    @Test
    public void rejectsMissingPinnedClasses() throws Exception {
        Map<String, Integer> missing = new TreeMap<>(AgpVisiblePatcher.expectedCalls());
        missing.remove(missing.keySet().iterator().next());

        assertThrows(
                IllegalArgumentException.class,
                () -> AgpVisiblePatcher.transformJar(fixtureJar(missing, false)));
    }

    @Test
    public void rejectsAdditionalSetterClasses() throws Exception {
        Map<String, Integer> extra = new TreeMap<>(AgpVisiblePatcher.expectedCalls());
        extra.put("unexpected/AdditionalSetter.class", 1);

        assertThrows(
                IllegalArgumentException.class,
                () -> AgpVisiblePatcher.transformJar(fixtureJar(extra, false)));
    }

    @Test
    public void preservesUntargetedClassAndResourceBytes() throws Exception {
        byte[] source = fixtureJar(AgpVisiblePatcher.expectedCalls(), false);
        Map<String, byte[]> before = readJar(source);
        Map<String, byte[]> after = readJar(AgpVisiblePatcher.transformJar(source));

        assertEquals(before.keySet(), after.keySet());
        for (String name : before.keySet()) {
            if (!AgpVisiblePatcher.expectedCalls().containsKey(name)) {
                assertArrayEquals("Unpatched entry changed: " + name, before.get(name), after.get(name));
            }
        }
    }

    @Test
    public void rejectsChangedSetterOperandBytecode() throws Exception {
        assertThrows(
                IllegalArgumentException.class,
                () -> AgpVisiblePatcher.transformJar(
                        fixtureJar(AgpVisiblePatcher.expectedCalls(), true)));
    }

    @Test
    public void rejectsAnUpstreamChecksumMismatch() throws Exception {
        byte[] source = fixtureJar(AgpVisiblePatcher.expectedCalls(), false);
        Path sourceJar = temporaryFolder.getRoot().toPath().resolve("wrong.jar");
        Path sourcePom = temporaryFolder.getRoot().toPath().resolve("wrong.pom");
        Files.write(sourceJar, source);
        Files.writeString(sourcePom, "not the pinned AGP POM");

        IllegalArgumentException error = assertThrows(
                IllegalArgumentException.class,
                () -> AgpVisiblePatcher.prepareMavenRepository(
                        sourceJar, sourcePom, sourceJar, sourcePom,
                        temporaryFolder.getRoot().toPath().resolve("repo")));
        org.junit.Assert.assertTrue(error.getMessage().contains("checksum mismatch"));
    }

    @Test
    public void rejectsSignedJarEntries() throws Exception {
        IllegalArgumentException error = assertThrows(
                IllegalArgumentException.class,
                () -> AgpVisiblePatcher.transformJar(
                        fixtureJar(AgpVisiblePatcher.expectedCalls(), false, true)));
        org.junit.Assert.assertTrue(error.getMessage().contains("signed JAR entry"));
    }

    @Test
    public void generatedFilesAreComparedAndAtomicallyReplacedOnlyWhenChanged() throws Exception {
        Path destination = temporaryFolder.getRoot().toPath().resolve("nested/generated.jar");
        byte[] first = new byte[] {1, 2, 3};
        byte[] replacement = new byte[] {4, 5};

        assertEquals(true, AgpVisiblePatcher.writeIfChangedAtomically(destination, first));
        assertEquals(false, AgpVisiblePatcher.writeIfChangedAtomically(destination, first));
        assertArrayEquals(first, Files.readAllBytes(destination));
        assertEquals(true, AgpVisiblePatcher.writeIfChangedAtomically(destination, replacement));
        assertArrayEquals(replacement, Files.readAllBytes(destination));
    }

    private static void invokeFirstFixtureMethod(byte[] jarBytes) throws Exception {
        Map<String, byte[]> entries = readJar(jarBytes);
        String entryName = "com/android/build/gradle/internal/plugins/BasePlugin$Companion.class";
        byte[] classBytes = entries.get(entryName);
        ClassLoader loader = new ClassLoader(AgpVisiblePatcherTest.class.getClassLoader()) {
            @Override
            protected Class<?> findClass(String name) throws ClassNotFoundException {
                if ("com.android.build.gradle.internal.plugins.BasePlugin$Companion".equals(name)) {
                    return defineClass(name, classBytes, 0, classBytes.length);
                }
                return super.findClass(name);
            }
        };
        Class<?> fixtureClass = loader.loadClass(
                "com.android.build.gradle.internal.plugins.BasePlugin$Companion");
        Configuration receiver = (Configuration) Proxy.newProxyInstance(
                AgpVisiblePatcherTest.class.getClassLoader(),
                new Class<?>[] {Configuration.class},
                (proxy, method, args) -> {
                    if ("setVisible".equals(method.getName())) {
                        throw new AssertionError("The patched method must not call setVisible.");
                    }
                    return null;
                });
        try {
            Object result = fixtureClass.getMethod("call0", Configuration.class).invoke(null, receiver);
            assertSame(receiver, result);
        } catch (InvocationTargetException exception) {
            throw new AssertionError("Patched receiver-returning bytecode failed.", exception.getCause());
        }
    }

    private static void assertStackMapFrameAndPopRemain(byte[] jarBytes) throws Exception {
        String entryName = "com/android/build/gradle/internal/plugins/BasePlugin$Companion.class";
        ClassNode node = new ClassNode(Opcodes.ASM9);
        new ClassReader(readJar(jarBytes).get(entryName)).accept(node, 0);
        boolean frameFound = false;
        boolean popFound = false;
        for (AbstractInsnNode instruction : node.methods.get(0).instructions) {
            frameFound |= instruction instanceof FrameNode;
            popFound |= instruction instanceof InsnNode && instruction.getOpcode() == Opcodes.POP;
        }
        org.junit.Assert.assertTrue("The explicit stack-map frame must survive patching.", frameFound);
        org.junit.Assert.assertTrue("The setter call must become POP.", popFound);
    }

    private static byte[] fixtureJar(Map<String, Integer> targets, boolean useTrue) throws Exception {
        return fixtureJar(targets, useTrue, false, false);
    }

    private static byte[] fixtureJar(Map<String, Integer> targets, boolean useTrue, boolean signed)
            throws Exception {
        return fixtureJar(targets, useTrue, signed, false);
    }

    private static byte[] fixtureJar(
            Map<String, Integer> targets, boolean useTrue, boolean signed, boolean includeStackMapFrame)
            throws Exception {
        TreeMap<String, byte[]> entries = new TreeMap<>();
        for (Map.Entry<String, Integer> target : targets.entrySet()) {
            String internalName = target.getKey().substring(0, target.getKey().length() - ".class".length());
            entries.put(target.getKey(), fixtureClass(
                    internalName, target.getValue(), useTrue, includeStackMapFrame));
        }
        entries.put("untargeted/Unchanged.class", fixtureClass("untargeted/Unchanged", 0, false, false));
        entries.put("META-INF/untargeted-resource.bin", new byte[] {0, 1, 2, (byte) 255});

        Manifest manifest = new Manifest();
        manifest.getMainAttributes().putValue("Manifest-Version", "1.0");
        manifest.getMainAttributes().putValue("Plugin-Version", "9.4.1");
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream output = new ZipOutputStream(bytes)) {
            ZipEntry manifestEntry = new ZipEntry("META-INF/MANIFEST.MF");
            manifestEntry.setTime(0L);
            output.putNextEntry(manifestEntry);
            manifest.write(output);
            output.closeEntry();
            if (signed) {
                ZipEntry signature = new ZipEntry("META-INF/AGP.SF");
                signature.setTime(0L);
                output.putNextEntry(signature);
                output.write(new byte[] {1});
                output.closeEntry();
            }
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

    private static byte[] fixtureClass(
            String internalName, int callCount, boolean useTrue, boolean includeStackMapFrame) {
        ClassWriter writer = new ClassWriter(0);
        writer.visit(Opcodes.V17, Opcodes.ACC_PUBLIC, internalName, null, "java/lang/Object", null);
        for (int i = 0; i < callCount; i++) {
            MethodVisitor method = writer.visitMethod(
                    Opcodes.ACC_PUBLIC | Opcodes.ACC_STATIC,
                    "call" + i,
                    "(Lorg/gradle/api/artifacts/Configuration;)Lorg/gradle/api/artifacts/Configuration;",
                    null,
                    null);
            method.visitCode();
            method.visitVarInsn(Opcodes.ALOAD, 0);
            method.visitInsn(useTrue ? Opcodes.ICONST_1 : Opcodes.ICONST_0);
            if (includeStackMapFrame) {
                method.visitLabel(new org.objectweb.asm.Label());
                method.visitFrame(
                        Opcodes.F_FULL,
                        1,
                        new Object[] {"org/gradle/api/artifacts/Configuration"},
                        2,
                        new Object[] {"org/gradle/api/artifacts/Configuration", Opcodes.INTEGER});
            }
            method.visitMethodInsn(
                    Opcodes.INVOKEINTERFACE,
                    "org/gradle/api/artifacts/Configuration",
                    "setVisible",
                    "(Z)Lorg/gradle/api/artifacts/Configuration;",
                    true);
            method.visitInsn(Opcodes.ARETURN);
            method.visitMaxs(2, 1);
            method.visitEnd();
        }
        writer.visitEnd();
        return writer.toByteArray();
    }

    private static Map<String, byte[]> readJar(byte[] bytes) throws Exception {
        TreeMap<String, byte[]> entries = new TreeMap<>();
        try (java.util.zip.ZipInputStream input = new java.util.zip.ZipInputStream(
                new java.io.ByteArrayInputStream(bytes))) {
            ZipEntry entry;
            while ((entry = input.getNextEntry()) != null) {
                entries.put(entry.getName(), input.readAllBytes());
            }
        }
        return entries;
    }
}
