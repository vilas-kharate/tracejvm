#include <natives-dsl.h>
#include <zlib.h>

// java.util.zip.Deflater on zlib, written from the contract of the JDK 23 class (Shiklo fork, 2026-10-28). It was
// missing, so ZipOutputStream, GZIPOutputStream, JarOutputStream (the jar tool) and Deflater itself failed with
// UnsatisfiedLinkError. The JDK's flush modes (NO_FLUSH 0, SYNC_FLUSH 2, FULL_FLUSH 3, FINISH 4) and strategies
// (DEFAULT 0, FILTERED 1, HUFFMAN_ONLY 2) have zlib's values.

#define DS thread->vm->z_deflate_streams

// int level, int strategy, boolean nowrap
DECLARE_NATIVE("java/util/zip", Deflater, init, "(IIZ)J") {
  z_stream *stream = calloc(1, sizeof(z_stream));
  if (!stream) {
    raise_vm_exception_no_msg(thread, STR("java/lang/OutOfMemoryError"));
    return (stack_value){.l = 0};
  }
  int ret = deflateInit2(stream, args[0].i, Z_DEFLATED, args[2].i ? -MAX_WBITS : MAX_WBITS, 8, args[1].i);
  if (ret != Z_OK) {
    free(stream);
    if (ret == Z_MEM_ERROR)
      raise_vm_exception_no_msg(thread, STR("java/lang/OutOfMemoryError"));
    else
      raise_vm_exception_no_msg(thread, STR("java/lang/IllegalArgumentException"));
    return (stack_value){.l = 0};
  }
  arrput(DS, stream);
  return (stack_value){.l = (s64)stream};
}

static void raise_with_zlib_message(vm_thread *thread, slice exception, z_stream *stream) {
  if (stream->msg)
    raise_vm_exception(thread, exception, str_to_utf8(stream->msg));
  else
    raise_vm_exception_no_msg(thread, exception);
}

/**
 * One deflate step, as the JDK's Deflater expects. params: bit 0 set = apply new parameters (bits 1-2 strategy, bits
 * 3-31 level, -1 for the default), and then only deflateParams runs. The JDK decodes the result as:
 *   int read = (int) (result & 0x7fff_ffffL);   int written = (int) (result >>> 31 & 0x7fff_ffffL);
 *   finished = (result >>> 62 & 1) != 0;       parameters still to set = (result >>> 63 & 1) != 0
 */
static s64 deflate_step(vm_thread *thread, z_stream *stream, u8 *src, u32 src_len, u8 *dst, u32 dst_len, int flush,
                        int params) {
  stream->next_in = src;
  stream->avail_in = src_len;
  stream->next_out = dst;
  stream->avail_out = dst_len;
  bool finished = false, params_pending = false;
  if (params & 1) {
    int ret = deflateParams(stream, params >> 3, (params >> 1) & 3);
    if (ret == Z_BUF_ERROR) {
      params_pending = true; // not enough room to flush what the old parameters compressed: called again
    } else if (ret != Z_OK) {
      raise_with_zlib_message(thread, STR("java/lang/InternalError"), stream);
      return 0;
    }
  } else {
    int ret = deflate(stream, flush);
    if (ret == Z_STREAM_END) {
      finished = true;
    } else if (ret != Z_OK && ret != Z_BUF_ERROR) { // Z_BUF_ERROR: no progress was possible
      raise_with_zlib_message(thread, STR("java/lang/InternalError"), stream);
      return 0;
    }
  }
  s64 read = (s64)(src_len - stream->avail_in);
  s64 written = (s64)(dst_len - stream->avail_out);
  return read | written << 31 | (s64)finished << 62 | (s64)params_pending << 63;
}

