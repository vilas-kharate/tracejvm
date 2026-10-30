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
