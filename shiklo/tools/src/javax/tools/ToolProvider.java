package javax.tools;

import java.util.ServiceConfigurationError;
import java.util.ServiceLoader;

/**
 * The JDK's system tools, for a class path that carries them (Shiklo's Java page: javac, javap and jshell come from a
 * class-path jar, not from modules). The JDK's own ToolProvider accepts a tool only from its module (jdk.compiler),
 * so on a class path it found none; this one takes the class the JDK names for each tool, from the loader that
 * loaded javax.tools.
 */
public class ToolProvider {

    private ToolProvider() {}

    public static JavaCompiler getSystemJavaCompiler() {
        return getSystemTool(JavaCompiler.class, "com.sun.tools.javac.api.JavacTool");
    }

    public static DocumentationTool getSystemDocumentationTool() {
        return getSystemTool(DocumentationTool.class, "jdk.javadoc.internal.api.JavadocTool");
    }

    @Deprecated(since = "9")
    public static ClassLoader getSystemToolClassLoader() {
        return null;
    }

    private static <T> T getSystemTool(Class<T> type, String className) {
        try {
            for (T tool : ServiceLoader.load(type, ToolProvider.class.getClassLoader())) {
                if (tool.getClass().getName().equals(className)) return tool;
            }
        } catch (ServiceConfigurationError e) {
            throw new Error(e);
        }
        return null;
    }
}
