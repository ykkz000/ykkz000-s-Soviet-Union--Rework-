#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "loader_internal.h"

namespace ykkz000::loader {

// 判断 [address, address+bytes) 是否落在已提交且可读的内存区域。
// 任何来自引擎的指针在解引用前都必须先过这一步（0x14000000BE 这类伪指针
// 只有 4 字节可读性都可能成立，因此还必须配合下面的对齐检查）。
bool isReadableRegion(const void* address, std::size_t bytes) {
  if (address == nullptr || bytes == 0) {
    return false;
  }
  MEMORY_BASIC_INFORMATION info = {};
  if (VirtualQuery(address, &info, sizeof(info)) == 0) {
    return false;
  }
  if (info.State != MEM_COMMIT) {
    return false;
  }
  const DWORD protection = info.Protect & 0xFF;
  const bool readable = protection == PAGE_READONLY || protection == PAGE_READWRITE ||
                        protection == PAGE_WRITECOPY || protection == PAGE_EXECUTE_READ ||
                        protection == PAGE_EXECUTE_READWRITE ||
                        protection == PAGE_EXECUTE_WRITECOPY;
  if (!readable) {
    return false;
  }
  const auto start = reinterpret_cast<std::uintptr_t>(address);
  const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
  return start + bytes <= regionEnd;
}

// 指针是否“像一个可解引用的对象”：非低地址、8 字节对齐、首指针可读。
// 8 字节对齐足以滤掉 0x14000000BE 这类由两个 int32 拼出的伪指针。
bool isCandidateObject(const void* pointer) {
  const auto value = reinterpret_cast<std::uintptr_t>(pointer);
  return value > 0x10000 && (value & 0x7) == 0 && isReadableRegion(pointer, sizeof(void*));
}

} // namespace ykkz000::loader
