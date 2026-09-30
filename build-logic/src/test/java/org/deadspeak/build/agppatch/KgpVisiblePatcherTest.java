package org.deadspeak.build.agppatch;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertSame;
import static org.junit.Assert.assertThrows;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.lang.reflect.Proxy;
import java.util.Map;
import java.util.TreeMap;
import java.util.jar.Manifest;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;
import org.gradle.api.Project;
import org.gradle.api.artifacts.Configuration;
import org.junit.Test;
import org.objectweb.asm.ClassWriter;
import org.objectweb.asm.Label;
import org.objectweb.asm.MethodVisitor;
import org.objectweb.asm.Opcodes;
import org.objectweb.asm.Type;
import org.objectweb.asm.tree.AbstractInsnNode;
import org.objectweb.asm.tree.ClassNode;
import org.objectweb.asm.tree.FrameNode;

public final class KgpVisiblePatcherTest {
    @Test
    public void readsValidManifestWhenItIsNotTheFirstZipEntry() throws Exception {
        KgpVisiblePatcher.verifyManifest(fixtureJar(1, true, null, false, "apply"));
    }

    @Test
    public void rejectsChangedPinnedArtifactChecksum() {
        IllegalArgumentException error = assertThrows(
                IllegalArgumentException.class,
                () -> KgpVisiblePatcher.verifyPinnedArtifact(new byte[] {1}, new byte[] {2}));
        org.junit.Assert.assertTrue(error.getMessage().contains("checksum mismatch"));
    }

