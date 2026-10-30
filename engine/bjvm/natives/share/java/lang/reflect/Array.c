#include "bjvm.h"
#include <arrays.h>
#include <linkage.h>
#include <natives-dsl.h>

DECLARE_NATIVE("java/lang/reflect", Array, newArray, "(Ljava/lang/Class;I)Ljava/lang/Object;") {
  DCHECK(argc == 2);
  if (!args[0].handle->obj)
    return value_null();
  classdesc *class = unmirror_class(args[0].handle->obj);
  initialize_class_t pox = {.args = {thread, class}};
  future_t f = initialize_class(&pox);
  CHECK(f.status == FUTURE_READY);
  s32 count = args[1].i;
  obj_header *result;
  if (class->kind == CD_KIND_PRIMITIVE) {
    result = CreatePrimitiveArray1D(thread, class->primitive_component, count);
  } else {
    result = CreateObjectArray1D(thread, class, count);
  }
  return (stack_value){.obj = result};
}

// Class componentType, int[] dimensions: Array.newInstance with several dimensions, as HotSpot's
// Reflection::reflect_new_multi_array (Shiklo fork, 2026-10-30; it was missing: UnsatisfiedLinkError).
DECLARE_NATIVE("java/lang/reflect", Array, multiNewArray, "(Ljava/lang/Class;[I)Ljava/lang/Object;") {
  if (!args[0].handle->obj || !args[1].handle->obj) {
    raise_null_pointer_exception(thread);
    return value_null();
  }
  int dims = ArrayLength(args[1].handle->obj);
  classdesc *type = unmirror_class(args[0].handle->obj);
  int existing = type->kind == CD_KIND_ORDINARY_ARRAY || type->kind == CD_KIND_PRIMITIVE_ARRAY ? type->dimensions : 0;
  if (dims == 0 || dims + existing > kArrayMaxDimensions ||
      (type->kind == CD_KIND_PRIMITIVE && type->primitive_component == TYPE_KIND_VOID)) {
    raise_vm_exception(thread, STR("java/lang/IllegalArgumentException"),
                       dims == 0 ? STR("Empty dimensions array") : STR("Wrong number of dimensions"));
    return value_null();
  }
  int sizes[kArrayMaxDimensions];
  for (int i = 0; i < dims; ++i) {
    sizes[i] = ((int *)ArrayData(args[1].handle->obj))[i];
    if (sizes[i] < 0) {
      raise_negative_array_size_exception(thread, sizes[i]);
      return value_null();
    }
  }
  if (type->kind != CD_KIND_PRIMITIVE)
    link_class(thread, type);
  for (int i = 0; i < dims; ++i) {
    get_or_create_array_classdesc(thread, type);
    type = type->array_type;
  }
  return (stack_value){.obj = CreateArray(thread, type, sizes, dims)};
}

DECLARE_NATIVE("java/lang/reflect", Array, getLength, "(Ljava/lang/Object;)I") {
  DCHECK(argc == 1);
  return (stack_value){.i = ArrayLength(args[0].handle->obj)};
}

