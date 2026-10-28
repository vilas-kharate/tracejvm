#include <natives-dsl.h>
#include <zlib.h>

// java.util.zip on zlib: Inflater, CRC32 and Adler32 here, Deflater in Deflater.c. Written from the contract of the
// JDK 23 classes that call these natives (Shiklo fork, 2026-10-28): Inflater had only init / inflateBytesBytes / reset
// / end, reported a corrupt stream as finished instead of throwing DataFormatException, and lacked the direct-buffer
// variants, dictionaries and getAdler; CRC32 lacked update and updateByteBuffer0; Adler32 had nothing.

DECLARE_NATIVE("java/util/zip", Inflater, initIDs, "()V") { return value_null(); }

#define ZS thread->vm->z_streams

DECLARE_NATIVE("java/util/zip", Inflater, init, "(Z)J") {
  bool nowrap = args[0].i;
  z_stream *stream = calloc(1, sizeof(z_stream));
  if (!stream) {
    raise_vm_exception_no_msg(thread, STR("java/lang/OutOfMemoryError"));
    return (stack_value){.l = 0};
  }
  int ret = inflateInit2(stream, nowrap ? -MAX_WBITS : MAX_WBITS);
  if (ret != Z_OK) {
    free(stream);
    if (ret == Z_MEM_ERROR)
      raise_vm_exception_no_msg(thread, STR("java/lang/OutOfMemoryError"));
    else
      raise_vm_exception(thread, STR("java/lang/InternalError"), STR("inflateInit2 failed"));
    return (stack_value){.l = 0};
  }
  arrput(ZS, stream);
  return (stack_value){.l = (s64)stream};
}

/**
 * The JDK decodes the result as:
 *   int read = (int) (result & 0x7fff_ffffL);
 *   int written = (int) (result >>> 31 & 0x7fff_ffffL);
 *   finished = (result >>> 62 & 1) != 0;   needDict = (result >>> 63 & 1) != 0;
 */
static s64 make_return_value(int read, int written, bool finished, bool needDict) {
  return (s64)read | (s64)written << 31 | (s64)finished << 62 | (s64)needDict << 63;
}

static void raise_with_zlib_message(vm_thread *thread, slice exception, z_stream *stream) {
  if (stream->msg)
    raise_vm_exception(thread, exception, str_to_utf8(stream->msg));
  else
    raise_vm_exception_no_msg(thread, exception);
}

// One inflate step over src (src_len bytes) into dst (dst_len bytes), as the JDK's Inflater expects: on a format error
// the bytes consumed so far go to the Inflater's inputConsumed / outputConsumed fields and DataFormatException is
// thrown; when zlib can make no progress (Z_BUF_ERROR) nothing was read or written.
static s64 inflate_step(vm_thread *thread, obj_header *inflater, z_stream *stream, u8 *src, u32 src_len, u8 *dst,
                        u32 dst_len) {
  stream->next_in = src;
  stream->avail_in = src_len;
  stream->next_out = dst;
  stream->avail_out = dst_len;
  int ret = inflate(stream, Z_PARTIAL_FLUSH);
  int read = (int)(src_len - stream->avail_in);
  int written = (int)(dst_len - stream->avail_out);
  switch (ret) {
  case Z_STREAM_END:
    return make_return_value(read, written, true, false);
  case Z_OK:
    return make_return_value(read, written, false, false);
  case Z_NEED_DICT:
    return make_return_value(read, written, false, true);
  case Z_BUF_ERROR:
    return make_return_value(0, 0, false, false);
  case Z_DATA_ERROR:
    StoreFieldInt(inflater, "inputConsumed", read);
    StoreFieldInt(inflater, "outputConsumed", written);
    raise_with_zlib_message(thread, STR("java/util/zip/DataFormatException"), stream);
    return 0;
  case Z_MEM_ERROR:
    raise_vm_exception_no_msg(thread, STR("java/lang/OutOfMemoryError"));
    return 0;
  default:
    raise_with_zlib_message(thread, STR("java/lang/InternalError"), stream);
    return 0;
  }
}

// long addr, byte[] inputArray, int inputOff, int inputLen, byte[] outputArray, int outputOff, int outputLen
DECLARE_NATIVE("java/util/zip", Inflater, inflateBytesBytes, "(J[BII[BII)J") {
  u8 *src = (u8 *)ArrayData(args[1].handle->obj) + args[2].i;
  u8 *dst = (u8 *)ArrayData(args[4].handle->obj) + args[5].i;
  return (stack_value){.l = inflate_step(thread, obj->obj, (z_stream *)args[0].l, src, args[3].i, dst, args[6].i)};
}

// long addr, byte[] inputArray, int inputOff, int inputLen, long outputAddress, int outputLen
DECLARE_NATIVE("java/util/zip", Inflater, inflateBytesBuffer, "(J[BIIJI)J") {
  u8 *src = (u8 *)ArrayData(args[1].handle->obj) + args[2].i;
  u8 *dst = (u8 *)(uintptr_t)args[4].l;
  return (stack_value){.l = inflate_step(thread, obj->obj, (z_stream *)args[0].l, src, args[3].i, dst, args[5].i)};
}