// long addr, byte[] inputArray, int inputOff, int inputLen, byte[] outputArray, int outputOff, int outputLen,
// int flush, int params
DECLARE_NATIVE("java/util/zip", Deflater, deflateBytesBytes, "(J[BII[BIIII)J") {
  u8 *src = (u8 *)ArrayData(args[1].handle->obj) + args[2].i;
  u8 *dst = (u8 *)ArrayData(args[4].handle->obj) + args[5].i;
  return (stack_value){
      .l = deflate_step(thread, (z_stream *)args[0].l, src, args[3].i, dst, args[6].i, args[7].i, args[8].i)};
}

// long addr, byte[] inputArray, int inputOff, int inputLen, long outputAddress, int outputLen, int flush, int params
DECLARE_NATIVE("java/util/zip", Deflater, deflateBytesBuffer, "(J[BIIJIII)J") {
  u8 *src = (u8 *)ArrayData(args[1].handle->obj) + args[2].i;
  u8 *dst = (u8 *)(uintptr_t)args[4].l;
  return (stack_value){
      .l = deflate_step(thread, (z_stream *)args[0].l, src, args[3].i, dst, args[5].i, args[6].i, args[7].i)};
}

// long addr, long inputAddress, int inputLen, byte[] outputArray, int outputOff, int outputLen, int flush, int params
DECLARE_NATIVE("java/util/zip", Deflater, deflateBufferBytes, "(JJI[BIIII)J") {
  u8 *src = (u8 *)(uintptr_t)args[1].l;
  u8 *dst = (u8 *)ArrayData(args[3].handle->obj) + args[4].i;
  return (stack_value){
      .l = deflate_step(thread, (z_stream *)args[0].l, src, args[2].i, dst, args[5].i, args[6].i, args[7].i)};
}

// long addr, long inputAddress, int inputLen, long outputAddress, int outputLen, int flush, int params
DECLARE_NATIVE("java/util/zip", Deflater, deflateBufferBuffer, "(JJIJIII)J") {
  u8 *src = (u8 *)(uintptr_t)args[1].l;
  u8 *dst = (u8 *)(uintptr_t)args[3].l;
  return (stack_value){
      .l = deflate_step(thread, (z_stream *)args[0].l, src, args[2].i, dst, args[4].i, args[5].i, args[6].i)};
}

static void deflater_set_dictionary(vm_thread *thread, z_stream *stream, u8 *dictionary, u32 length) {
  int ret = deflateSetDictionary(stream, dictionary, length);
  if (ret == Z_STREAM_ERROR)
    raise_with_zlib_message(thread, STR("java/lang/IllegalArgumentException"), stream);
  else if (ret != Z_OK)
    raise_with_zlib_message(thread, STR("java/lang/InternalError"), stream);
}

// long addr, byte[] b, int off, int len
DECLARE_NATIVE("java/util/zip", Deflater, setDictionary, "(J[BII)V") {
  deflater_set_dictionary(thread, (z_stream *)args[0].l, (u8 *)ArrayData(args[1].handle->obj) + args[2].i, args[3].i);
  return value_null();
}

// long addr, long bufAddress, int len
DECLARE_NATIVE("java/util/zip", Deflater, setDictionaryBuffer, "(JJI)V") {
  deflater_set_dictionary(thread, (z_stream *)args[0].l, (u8 *)(uintptr_t)args[1].l, args[2].i);
  return value_null();
}

DECLARE_NATIVE("java/util/zip", Deflater, getAdler, "(J)I") {
  return (stack_value){.i = (s32)((z_stream *)args[0].l)->adler};
}

DECLARE_NATIVE("java/util/zip", Deflater, reset, "(J)V") {
  deflateReset((z_stream *)args[0].l);
  return value_null();
}

DECLARE_NATIVE("java/util/zip", Deflater, end, "(J)V") {
  z_stream *stream = (z_stream *)args[0].l;
  deflateEnd(stream);
  for (int i = 0; i < arrlen(DS); ++i) {
    if (DS[i] == stream) {
      arrdelswap(DS, i);
      break;
    }
  }
  free(stream);
  return value_null();
}
#undef DS
