package org.deadspeak.build.agppatch;

import java.io.IOException;
import java.nio.file.Path;
import org.gradle.api.Plugin;
import org.gradle.api.initialization.Settings;
import org.gradle.api.artifacts.repositories.MavenArtifactRepository;

/** Creates a deterministic, project-local AGP artifact and routes the Android plugin to it. */
public final class AgpVisiblePatchSettingsPlugin implements Plugin<Settings> {
    @Override
    public void apply(Settings settings) {
        Path projectBuildDir = settings.getRootDir().toPath().resolve("build");
        Path sourceDir = projectBuildDir.resolve("agp-patch-source");
        Path sourceJar = sourceDir.resolve("gradle-9.4.1.jar");
        Path sourcePom = sourceDir.resolve("gradle-9.4.1.pom");
        Path repository = projectBuildDir.resolve("agp-patched-maven");

        try {
            AgpVisiblePatcher.prepareMavenRepository(
                    sourceJar,
                    sourcePom,
                    projectBuildDir.resolve("kgp-patch-source/kotlin-gradle-plugin-2.2.10-gradle813.jar"),
                    projectBuildDir.resolve("kgp-patch-source/kotlin-gradle-plugin-2.2.10.pom"),
                    repository);
        } catch (IOException | IllegalArgumentException exception) {
            throw new IllegalStateException(
                    "Could not prepare the checksum-pinned AGP 9.4.1/KGP 2.2.10 patch. "
                            + "Run the included build's stageAgpPatchInputs task and inspect its output.",
                    exception);
        }

        settings.getPluginManagement().getRepositories().exclusiveContent(exclusive -> {
            exclusive.forRepository(() -> settings.getPluginManagement().getRepositories().maven(repositorySpec -> {
                repositorySpec.setName("deadspeakPatchedAgp");
                repositorySpec.setUrl(repository.toUri());
                repositorySpec.metadataSources(MavenArtifactRepository.MetadataSources::mavenPom);
            }));
            exclusive.filter(filter -> {
                filter.includeModule("org.deadspeak.build-patches", "agp-gradle");
                filter.includeModule("org.deadspeak.build-patches", "kotlin-gradle-plugin");
            });
        });

        settings.getPluginManagement().getResolutionStrategy().eachPlugin(request -> {
            if (!"com.android.application".equals(request.getRequested().getId().getId())) {
                return;
            }
            String requestedVersion = request.getRequested().getVersion();
            if (requestedVersion != null && !AgpVisiblePatcher.UPSTREAM_VERSION.equals(requestedVersion)) {
                throw new IllegalStateException(
                        "The local AGP patch is pinned to " + AgpVisiblePatcher.UPSTREAM_VERSION
                                + " but com.android.application requested " + requestedVersion + ".");
            }
            request.useModule(AgpVisiblePatcher.PATCHED_COORDINATE);
        });
    }
}