// long addr, long inputAddress, int inputLen, byte[] outputArray, int outputOff, int outputLen
DECLARE_NATIVE("java/util/zip", Inflater, inflateBufferBytes, "(JJI[BII)J") {
  u8 *src = (u8 *)(uintptr_t)args[1].l;
  u8 *dst = (u8 *)ArrayData(args[3].handle->obj) + args[4].i;
  return (stack_value){.l = inflate_step(thread, obj->obj, (z_stream *)args[0].l, src, args[2].i, dst, args[5].i)};
}

// long addr, long inputAddress, int inputLen, long outputAddress, int outputLen
DECLARE_NATIVE("java/util/zip", Inflater, inflateBufferBuffer, "(JJIJI)J") {
  u8 *src = (u8 *)(uintptr_t)args[1].l;
  u8 *dst = (u8 *)(uintptr_t)args[3].l;
  return (stack_value){.l = inflate_step(thread, obj->obj, (z_stream *)args[0].l, src, args[2].i, dst, args[4].i)};
}

static void inflater_set_dictionary(vm_thread *thread, z_stream *stream, u8 *dictionary, u32 length) {
  int ret = inflateSetDictionary(stream, dictionary, length);
  if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR)
    raise_with_zlib_message(thread, STR("java/lang/IllegalArgumentException"), stream);
  else if (ret != Z_OK)
    raise_with_zlib_message(thread, STR("java/lang/InternalError"), stream);
}

// long addr, byte[] b, int off, int len
DECLARE_NATIVE("java/util/zip", Inflater, setDictionary, "(J[BII)V") {
  inflater_set_dictionary(thread, (z_stream *)args[0].l, (u8 *)ArrayData(args[1].handle->obj) + args[2].i, args[3].i);
  return value_null();
}

// long addr, long bufAddress, int len
DECLARE_NATIVE("java/util/zip", Inflater, setDictionaryBuffer, "(JJI)V") {
  inflater_set_dictionary(thread, (z_stream *)args[0].l, (u8 *)(uintptr_t)args[1].l, args[2].i);
  return value_null();
}

DECLARE_NATIVE("java/util/zip", Inflater, getAdler, "(J)I") {
  return (stack_value){.i = (s32)((z_stream *)args[0].l)->adler};
}

DECLARE_NATIVE("java/util/zip", Inflater, reset, "(J)V") {
  inflateReset((z_stream *)args[0].l);
  return value_null();
}

DECLARE_NATIVE("java/util/zip", Inflater, end, "(J)V") {
  z_stream *stream = (z_stream *)args[0].l;
  inflateEnd(stream);
  for (int i = 0; i < arrlen(ZS); ++i) {
    if (ZS[i] == stream) {
      arrdelswap(ZS, i);
      break;
    }
  }
  free(stream);
  return value_null();
}
#undef ZS

// int crc, int b
DECLARE_NATIVE("java/util/zip", CRC32, update, "(II)I") {
  u8 byte = (u8)args[1].i;
  return (stack_value){.i = (s32)crc32((u32)args[0].i, &byte, 1)};
}

// int crc, byte[] b, int off, int len
DECLARE_NATIVE("java/util/zip", CRC32, updateBytes0, "(I[BII)I") {
  u8 *data = (u8 *)ArrayData(args[1].handle->obj) + args[2].i;
  return (stack_value){.i = (s32)crc32((u32)args[0].i, data, args[3].i)};
}

// int crc, long addr, int off, int len
DECLARE_NATIVE("java/util/zip", CRC32, updateByteBuffer0, "(IJII)I") {
  u8 *data = (u8 *)(uintptr_t)args[1].l + args[2].i;
  return (stack_value){.i = (s32)crc32((u32)args[0].i, data, args[3].i)};
}

// int adler, int b
DECLARE_NATIVE("java/util/zip", Adler32, update, "(II)I") {
  u8 byte = (u8)args[1].i;
  return (stack_value){.i = (s32)adler32((u32)args[0].i, &byte, 1)};
}

// int adler, byte[] b, int off, int len
DECLARE_NATIVE("java/util/zip", Adler32, updateBytes, "(I[BII)I") {
  u8 *data = (u8 *)ArrayData(args[1].handle->obj) + args[2].i;
  return (stack_value){.i = (s32)adler32((u32)args[0].i, data, args[3].i)};
}

// int adler, long addr, int off, int len
DECLARE_NATIVE("java/util/zip", Adler32, updateByteBuffer, "(IJII)I") {
  u8 *data = (u8 *)(uintptr_t)args[1].l + args[2].i;
  return (stack_value){.i = (s32)adler32((u32)args[0].i, data, args[3].i)};
}
