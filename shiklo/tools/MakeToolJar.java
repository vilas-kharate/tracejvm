import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.lang.module.ModuleDescriptor;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.stream.Stream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

/**
 * Makes a class-path jar of JDK tools from the JDK's .jmod files (Shiklo fork): the classes and resources of each module
 * (module-info.class left out), each module's "provides" written as META-INF/services files (a class-path jar has no
 * module descriptor; ServiceLoader reads those files instead), and the classes of an overrides folder in place of the
 * JDK's own. Entries carry one fixed time, so the jar is the same on every build.
 *
 *   java MakeToolJar.java out.jar overridesFolder|- a.jmod [b.jmod ...]
 */
public class MakeToolJar {
    private static final long TIME = 946684800000L; // 2000-01-01, as the runtime's other archives

    public static void main(String[] args) throws IOException {
        Path out = Path.of(args[0]);
        Map<String, byte[]> overrides = new TreeMap<>();
        if (!args[1].equals("-")) {
            Path dir = Path.of(args[1]);
            try (Stream<Path> files = Files.walk(dir)) {
                for (Path f : (Iterable<Path>) files.filter(Files::isRegularFile)::iterator)
                    overrides.put(dir.relativize(f).toString().replace('\\', '/'), Files.readAllBytes(f));
            }
        }
        Set<String> seen = new HashSet<>();
        Map<String, StringBuilder> services = new TreeMap<>();
        int count = 0;
        try (ZipOutputStream zip = new ZipOutputStream(Files.newOutputStream(out))) {
            for (int i = 2; i < args.length; i++) {
                try (InputStream raw = Files.newInputStream(Path.of(args[i]))) {
                    byte[] magic = raw.readNBytes(4);
                    if (magic.length != 4 || magic[0] != 'J' || magic[1] != 'M') throw new IOException("not a jmod: " + args[i]);
                    ZipInputStream jmod = new ZipInputStream(raw);
                    for (ZipEntry e; (e = jmod.getNextEntry()) != null; ) {
                        String name = e.getName();
                        if (name.equals("classes/module-info.class")) {
                            ModuleDescriptor md = ModuleDescriptor.read(new ByteArrayInputStream(jmod.readAllBytes()));
                            for (ModuleDescriptor.Provides p : md.provides())
                                for (String impl : p.providers())
                                    services.computeIfAbsent(p.service(), k -> new StringBuilder()).append(impl).append('\n');
                            continue;
                        }
                        if (!name.startsWith("classes/") || e.isDirectory()) continue;
                        name = name.substring("classes/".length());
                        if (!seen.add(name)) throw new IOException("in two modules: " + name);
                        byte[] content = overrides.containsKey(name) ? overrides.remove(name) : jmod.readAllBytes();
                        put(zip, name, content);
                        count++;
                    }
                }
            }
            for (var o : overrides.entrySet()) {
                put(zip, o.getKey(), o.getValue());
                count++;
            }
            for (var s : services.entrySet()) {
                put(zip, "META-INF/services/" + s.getKey(), s.getValue().toString().getBytes(StandardCharsets.UTF_8));
                count++;
            }
        }
        System.out.println(out.getFileName() + ": " + count + " entries, " + Files.size(out) + " bytes");
    }

    private static void put(ZipOutputStream zip, String name, byte[] content) throws IOException {
        ZipEntry entry = new ZipEntry(name);
        entry.setTime(TIME);
        zip.putNextEntry(entry);
        zip.write(content);
        zip.closeEntry();
    }
}