DECLARE_NATIVE("java/lang/reflect", Array, get, "(Ljava/lang/Object;I)Ljava/lang/Object;") {
  DCHECK(argc == 2);

  object array = args[0].handle->obj;
  if (array->descriptor->kind == CD_KIND_ORDINARY) {
    raise_vm_exception(thread, STR("java/lang/IllegalArgumentException"), STR("Argument is not an array"));
    return value_null();
  }

  if (args[1].i < 0 || args[1].i >= ArrayLength(array)) {
    raise_vm_exception(thread, STR("java/lang/ArrayIndexOutOfBoundsException"), STR(""));
    return value_null();
  }
  switch (array->descriptor->kind) {
  case CD_KIND_ORDINARY:
  case CD_KIND_PRIMITIVE:
  default:
    UNREACHABLE();
  case CD_KIND_ORDINARY_ARRAY:
    return (stack_value){.obj = ReferenceArrayLoad(array, args[1].i)};
  case CD_KIND_PRIMITIVE_ARRAY:
    stack_value val;
    cp_method *fromPrimitive;
    switch (array->descriptor->primitive_component) {
    case TYPE_KIND_BOOLEAN:
      val.i = BooleanArrayLoad(array, args[1].i);
      fromPrimitive = method_lookup(cached_classes(thread->vm)->boolean, STR("valueOf"), STR("(Z)Ljava/lang/Boolean;"),
                                    false, false);
      break;
    case TYPE_KIND_CHAR:
      val.i = CharArrayLoad(array, args[1].i);
      fromPrimitive = method_lookup(cached_classes(thread->vm)->character, STR("valueOf"),
                                    STR("(C)Ljava/lang/Character;"), false, false);
      break;
    case TYPE_KIND_FLOAT:
      val.f = FloatArrayLoad(array, args[1].i);
      fromPrimitive =
          method_lookup(cached_classes(thread->vm)->float_, STR("valueOf"), STR("(F)Ljava/lang/Float;"), false, false);
      break;
    case TYPE_KIND_DOUBLE:
      val.d = DoubleArrayLoad(array, args[1].i);
      fromPrimitive = method_lookup(cached_classes(thread->vm)->double_, STR("valueOf"), STR("(D)Ljava/lang/Double;"),
                                    false, false);
      break;
    case TYPE_KIND_BYTE:
      val.i = (jint)ByteArrayLoad(array, args[1].i);
      fromPrimitive =
          method_lookup(cached_classes(thread->vm)->byte, STR("valueOf"), STR("(B)Ljava/lang/Byte;"), false, false);
      break;
    case TYPE_KIND_SHORT:
      val.i = ShortArrayLoad(array, args[1].i);
      fromPrimitive =
          method_lookup(cached_classes(thread->vm)->short_, STR("valueOf"), STR("(S)Ljava/lang/Short;"), false, false);
      break;
    case TYPE_KIND_INT:
      val.i = IntArrayLoad(array, args[1].i);
      fromPrimitive = method_lookup(cached_classes(thread->vm)->integer, STR("valueOf"), STR("(I)Ljava/lang/Integer;"),
                                    false, false);
      break;
    case TYPE_KIND_LONG:
      val.l = LongArrayLoad(array, args[1].i);
      fromPrimitive =
          method_lookup(cached_classes(thread->vm)->long_, STR("valueOf"), STR("(J)Ljava/lang/Long;"), false, false);
      break;
    case TYPE_KIND_VOID:
    case TYPE_KIND_REFERENCE:
    default:
      UNREACHABLE();
    }

    DCHECK(fromPrimitive);

    // Now call fromPrimitive and return the result
    return call_interpreter_synchronous(thread, fromPrimitive, (stack_value[]){val});
  }
}

static bool primitive_wrapper_can_widen(classdesc *source, type_kind target, vm *vm) {
  struct cached_classdescs *cached = cached_classes(vm);
  if (target == TYPE_KIND_BOOLEAN)
    return source == cached->boolean;
  if (target == TYPE_KIND_CHAR)
    return source == cached->character;
  if (source == cached->byte)
    return target == TYPE_KIND_BYTE || target == TYPE_KIND_SHORT ||
           target == TYPE_KIND_INT || target == TYPE_KIND_LONG ||
           target == TYPE_KIND_FLOAT || target == TYPE_KIND_DOUBLE;
  if (source == cached->short_)
    return target == TYPE_KIND_SHORT || target == TYPE_KIND_INT ||
           target == TYPE_KIND_LONG || target == TYPE_KIND_FLOAT ||
           target == TYPE_KIND_DOUBLE;
  if (source == cached->character)
    return target == TYPE_KIND_INT || target == TYPE_KIND_LONG ||
           target == TYPE_KIND_FLOAT || target == TYPE_KIND_DOUBLE;
  if (source == cached->integer)
    return target == TYPE_KIND_INT || target == TYPE_KIND_LONG ||
           target == TYPE_KIND_FLOAT || target == TYPE_KIND_DOUBLE;
  if (source == cached->long_)
    return target == TYPE_KIND_LONG || target == TYPE_KIND_FLOAT ||
           target == TYPE_KIND_DOUBLE;
  if (source == cached->float_)
    return target == TYPE_KIND_FLOAT || target == TYPE_KIND_DOUBLE;
  return source == cached->double_ && target == TYPE_KIND_DOUBLE;
}

static stack_value unbox_for_array_store(vm_thread *thread, object boxed, type_kind target) {
  slice method_name;
  slice descriptor;
  if (target == TYPE_KIND_BOOLEAN) {
    method_name = STR("booleanValue");
    descriptor = STR("()Z");
  } else if (target == TYPE_KIND_CHAR || boxed->descriptor == cached_classes(thread->vm)->character) {
    method_name = STR("charValue");
    descriptor = STR("()C");
  } else if (target == TYPE_KIND_LONG) {
    method_name = STR("longValue");
    descriptor = STR("()J");
  } else if (target == TYPE_KIND_FLOAT) {
    method_name = STR("floatValue");
    descriptor = STR("()F");
  } else if (target == TYPE_KIND_DOUBLE) {
    method_name = STR("doubleValue");
    descriptor = STR("()D");
  } else if (target == TYPE_KIND_BYTE) {
    method_name = STR("byteValue");
    descriptor = STR("()B");
  } else if (target == TYPE_KIND_SHORT) {
    method_name = STR("shortValue");
    descriptor = STR("()S");
  } else {
    method_name = STR("intValue");
    descriptor = STR("()I");
  }
  cp_method *method = method_lookup(boxed->descriptor, method_name, descriptor, true, false);
  CHECK(method);
  return call_interpreter_synchronous(
      thread, method, (stack_value[]){{.obj = boxed}});
}

