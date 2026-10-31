// Android has no native file dialog backend: JFG ANDROID uses JFGActivity instead (see runtime/support.cpp).
// From Zelda64Recomp-Android (linkzenic).
#include "nfd.h"

#include <cstdlib>

namespace {
constexpr const char* kUnavailable = "Native file dialogs are unavailable on Android.";
}

extern "C" {

NFD_API void NFD_FreePathN(nfdnchar_t* filePath) {
    std::free(filePath);
}

NFD_API void NFD_FreePathU8(nfdu8char_t* filePath) {
    std::free(filePath);
}

NFD_API nfdresult_t NFD_Init(void) {
    return NFD_OKAY;
}

NFD_API void NFD_Quit(void) {
}

NFD_API nfdresult_t NFD_OpenDialogN(nfdnchar_t** outPath, const nfdnfilteritem_t*, nfdfiltersize_t, const nfdnchar_t*) {
    if (outPath != nullptr) {
        *outPath = nullptr;
    }
    return NFD_CANCEL;
}

NFD_API nfdresult_t NFD_OpenDialogU8(nfdu8char_t** outPath, const nfdu8filteritem_t*, nfdfiltersize_t, const nfdu8char_t*) {
    if (outPath != nullptr) {
        *outPath = nullptr;
    }
    return NFD_CANCEL;
}

NFD_API nfdresult_t NFD_OpenDialogMultipleN(const nfdpathset_t** outPaths, const nfdnfilteritem_t*, nfdfiltersize_t, const nfdnchar_t*) {
    if (outPaths != nullptr) {
        *outPaths = nullptr;
    }
    return NFD_CANCEL;
}

NFD_API nfdresult_t NFD_OpenDialogMultipleU8(const nfdpathset_t** outPaths, const nfdu8filteritem_t*, nfdfiltersize_t, const nfdu8char_t*) {
    if (outPaths != nullptr) {
        *outPaths = nullptr;
    }
    return NFD_CANCEL;
}

NFD_API nfdresult_t NFD_SaveDialogN(nfdnchar_t** outPath, const nfdnfilteritem_t*, nfdfiltersize_t, const nfdnchar_t*, const nfdnchar_t*) {
    if (outPath != nullptr) {
        *outPath = nullptr;
    }
    return NFD_CANCEL;
}

NFD_API nfdresult_t NFD_SaveDialogU8(nfdu8char_t** outPath, const nfdu8filteritem_t*, nfdfiltersize_t, const nfdu8char_t*, const nfdu8char_t*) {
    if (outPath != nullptr) {
        *outPath = nullptr;
    }
    return NFD_CANCEL;
}

NFD_API nfdresult_t NFD_PickFolderN(nfdnchar_t** outPath, const nfdnchar_t*) {
    if (outPath != nullptr) {
        *outPath = nullptr;
    }
    return NFD_CANCEL;
}

NFD_API nfdresult_t NFD_PickFolderU8(nfdu8char_t** outPath, const nfdu8char_t*) {
    if (outPath != nullptr) {
        *outPath = nullptr;
    }
    return NFD_CANCEL;
}

NFD_API const char* NFD_GetError(void) {
    return kUnavailable;
}

NFD_API void NFD_ClearError(void) {
}

NFD_API nfdresult_t NFD_PathSet_GetCount(const nfdpathset_t*, nfdpathsetsize_t* count) {
    if (count != nullptr) {
        *count = 0;
    }
    return NFD_OKAY;
}

NFD_API nfdresult_t NFD_PathSet_GetPathN(const nfdpathset_t*, nfdpathsetsize_t, nfdnchar_t** outPath) {
    if (outPath != nullptr) {
        *outPath = nullptr;
    }
    return NFD_ERROR;
}

NFD_API nfdresult_t NFD_PathSet_GetPathU8(const nfdpathset_t*, nfdpathsetsize_t, nfdu8char_t** outPath) {
    if (outPath != nullptr) {
        *outPath = nullptr;
    }
    return NFD_ERROR;
}

NFD_API void NFD_PathSet_FreePathN(const nfdnchar_t* filePath) {
    std::free(const_cast<nfdnchar_t*>(filePath));
}

NFD_API void NFD_PathSet_FreePathU8(const nfdu8char_t* filePath) {
    std::free(const_cast<nfdu8char_t*>(filePath));
}

NFD_API nfdresult_t NFD_PathSet_GetEnum(const nfdpathset_t*, nfdpathsetenum_t* outEnumerator) {
    if (outEnumerator != nullptr) {
        outEnumerator->ptr = nullptr;
    }
    return NFD_OKAY;
}

NFD_API void NFD_PathSet_FreeEnum(nfdpathsetenum_t*) {
}

NFD_API nfdresult_t NFD_PathSet_EnumNextN(nfdpathsetenum_t*, nfdnchar_t** outPath) {
    if (outPath != nullptr) {
        *outPath = nullptr;
    }
    return NFD_OKAY;
}

NFD_API nfdresult_t NFD_PathSet_EnumNextU8(nfdpathsetenum_t*, nfdu8char_t** outPath) {
    if (outPath != nullptr) {
        *outPath = nullptr;
    }
    return NFD_OKAY;
}

NFD_API void NFD_PathSet_Free(const nfdpathset_t*) {
}

}