    @Test
    public void rejectsSignedJarEntries() throws Exception {
        byte[] source = fixtureJar(1, true, null, false, "apply");
        TreeMap<String, byte[]> entries = new TreeMap<>(readJar(source));
        entries.put("META-INF/DEADSPEAK.SF", new byte[] {1, 2, 3});
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream output = new ZipOutputStream(bytes)) {
            for (Map.Entry<String, byte[]> entry : entries.entrySet()) {
                output.putNextEntry(new ZipEntry(entry.getKey()));
                output.write(entry.getValue());
                output.closeEntry();
            }
        }
        IllegalArgumentException error = assertThrows(
                IllegalArgumentException.class,
                () -> KgpVisiblePatcher.transformJar(bytes.toByteArray()));
        org.junit.Assert.assertTrue(error.getMessage().contains("signed JAR entry"));
    }

    @Test
    public void removesTheSetterKeepsTheExplicitFrameAndExecutes() throws Exception {
        byte[] patched = KgpVisiblePatcher.transformJar(fixtureJar(1, true, null, false, "apply"));
        Map<String, byte[]> entries = readJar(patched);

        assertEquals(0, KgpVisiblePatcher.countSetVisibleCalls(patched));
        assertArrayEquals(new byte[] {9, 8, 7}, entries.get("marker.bin"));
        assertFrameAndTwoPopsRemain(entries.get(KgpVisiblePatcher.CLASS_ENTRY));
        invokeApply(entries.get(KgpVisiblePatcher.CLASS_ENTRY));
    }

    @Test
    public void transformationIsByteForByteDeterministic() throws Exception {
        byte[] source = fixtureJar(1, true, null, false, "apply");
        assertArrayEquals(
                KgpVisiblePatcher.transformJar(source), KgpVisiblePatcher.transformJar(source));
    }

    @Test
    public void rejectsMissingOrUnexpectedCallSites() throws Exception {
        assertThrows(IllegalArgumentException.class,
                () -> KgpVisiblePatcher.transformJar(fixtureJar(0, true, null, false, "apply")));
        assertThrows(IllegalArgumentException.class,
                () -> KgpVisiblePatcher.transformJar(
                        fixtureJar(0, true, "other/Unexpected.class", false, "apply")));
        assertThrows(IllegalArgumentException.class,
                () -> KgpVisiblePatcher.transformJar(fixtureJar(2, true, null, false, "apply")));
        assertThrows(IllegalArgumentException.class,
                () -> KgpVisiblePatcher.transformJar(fixtureJar(1, true, null, false, "other")));
    }

    @Test
    public void rejectsChangedOperandAndMissingResultDiscard() throws Exception {
        assertThrows(IllegalArgumentException.class,
                () -> KgpVisiblePatcher.transformJar(fixtureJar(1, false, null, false, "apply")));
        assertThrows(IllegalArgumentException.class,
                () -> KgpVisiblePatcher.transformJar(fixtureJar(1, true, null, true, "apply")));
    }

    private static void assertFrameAndTwoPopsRemain(byte[] classBytes) {
        ClassNode node = new ClassNode(Opcodes.ASM9);
        new org.objectweb.asm.ClassReader(classBytes).accept(node, 0);
        boolean frameFound = false;
        int pops = 0;
        for (AbstractInsnNode instruction : node.methods.stream()
                .filter(method -> "apply".equals(method.name)).findFirst().orElseThrow().instructions) {
            frameFound |= instruction instanceof FrameNode;
            if (instruction.getOpcode() == Opcodes.POP) {
                pops++;
            }
        }
        org.junit.Assert.assertTrue("The stack-map frame between ICONST_0 and POP must remain.", frameFound);
        assertEquals("The replacement POP and original result POP both remain.", 2, pops);
    }

    private static void invokeApply(byte[] classBytes) throws Exception {
        ClassLoader loader = new ClassLoader(KgpVisiblePatcherTest.class.getClassLoader()) {
            @Override
            protected Class<?> findClass(String name) throws ClassNotFoundException {
                if ("org.jetbrains.kotlin.gradle.plugin.DefaultKotlinBasePlugin".equals(name)) {
                    return defineClass(name, classBytes, 0, classBytes.length);
                }
                return super.findClass(name);
            }
        };
        Class<?> fixture = loader.loadClass("org.jetbrains.kotlin.gradle.plugin.DefaultKotlinBasePlugin");
        Configuration receiver = (Configuration) Proxy.newProxyInstance(
                KgpVisiblePatcherTest.class.getClassLoader(),
                new Class<?>[] {Configuration.class},
                (proxy, method, args) -> {
                    if ("setVisible".equals(method.getName())) {
                        throw new AssertionError("Patched KGP must not call setVisible.");
                    }
                    return null;
                });
        fixture.getField("receiver").set(null, receiver);
        Object instance = fixture.getConstructor().newInstance();
        fixture.getMethod("apply", Project.class).invoke(instance, new Object[] {null});
        assertSame(receiver, fixture.getField("receiver").get(null));
    }

    private static byte[] fixtureJar(
            int targetCallCount,
            boolean falseOperand,
            String extraClass,
            boolean omitResultPop,
            String targetMethodName) throws Exception {
        TreeMap<String, java.util.List<FixtureMethod>> methodsByClass = new TreeMap<>();
        for (Map.Entry<String, Integer> expected : KgpVisiblePatcher.expectedCalls().entrySet()) {
            int separator = expected.getKey().indexOf('#');
            String classEntry = expected.getKey().substring(0, separator);
            String methodAndDescriptor = expected.getKey().substring(separator + 1);
            int descriptorStart = methodAndDescriptor.indexOf('(');
            String methodName = methodAndDescriptor.substring(0, descriptorStart);
            String descriptor = methodAndDescriptor.substring(descriptorStart);
            boolean target = KgpVisiblePatcher.CLASS_ENTRY.equals(classEntry);
            methodsByClass.computeIfAbsent(classEntry, ignored -> new java.util.ArrayList<>())
                    .add(new FixtureMethod(
                            target ? targetMethodName : methodName,
                            descriptor,
                            target ? targetCallCount : expected.getValue(),
                            target ? falseOperand : true,
                            target && omitResultPop,
                            target));
        }

        TreeMap<String, byte[]> entries = new TreeMap<>();
        for (Map.Entry<String, java.util.List<FixtureMethod>> entry : methodsByClass.entrySet()) {
            String internalName = entry.getKey().substring(0, entry.getKey().length() - ".class".length());
            entries.put(entry.getKey(), fixtureClass(internalName, entry.getValue()));
        }
        if (extraClass != null) {
            entries.put(extraClass, fixtureClass(
                    "other/Unexpected",
                    java.util.List.of(new FixtureMethod(
                            "apply", "(Lorg/gradle/api/Project;)V", 1, falseOperand,
                            omitResultPop, false))));
        }
        entries.put("marker.bin", new byte[] {9, 8, 7});

        Manifest manifest = new Manifest();
        manifest.getMainAttributes().putValue("Manifest-Version", "1.0");
        manifest.getMainAttributes().putValue("Implementation-Title", "kotlin-gradle-plugin");
        manifest.getMainAttributes().putValue("Implementation-Version", "2.2.10-release-430");
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        try (ZipOutputStream output = new ZipOutputStream(bytes)) {
            for (Map.Entry<String, byte[]> entry : entries.entrySet()) {
                ZipEntry zipEntry = new ZipEntry(entry.getKey());
                zipEntry.setTime(0L);
                output.putNextEntry(zipEntry);
                output.write(entry.getValue());
                output.closeEntry();
            }
            ZipEntry manifestEntry = new ZipEntry("META-INF/MANIFEST.MF");
            manifestEntry.setTime(0L);
            output.putNextEntry(manifestEntry);
            manifest.write(output);
            output.closeEntry();
        }
        return bytes.toByteArray();
    }

    private static byte[] fixtureClass(String internalName, java.util.List<FixtureMethod> methods) {
        ClassWriter writer = new ClassWriter(0);
        writer.visit(Opcodes.V17, Opcodes.ACC_PUBLIC, internalName, null, "java/lang/Object", null);

        writer.visitField(
                Opcodes.ACC_PUBLIC | Opcodes.ACC_STATIC,
                "receiver",
                "Lorg/gradle/api/artifacts/Configuration;",
                null,
                null).visitEnd();

        MethodVisitor constructor = writer.visitMethod(Opcodes.ACC_PUBLIC, "<init>", "()V", null, null);
        constructor.visitCode();
        constructor.visitVarInsn(Opcodes.ALOAD, 0);
        constructor.visitMethodInsn(Opcodes.INVOKESPECIAL, "java/lang/Object", "<init>", "()V", false);
        constructor.visitInsn(Opcodes.RETURN);
        constructor.visitMaxs(1, 1);
        constructor.visitEnd();

        for (FixtureMethod fixtureMethod : methods) {
            MethodVisitor method = writer.visitMethod(
                    Opcodes.ACC_PUBLIC,
                    fixtureMethod.name,
                    fixtureMethod.descriptor,
                    null,
                    null);
            method.visitCode();
            for (int callIndex = 0; callIndex < fixtureMethod.callCount; callIndex++) {
                method.visitFieldInsn(
                        Opcodes.GETSTATIC,
                        internalName,
                        "receiver",
                        "Lorg/gradle/api/artifacts/Configuration;");
                method.visitInsn(fixtureMethod.falseOperand ? Opcodes.ICONST_0 : Opcodes.ICONST_1);
                if (fixtureMethod.explicitFrame) {
                    Label frame = new Label();
                    method.visitLabel(frame);
                    method.visitFrame(
                            Opcodes.F_FULL,
                            2,
                            new Object[] {internalName, "org/gradle/api/Project"},
                            2,
                            new Object[] {"org/gradle/api/artifacts/Configuration", Opcodes.INTEGER});
                }
                method.visitMethodInsn(
                        Opcodes.INVOKEINTERFACE,
                        "org/gradle/api/artifacts/Configuration",
                        "setVisible",
                        "(Z)Lorg/gradle/api/artifacts/Configuration;",
                        true);
                if (!fixtureMethod.omitResultPop) {
                    method.visitInsn(Opcodes.POP);
                }
            }
            Type returnType = Type.getReturnType(fixtureMethod.descriptor);
            switch (returnType.getSort()) {
                case Type.VOID -> method.visitInsn(Opcodes.RETURN);
                case Type.OBJECT, Type.ARRAY -> {
                    method.visitInsn(Opcodes.ACONST_NULL);
                    method.visitInsn(Opcodes.ARETURN);
                }
                case Type.LONG -> {
                    method.visitInsn(Opcodes.LCONST_0);
                    method.visitInsn(Opcodes.LRETURN);
                }
                case Type.FLOAT -> {
                    method.visitInsn(Opcodes.FCONST_0);
                    method.visitInsn(Opcodes.FRETURN);
                }
                case Type.DOUBLE -> {
                    method.visitInsn(Opcodes.DCONST_0);
                    method.visitInsn(Opcodes.DRETURN);
                }
                default -> {
                    method.visitInsn(Opcodes.ICONST_0);
                    method.visitInsn(Opcodes.IRETURN);
                }
            }
            method.visitMaxs(2, Type.getArgumentsAndReturnSizes(fixtureMethod.descriptor) >>> 2);
            method.visitEnd();
        }
        writer.visitEnd();
        return writer.toByteArray();
    }

    private static final class FixtureMethod {
        private final String name;
        private final String descriptor;
        private final int callCount;
        private final boolean falseOperand;
        private final boolean omitResultPop;
        private final boolean explicitFrame;

        private FixtureMethod(
                String name, String descriptor, int callCount, boolean falseOperand,
                boolean omitResultPop, boolean explicitFrame) {
            this.name = name;
            this.descriptor = descriptor;
            this.callCount = callCount;
            this.falseOperand = falseOperand;
            this.omitResultPop = omitResultPop;
            this.explicitFrame = explicitFrame;
        }
    }

    private static Map<String, byte[]> readJar(byte[] bytes) throws Exception {
        TreeMap<String, byte[]> entries = new TreeMap<>();
        try (ZipInputStream input = new ZipInputStream(new ByteArrayInputStream(bytes))) {
            ZipEntry entry;
            while ((entry = input.getNextEntry()) != null) {
                entries.put(entry.getName(), input.readAllBytes());
            }
        }
        return entries;
    }
}
