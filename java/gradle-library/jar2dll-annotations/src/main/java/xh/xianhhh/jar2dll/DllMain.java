package xh.xianhhh.jar2dll;

import java.lang.annotation.Documented;
import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/**
 * Marks the single Java method invoked after the generated DLL is loaded.
 *
 * <p>The annotated method must be {@code static void} and take no arguments.
 * The Gradle plugin validates these constraints while executing {@code buildDll}.</p>
 */
@Documented
@Retention(RetentionPolicy.CLASS)
@Target(ElementType.METHOD)
public @interface DllMain {
}