DECLARE_NATIVE("java/lang/reflect", Array, set, "(Ljava/lang/Object;ILjava/lang/Object;)V") {
  DCHECK(argc == 3);
  object array = args[0].handle->obj;
  if (!array) {
    raise_vm_exception(thread, STR("java/lang/NullPointerException"), STR(""));
    return value_null();
  }
  if (array->descriptor->kind == CD_KIND_ORDINARY ||
      array->descriptor->kind == CD_KIND_PRIMITIVE) {
    raise_vm_exception(thread, STR("java/lang/IllegalArgumentException"), STR("Argument is not an array"));
    return value_null();
  }
  s32 index = args[1].i;
  if (index < 0 || index >= ArrayLength(array)) {
    raise_vm_exception(thread, STR("java/lang/ArrayIndexOutOfBoundsException"), STR(""));
    return value_null();
  }

  object value = args[2].handle->obj;
  if (array->descriptor->kind == CD_KIND_ORDINARY_ARRAY) {
    if (value && !instanceof(value->descriptor, array->descriptor->one_fewer_dim)) {
      raise_vm_exception(thread, STR("java/lang/IllegalArgumentException"), STR("array element type mismatch"));
      return value_null();
    }
    ReferenceArrayStore(array, index, value);
    return value_null();
  }

  type_kind target = array->descriptor->primitive_component;
  if (!value || !primitive_wrapper_can_widen(value->descriptor, target, thread->vm)) {
    raise_vm_exception(thread, STR("java/lang/IllegalArgumentException"), STR("argument type mismatch"));
    return value_null();
  }
  stack_value unboxed = unbox_for_array_store(thread, value, target);
  if (thread->current_exception)
    return value_null();

  switch (target) {
  case TYPE_KIND_BOOLEAN:
    BooleanArrayStore(array, index, unboxed.i);
    break;
  case TYPE_KIND_CHAR:
    CharArrayStore(array, index, unboxed.i);
    break;
  case TYPE_KIND_BYTE:
    ByteArrayStore(array, index, unboxed.i);
    break;
  case TYPE_KIND_SHORT:
    ShortArrayStore(array, index, unboxed.i);
    break;
  case TYPE_KIND_INT:
    IntArrayStore(array, index, unboxed.i);
    break;
  case TYPE_KIND_LONG:
    if (value->descriptor == cached_classes(thread->vm)->character)
      LongArrayStore(array, index, unboxed.i);
    else
      LongArrayStore(array, index, unboxed.l);
    break;
  case TYPE_KIND_FLOAT:
    if (value->descriptor == cached_classes(thread->vm)->character)
      FloatArrayStore(array, index, unboxed.i);
    else
      FloatArrayStore(array, index, unboxed.f);
    break;
  case TYPE_KIND_DOUBLE:
    if (value->descriptor == cached_classes(thread->vm)->character)
      DoubleArrayStore(array, index, unboxed.i);
    else
      DoubleArrayStore(array, index, unboxed.d);
    break;
  case TYPE_KIND_VOID:
  case TYPE_KIND_REFERENCE:
  default:
    UNREACHABLE();
  }
  return value_null();
}

// ---- The primitive Array.getX / setX (Shiklo fork, 2026-10-30; all were missing: Array.setInt threw
// UnsatisfiedLinkError). As HotSpot's Reflection::array_get / array_set: a primitive array, the element widened to the
// asked type (JLS 5.1.2) on get, the value widened to the element type on set; otherwise IllegalArgumentException.

// A primitive value: integral kinds (and boolean) in l, float and double in d.
typedef struct {
  s64 l;
  double d;
} array_primitive;

