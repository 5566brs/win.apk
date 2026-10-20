// native_icu.cpp -- com.macapk.icu.HostIcu on Windows: number-skeleton
// formatting (units and the like, which the JDK has no CLDR data for) through
// the ICU that Windows ships as icu.dll (Windows 10 1903 and later), as
// upstream's icu_host_bridge.cpp uses macOS's libicucore. Same engine and data
// family as Android's android.icu. icu.dll exports the C API without version
// suffixes, so the functions are looked up by their plain names.
//
// Errors follow HostIcu's contract: an ICU failure throws
// IllegalArgumentException(u_errorName), and no icu.dll throws
// UnsupportedOperationException, never a made-up string.

#include "host_natives.h"

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>

namespace winapk::host {

namespace {

using UErrorCode = int;
struct Icu {
    void* (*open)(const char16_t*, int32_t, const char*, UErrorCode*);
    void* (*open_result)(UErrorCode*);
    void (*format_double)(const void*, double, void*, UErrorCode*);
    void (*format_decimal)(const void*, const char*, int32_t, void*, UErrorCode*);
    int32_t (*to_string)(const void*, char16_t*, int32_t, UErrorCode*);
    void (*close)(void*);
    void (*close_result)(void*);
    const char* (*error_name)(UErrorCode);
};

const Icu* icu() {
    static const Icu* api = []() -> const Icu* {
        HMODULE m = LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) return nullptr;
        static Icu a;
        auto get = [m](const char* name) { return GetProcAddress(m, name); };
        a.open = reinterpret_cast<decltype(a.open)>(get("unumf_openForSkeletonAndLocale"));
        a.open_result = reinterpret_cast<decltype(a.open_result)>(get("unumf_openResult"));
        a.format_double = reinterpret_cast<decltype(a.format_double)>(get("unumf_formatDouble"));
        a.format_decimal = reinterpret_cast<decltype(a.format_decimal)>(get("unumf_formatDecimal"));
        a.to_string = reinterpret_cast<decltype(a.to_string)>(get("unumf_resultToString"));
        a.close = reinterpret_cast<decltype(a.close)>(get("unumf_close"));
        a.close_result = reinterpret_cast<decltype(a.close_result)>(get("unumf_closeResult"));
        a.error_name = reinterpret_cast<decltype(a.error_name)>(get("u_errorName"));
        const bool complete = a.open && a.open_result && a.format_double && a.format_decimal && a.to_string &&
                              a.close && a.close_result && a.error_name;
        return complete ? &a : nullptr;
    }();
    return api;
}

void throw_java(JNIEnv* env, const char* cls, const char* message) {
    if (jclass c = env->FindClass(cls)) env->ThrowNew(c, message);
}

// Formats with `skeleton` in `locale`; `format` fills the result.
template <typename F>
jstring format(JNIEnv* env, jstring skeleton, jstring locale, F fill) {
    const Icu* api = icu();
    if (!api) {
        throw_java(env, "java/lang/UnsupportedOperationException", "Windows' ICU (icu.dll) is not available");
        return nullptr;
    }
    if (!skeleton || !locale) {
        throw_java(env, "java/lang/IllegalArgumentException", "U_ILLEGAL_ARGUMENT_ERROR");
        return nullptr;
    }
    const jchar* sk = env->GetStringChars(skeleton, nullptr);
    const std::u16string skel(reinterpret_cast<const char16_t*>(sk), static_cast<size_t>(env->GetStringLength(skeleton)));
    env->ReleaseStringChars(skeleton, sk);
    const char* loc = env->GetStringUTFChars(locale, nullptr);
    const std::string locale_id = loc ? loc : "";
    if (loc) env->ReleaseStringUTFChars(locale, loc);

    UErrorCode ec = 0;
    void* formatter = api->open(skel.data(), static_cast<int32_t>(skel.size()), locale_id.c_str(), &ec);
    void* result = ec <= 0 ? api->open_result(&ec) : nullptr;
    std::u16string text;
    if (ec <= 0) fill(api, formatter, result, &ec);
    if (ec <= 0) {
        const int32_t n = api->to_string(result, nullptr, 0, &ec);
        if (ec == 15 /* U_BUFFER_OVERFLOW_ERROR */ || ec <= 0) {
            ec = 0;
            text.resize(static_cast<size_t>(n));
            api->to_string(result, text.data(), n, &ec);
            if (ec == -124 /* U_STRING_NOT_TERMINATED_WARNING */) ec = 0;
        }
    }
    if (result) api->close_result(result);
    if (formatter) api->close(formatter);
    if (ec > 0) {
        throw_java(env, "java/lang/IllegalArgumentException", api->error_name(ec));
        return nullptr;
    }
    return env->NewString(reinterpret_cast<const jchar*>(text.data()), static_cast<jsize>(text.size()));
}

jstring JNICALL Icu_formatDouble(JNIEnv* env, jclass, jstring skeleton, jstring locale, jdouble value) {
    return format(env, skeleton, locale, [value](const Icu* api, void* f, void* r, UErrorCode* ec) {
        api->format_double(f, value, r, ec);
    });
}
jstring JNICALL Icu_formatDecimal(JNIEnv* env, jclass, jstring skeleton, jstring locale, jstring decimal) {
    if (!decimal) {
        throw_java(env, "java/lang/IllegalArgumentException", "U_ILLEGAL_ARGUMENT_ERROR");
        return nullptr;
    }
    const char* d = env->GetStringUTFChars(decimal, nullptr);
    const std::string number = d ? d : "";
    if (d) env->ReleaseStringUTFChars(decimal, d);
    return format(env, skeleton, locale, [&number](const Icu* api, void* f, void* r, UErrorCode* ec) {
        api->format_decimal(f, number.c_str(), static_cast<int32_t>(number.size()), r, ec);
    });
}

constexpr const char* kCls = "com.macapk.icu.HostIcu";

const HostMethod kMethods[] = {
    WINAPK_HOST_NATIVE(kCls, "nativeFormatDouble", "(Ljava/lang/String;Ljava/lang/String;D)Ljava/lang/String;",
                       Icu_formatDouble),
    WINAPK_HOST_NATIVE(kCls, "nativeFormatDecimal",
                       "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", Icu_formatDecimal),
};

}  // namespace

HostMethods native_icu_methods() { return {kMethods, sizeof(kMethods) / sizeof(kMethods[0])}; }

}  // namespace winapk::host
