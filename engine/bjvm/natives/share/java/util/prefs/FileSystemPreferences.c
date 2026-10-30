// java.util.prefs on the Unix file system (Shiklo fork, 2026-10-30): Preferences.userRoot() threw UnsatisfiedLinkError.
// The preferences files are kept on the run's own file system, which one program uses at a time, so the file locks
// always succeed and permissions are not kept.

#include <arrays.h>
#include <natives-dsl.h>

// int chmod(String fileName, int permission): 0 (success).
DECLARE_NATIVE("java/util/prefs", FileSystemPreferences, chmod, "(Ljava/lang/String;I)I") {
  return (stack_value){.i = 0};
}

// int[] lockFile0(String fileName, int permission, boolean shared): { lock handle, error code }; a handle other than
// 0 means the lock is held.
DECLARE_NATIVE("java/util/prefs", FileSystemPreferences, lockFile0, "(Ljava/lang/String;IZ)[I") {
  obj_header *result = CreatePrimitiveArray1D(thread, TYPE_KIND_INT, 2);
  if (!result)
    return value_null();
  ((s32 *)ArrayData(result))[0] = 1;
  ((s32 *)ArrayData(result))[1] = 0;
  return (stack_value){.obj = result};
}

// int unlockFile0(int lockHandle): 0 (success).
DECLARE_NATIVE("java/util/prefs", FileSystemPreferences, unlockFile0, "(I)I") {
  return (stack_value){.i = 0};
}
