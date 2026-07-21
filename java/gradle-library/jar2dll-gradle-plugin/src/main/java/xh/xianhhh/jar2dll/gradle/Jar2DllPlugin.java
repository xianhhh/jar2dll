package xh.xianhhh.jar2dll.gradle;

import org.gradle.api.Plugin;
import org.gradle.api.Project;
import org.gradle.api.Task;
import org.gradle.api.plugins.JavaPlugin;
import org.gradle.api.tasks.SourceSet;
import org.gradle.api.tasks.SourceSetContainer;
import org.gradle.api.tasks.bundling.Jar;

import java.util.List;

/** Registers {@code buildDll} for Java projects. */
public final class Jar2DllPlugin implements Plugin<Project> {
    @Override
    public void apply(Project project) {
        Jar2DllExtension extension = project.getExtensions().create("jar2dll", Jar2DllExtension.class);
        extension.getOutputName().convention(project.getName() + ".dll");
        extension.getPlatform().convention("x64");
        extension.getConfiguration().convention("Release");
        extension.getCanDump().convention(true);
        extension.getClassLoaders().convention(List.of());

        project.getPluginManager().withPlugin("java", ignored -> configure(project, extension));
        project.getPluginManager().withPlugin("java-library", ignored -> configure(project, extension));
    }

    private static void configure(Project project, Jar2DllExtension extension) {
        if (project.getTasks().findByName("buildDll") != null) return;
        SourceSetContainer sourceSets = project.getExtensions().getByType(SourceSetContainer.class);
        Task jarTask = project.getTasks().named(JavaPlugin.JAR_TASK_NAME).get();
        if (!(jarTask instanceof Jar)) {
            throw new IllegalStateException("The java plugin did not create a Jar task");
        }

        project.getTasks().register("buildDll", BuildDllTask.class, task -> {
            task.setGroup("build");
            task.setDescription("Builds a DLL using the method marked with @DllMain.");
            task.dependsOn(jarTask);
            task.getInputJar().set(((Jar) jarTask).getArchiveFile());
            task.getClassDirectories().from(sourceSets.getByName(SourceSet.MAIN_SOURCE_SET_NAME).getOutput().getClassesDirs());
            task.getOutputName().set(extension.getOutputName());
            task.getPlatform().set(extension.getPlatform());
            task.getConfiguration().set(extension.getConfiguration());
            task.getCanDump().set(extension.getCanDump());
            task.getClassLoaders().set(extension.getClassLoaders());
            task.getLibraries().from(extension.getLibraries());
            task.getConfiguredBuilder().set(extension.getBuilderExecutable());
            task.getConfiguredOutput().set(extension.getOutputFile());
            task.getDefaultOutputDirectory().set(project.getLayout().getBuildDirectory().dir("libs"));
        });
    }
}
