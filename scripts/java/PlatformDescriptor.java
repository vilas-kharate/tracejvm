import java.io.FileOutputStream;
import java.io.OutputStream;
import java.lang.module.ModuleDescriptor;
import java.lang.module.ModuleFinder;
import java.util.Set;
import java.util.TreeSet;
import jdk.internal.module.ModuleInfoWriter;

/**
 * Writes the java.base module-info.class that TeaVM javac compiles against for one runtime profile.
 *
 * <p>TeaVM javac learns java.base's packages from a module descriptor. The TeaVM class library's own descriptor lists
 * only TeaVM's packages, so programs could not import java.util.concurrent.locks, java.sql, javax.xml and the rest
 * although the profile's classes are in the platform jar. This descriptor exports what the profile's real modules
 * export without qualification (internal packages stay hidden, as with a real JDK) and lists every package the jar
 * holds. It is read by the compiler only; the runner keeps the real modules.
 *
 * <p>Usage: java --add-exports java.base/jdk.internal.module=ALL-UNNAMED PlatformDescriptor
 * module1,module2,... extraPackage1,extraPackage2,... output/module-info.class (Shiklo fork, 2026-10-28)
 */
public final class PlatformDescriptor {
  public static void main(String[] args) throws Exception {
    Set<String> exported = new TreeSet<>();
    Set<String> packages = new TreeSet<>();
    ModuleFinder system = ModuleFinder.ofSystem();
    for (String name : args[0].split(",")) {
      ModuleDescriptor module =
          system.find(name).orElseThrow(() -> new IllegalArgumentException("No module " + name)).descriptor();
      packages.addAll(module.packages());
      for (ModuleDescriptor.Exports export : module.exports()) {
        if (!export.isQualified()) {
          exported.add(export.source());
        }
      }
    }
    if (!args[1].isEmpty()) {
      for (String extra : args[1].split(",")) {
        packages.add(extra);
      }
    }
    ModuleDescriptor.Builder builder = ModuleDescriptor.newModule("java.base").packages(packages);
    exported.forEach(builder::exports);
    try (OutputStream out = new FileOutputStream(args[2])) {
      ModuleInfoWriter.write(builder.build(), out);
    }
    System.out.println("java.base descriptor: " + exported.size() + " exported of " + packages.size() + " packages");
  }
}
