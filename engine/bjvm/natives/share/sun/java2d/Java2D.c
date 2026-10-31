// Java 2D on a BufferedImage without libawt's native loops (Shiklo fork, 2026-10-31): Graphics2D.fillRect, drawLine …
// threw UnsatisfiedLinkError. GraphicsPrimitiveMgr registers its pure-Java loops (GeneralRenderer, CustomComponent)
// besides the native ones, and those draw through the image's Raster in Java; here no native loop is registered, and a
// BufferedImage's surface keeps no native description of its pixels.

#include <natives-dsl.h>

// void GraphicsPrimitiveMgr.registerNativeLoops(): none, the Java loops are used.
DECLARE_NATIVE("sun/java2d/loops", GraphicsPrimitiveMgr, registerNativeLoops, "()V") {
  return value_null();
}

// void BufImgSurfaceData.initRaster(Object theArray, int offset, int bitoffset, int width, int height, int pixStr,
// int scanStr, IndexColorModel icm): the native loops' view of the pixels, not needed by the Java loops.
DECLARE_NATIVE("sun/java2d", BufImgSurfaceData, initRaster,
               "(Ljava/lang/Object;IIIIIILjava/awt/image/IndexColorModel;)V") {
  return value_null();
}
