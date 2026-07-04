#ifndef _GLOBAL_SHIM_H_
#define _GLOBAL_SHIM_H_

#include <stdint.h>
#include <stdbool.h>

// ✅ 每个 typedef 单独守卫，避免与 rwcore.h / rwplcore.h 冲突
#ifndef rwTYPEDEF_RwInt8
#define rwTYPEDEF_RwInt8
typedef int8_t   RwInt8;
#endif

#ifndef rwTYPEDEF_RwInt16
#define rwTYPEDEF_RwInt16
typedef int16_t  RwInt16;
#endif

#ifndef rwTYPEDEF_RwInt32
#define rwTYPEDEF_RwInt32
typedef int32_t  RwInt32;
#endif

#ifndef rwTYPEDEF_RwUInt8
#define rwTYPEDEF_RwUInt8
typedef uint8_t  RwUInt8;
#endif

#ifndef rwTYPEDEF_RwUInt16
#define rwTYPEDEF_RwUInt16
typedef uint16_t RwUInt16;
#endif

#ifndef rwTYPEDEF_RwUInt32
#define rwTYPEDEF_RwUInt32
typedef uint32_t RwUInt32;
#endif

#ifndef rwTYPEDEF_RwReal
#define rwTYPEDEF_RwReal
typedef float    RwReal;
#endif

#ifndef rwTYPEDEF_RwBool
#define rwTYPEDEF_RwBool
typedef int32_t  RwBool;
#endif

#ifndef rwTYPEDEF_RwChar
#define rwTYPEDEF_RwChar
typedef char     RwChar;
#endif

// rw 命名空间别名
namespace rw {
    typedef ::RwInt8   int8;
    typedef ::RwInt16  int16;
    typedef ::RwInt32  int32;
    typedef ::RwUInt8  uint8;
    typedef ::RwUInt16 uint16;
    typedef ::RwUInt32 uint32;
    typedef ::RwReal   float32;
}

#endif // _GLOBAL_SHIM_H_