static bool primitive_widens(type_kind from, type_kind to) {
  if (from == to)
    return true;
  switch (from) {
  case TYPE_KIND_BYTE:
    return to == TYPE_KIND_SHORT || to == TYPE_KIND_INT || to == TYPE_KIND_LONG || to == TYPE_KIND_FLOAT ||
           to == TYPE_KIND_DOUBLE;
  case TYPE_KIND_SHORT:
  case TYPE_KIND_CHAR:
    return to == TYPE_KIND_INT || to == TYPE_KIND_LONG || to == TYPE_KIND_FLOAT || to == TYPE_KIND_DOUBLE;
  case TYPE_KIND_INT:
    return to == TYPE_KIND_LONG || to == TYPE_KIND_FLOAT || to == TYPE_KIND_DOUBLE;
  case TYPE_KIND_LONG:
    return to == TYPE_KIND_FLOAT || to == TYPE_KIND_DOUBLE;
  case TYPE_KIND_FLOAT:
    return to == TYPE_KIND_DOUBLE;
  default:
    return false;
  }
}

static bool is_floating(type_kind kind) { return kind == TYPE_KIND_FLOAT || kind == TYPE_KIND_DOUBLE; }

// The primitive array args[0] and index args[1], checked; null (with the exception raised) when they are not usable.
static obj_header *primitive_array_at(vm_thread *thread, value *args) {
  obj_header *array = args[0].handle->obj;
  if (!array) {
    raise_null_pointer_exception(thread);
    return nullptr;
  }
  if (array->descriptor->kind == CD_KIND_ORDINARY || array->descriptor->kind == CD_KIND_PRIMITIVE) {
    raise_vm_exception(thread, STR("java/lang/IllegalArgumentException"), STR("Argument is not an array"));
    return nullptr;
  }
  if (array->descriptor->kind != CD_KIND_PRIMITIVE_ARRAY) {
    raise_vm_exception(thread, STR("java/lang/IllegalArgumentException"), STR("argument type mismatch"));
    return nullptr;
  }
  if (args[1].i < 0 || args[1].i >= ArrayLength(array)) {
    raise_array_index_oob_exception(thread, args[1].i, ArrayLength(array));
    return nullptr;
  }
  return array;
}

// Array.getX: the element as kind `as`; false with the exception raised.
static bool array_get_primitive(vm_thread *thread, value *args, type_kind as, array_primitive *out) {
  obj_header *array = primitive_array_at(thread, args);
  if (!array)
    return false;
  type_kind kind = array->descriptor->primitive_component;
  if (!primitive_widens(kind, as)) {
    raise_vm_exception(thread, STR("java/lang/IllegalArgumentException"), STR("argument type mismatch"));
    return false;
  }
  s32 i = args[1].i;
  array_primitive v = {0, 0};
  switch (kind) {
  case TYPE_KIND_BOOLEAN: v.l = BooleanArrayLoad(array, i); break;
  case TYPE_KIND_BYTE: v.l = ByteArrayLoad(array, i); break;
  case TYPE_KIND_SHORT: v.l = ShortArrayLoad(array, i); break;
  case TYPE_KIND_CHAR: v.l = CharArrayLoad(array, i); break;
  case TYPE_KIND_INT: v.l = IntArrayLoad(array, i); break;
  case TYPE_KIND_LONG: v.l = LongArrayLoad(array, i); break;
  case TYPE_KIND_FLOAT: v.d = FloatArrayLoad(array, i); break;
  case TYPE_KIND_DOUBLE: v.d = DoubleArrayLoad(array, i); break;
  default: UNREACHABLE();
  }
  if (is_floating(as) && !is_floating(kind))
    v.d = (double)v.l;
  *out = v;
  return true;
}

// Array.setX: v, of kind `from`, stored in args[0][args[1]] widened to the element's kind.
static void array_set_primitive(vm_thread *thread, value *args, type_kind from, array_primitive v) {
  obj_header *array = primitive_array_at(thread, args);
  if (!array)
    return;
  type_kind kind = array->descriptor->primitive_component;
  if (!primitive_widens(from, kind)) {
    raise_vm_exception(thread, STR("java/lang/IllegalArgumentException"), STR("argument type mismatch"));
    return;
  }
  if (is_floating(kind) && !is_floating(from))
    v.d = (double)v.l;
  s32 i = args[1].i;
  switch (kind) {
  case TYPE_KIND_BOOLEAN: BooleanArrayStore(array, i, v.l != 0); break;
  case TYPE_KIND_BYTE: ByteArrayStore(array, i, (s8)v.l); break;
  case TYPE_KIND_SHORT: ShortArrayStore(array, i, (s16)v.l); break;
  case TYPE_KIND_CHAR: CharArrayStore(array, i, (u16)v.l); break;
  case TYPE_KIND_INT: IntArrayStore(array, i, (s32)v.l); break;
  case TYPE_KIND_LONG: LongArrayStore(array, i, v.l); break;
  case TYPE_KIND_FLOAT: FloatArrayStore(array, i, (float)v.d); break;
  case TYPE_KIND_DOUBLE: DoubleArrayStore(array, i, v.d); break;
  default: UNREACHABLE();
  }
}

