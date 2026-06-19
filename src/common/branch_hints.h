#pragma once

#if defined(__GNUC__) || defined(__clang__)
    #define HFT_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define HFT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#elif defined(_MSC_VER)
    #define HFT_LIKELY(x)   (x)
    #define HFT_UNLIKELY(x) (x)
#else
    #define HFT_LIKELY(x)   (x)
    #define HFT_UNLIKELY(x) (x)
#endif
