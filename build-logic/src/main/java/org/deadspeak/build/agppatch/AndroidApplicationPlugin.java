package org.deadspeak.build.agppatch;

import org.gradle.api.Plugin;
import org.gradle.api.Project;

/** Applies the project's checksum-patched AGP application plugin. */
public final class AndroidApplicationPlugin implements Plugin<Project> {
    @Override
    public void apply(Project project) {
        project.getPluginManager().apply("com.android.application");
    }
}