// Written out one by one: codegen/scan_native_declarations.py finds natives in the source text, not in macros.
DECLARE_NATIVE("java/lang/reflect", Array, getBoolean, "(Ljava/lang/Object;I)Z") {
  array_primitive v;
  return array_get_primitive(thread, args, TYPE_KIND_BOOLEAN, &v) ? (stack_value){.i = v.l != 0} : value_null();
}
DECLARE_NATIVE("java/lang/reflect", Array, getByte, "(Ljava/lang/Object;I)B") {
  array_primitive v;
  return array_get_primitive(thread, args, TYPE_KIND_BYTE, &v) ? (stack_value){.i = (s8)v.l} : value_null();
}
DECLARE_NATIVE("java/lang/reflect", Array, getChar, "(Ljava/lang/Object;I)C") {
  array_primitive v;
  return array_get_primitive(thread, args, TYPE_KIND_CHAR, &v) ? (stack_value){.i = (u16)v.l} : value_null();
}
DECLARE_NATIVE("java/lang/reflect", Array, getShort, "(Ljava/lang/Object;I)S") {
  array_primitive v;
  return array_get_primitive(thread, args, TYPE_KIND_SHORT, &v) ? (stack_value){.i = (s16)v.l} : value_null();
}
DECLARE_NATIVE("java/lang/reflect", Array, getInt, "(Ljava/lang/Object;I)I") {
  array_primitive v;
  return array_get_primitive(thread, args, TYPE_KIND_INT, &v) ? (stack_value){.i = (s32)v.l} : value_null();
}
DECLARE_NATIVE("java/lang/reflect", Array, getLong, "(Ljava/lang/Object;I)J") {
  array_primitive v;
  return array_get_primitive(thread, args, TYPE_KIND_LONG, &v) ? (stack_value){.l = v.l} : value_null();
}
DECLARE_NATIVE("java/lang/reflect", Array, getFloat, "(Ljava/lang/Object;I)F") {
  array_primitive v;
  return array_get_primitive(thread, args, TYPE_KIND_FLOAT, &v) ? (stack_value){.f = (float)v.d} : value_null();
}
DECLARE_NATIVE("java/lang/reflect", Array, getDouble, "(Ljava/lang/Object;I)D") {
  array_primitive v;
  return array_get_primitive(thread, args, TYPE_KIND_DOUBLE, &v) ? (stack_value){.d = v.d} : value_null();
}
DECLARE_NATIVE("java/lang/reflect", Array, setBoolean, "(Ljava/lang/Object;IZ)V") {
  array_set_primitive(thread, args, TYPE_KIND_BOOLEAN, (array_primitive){.l = args[2].i != 0});
  return value_null();
}
DECLARE_NATIVE("java/lang/reflect", Array, setByte, "(Ljava/lang/Object;IB)V") {
  array_set_primitive(thread, args, TYPE_KIND_BYTE, (array_primitive){.l = (s8)args[2].i});
  return value_null();
}
DECLARE_NATIVE("java/lang/reflect", Array, setChar, "(Ljava/lang/Object;IC)V") {
  array_set_primitive(thread, args, TYPE_KIND_CHAR, (array_primitive){.l = (u16)args[2].i});
  return value_null();
}
DECLARE_NATIVE("java/lang/reflect", Array, setShort, "(Ljava/lang/Object;IS)V") {
  array_set_primitive(thread, args, TYPE_KIND_SHORT, (array_primitive){.l = (s16)args[2].i});
  return value_null();
}
DECLARE_NATIVE("java/lang/reflect", Array, setInt, "(Ljava/lang/Object;II)V") {
  array_set_primitive(thread, args, TYPE_KIND_INT, (array_primitive){.l = args[2].i});
  return value_null();
}
DECLARE_NATIVE("java/lang/reflect", Array, setLong, "(Ljava/lang/Object;IJ)V") {
  array_set_primitive(thread, args, TYPE_KIND_LONG, (array_primitive){.l = args[2].l});
  return value_null();
}
DECLARE_NATIVE("java/lang/reflect", Array, setFloat, "(Ljava/lang/Object;IF)V") {
  array_set_primitive(thread, args, TYPE_KIND_FLOAT, (array_primitive){.d = args[2].f});
  return value_null();
}
DECLARE_NATIVE("java/lang/reflect", Array, setDouble, "(Ljava/lang/Object;ID)V") {
  array_set_primitive(thread, args, TYPE_KIND_DOUBLE, (array_primitive){.d = args[2].d});
  return value_null();
}
