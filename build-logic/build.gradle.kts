import org.gradle.api.tasks.bundling.Jar

plugins {
    `java-gradle-plugin`
}

group = "org.deadspeak.build"
version = "1.0.0"

repositories {
    google()
    mavenCentral()
}

val agpPatchSource = configurations.create("agpPatchSource") {
    isCanBeConsumed = false
    isCanBeResolved = true
    isTransitive = false
}
val agpPomSource = configurations.create("agpPomSource") {
    isCanBeConsumed = false
    isCanBeResolved = true
    isTransitive = false
}
val kgpPatchSource = configurations.create("kgpPatchSource") {
    isCanBeConsumed = false
    isCanBeResolved = true
    isTransitive = false
}
val kgpPomSource = configurations.create("kgpPomSource") {
    isCanBeConsumed = false
    isCanBeResolved = true
    isTransitive = false
}

dependencies {
    add(agpPatchSource.name, "com.android.tools.build:gradle:9.4.1")
    add(agpPomSource.name, "com.android.tools.build:gradle:9.4.1@pom")
    add(kgpPatchSource.name, "org.jetbrains.kotlin:kotlin-gradle-plugin:2.2.10:gradle813@jar")
    add(kgpPomSource.name, "org.jetbrains.kotlin:kotlin-gradle-plugin:2.2.10@pom")
    implementation("org.ow2.asm:asm-tree:9.10.1")
    testImplementation("junit:junit:4.13.2")
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

tasks.withType<JavaCompile>().configureEach {
    options.release.set(17)
}

gradlePlugin {
    plugins {
        register("agpVisiblePatch") {
            id = "org.deadspeak.agp-visible-patch"
            implementationClass = "org.deadspeak.build.agppatch.AgpVisiblePatchSettingsPlugin"
        }
        register("androidApplicationDelegate") {
            id = "org.deadspeak.android-application"
            implementationClass = "org.deadspeak.build.agppatch.AndroidApplicationPlugin"
        }
    }
}

val stageAgpPatchInputs = tasks.register("stageAgpPatchInputs") {
    val outputDir = rootProject.projectDir.parentFile.resolve("build/agp-patch-source")
    val kgpOutputDir = rootProject.projectDir.parentFile.resolve("build/kgp-patch-source")
    inputs.files(agpPatchSource, agpPomSource, kgpPatchSource, kgpPomSource)
    outputs.dir(outputDir)
    outputs.dir(kgpOutputDir)

    doLast {
        val artifacts = agpPatchSource.resolve().filter { it.extension == "jar" }
        check(artifacts.size == 1) {
            "Expected one AGP 9.4.1 JAR from the pinned coordinate, found: $artifacts"
        }

        val poms = agpPomSource.resolve().filter { it.extension == "pom" }
        outputDir.mkdirs()
        artifacts.single().copyTo(outputDir.resolve("gradle-9.4.1.jar"), overwrite = true)
        check(poms.size == 1) {
            "Expected one AGP 9.4.1 POM from the explicit @pom artifact, found: $poms"
        }
        poms.single().copyTo(outputDir.resolve("gradle-9.4.1.pom"), overwrite = true)

        val kgpJars = kgpPatchSource.resolve().filter { it.extension == "jar" }
        check(kgpJars.size == 1) {
            "Expected one KGP 2.2.10 gradle813 JAR from the pinned classifier, found: $kgpJars"
        }
        val kgpPoms = kgpPomSource.resolve().filter { it.extension == "pom" }
        check(kgpPoms.size == 1) {
            "Expected one KGP 2.2.10 POM from the explicit @pom artifact, found: $kgpPoms"
        }
        kgpOutputDir.mkdirs()
        kgpJars.single().copyTo(
            kgpOutputDir.resolve("kotlin-gradle-plugin-2.2.10-gradle813.jar"), overwrite = true)
        kgpPoms.single().copyTo(
            kgpOutputDir.resolve("kotlin-gradle-plugin-2.2.10.pom"), overwrite = true)
    }
}

tasks.named<Jar>("jar") {
    dependsOn(stageAgpPatchInputs)
}

tasks.register<JavaExec>("verifyAgpPatch") {
    dependsOn(stageAgpPatchInputs)
    dependsOn(tasks.named("classes"))
    classpath = sourceSets.main.get().runtimeClasspath
    mainClass.set("org.deadspeak.build.agppatch.AgpVisiblePatcher")
    val rootDir = rootProject.projectDir.parentFile
    args(
        rootDir.resolve("build/agp-patch-source/gradle-9.4.1.jar").absolutePath,
        rootDir.resolve("build/agp-patch-source/gradle-9.4.1.pom").absolutePath,
        rootDir.resolve("build/kgp-patch-source/kotlin-gradle-plugin-2.2.10-gradle813.jar").absolutePath,
        rootDir.resolve("build/kgp-patch-source/kotlin-gradle-plugin-2.2.10.pom").absolutePath,
        rootDir.resolve("build/agp-patched-maven-verification").absolutePath,
    )
}

tasks.register<JavaExec>("scanKgpVisibleCalls") {
    dependsOn(stageAgpPatchInputs)
    dependsOn(tasks.named("classes"))
    classpath = sourceSets.main.get().runtimeClasspath
    mainClass.set("org.deadspeak.build.agppatch.KgpVisiblePatcher")
    args(
        "--scan",
        rootProject.projectDir.parentFile
            .resolve("build/kgp-patch-source/kotlin-gradle-plugin-2.2.10-gradle813.jar")
            .absolutePath,
    )
}
