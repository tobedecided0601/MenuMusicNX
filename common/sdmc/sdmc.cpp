#include "sdmc.hpp"

namespace sdmc {

    namespace {

        FsFileSystem sdmc;
    }

    Result Open() {
        return fsOpenSdCardFileSystem(&sdmc);
    }

    void Close() {
        fsFsClose(&sdmc);
    }

    Result OpenFile(FsFile *file, const char *path, int open_mode) {
        if (file == nullptr || path == nullptr)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsOpenFile(&sdmc, path, open_mode, file);
    }

    Result OpenDir(FsDir *dir, const char *path, int open_mode) {
        if (dir == nullptr || path == nullptr)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsOpenDirectory(&sdmc, path, open_mode, dir);
    }

    Result GetType(const char* path, FsDirEntryType* type) {
        if (path == nullptr || type == nullptr)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsGetEntryType(&sdmc, path, type);
    }

    bool FileExists(const char* path) {
        FsDirEntryType type;
        return R_SUCCEEDED(GetType(path, &type)) && type == FsDirEntryType_File;
    }

    Result CreateFolder(const char* path) {
        if (path == nullptr)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        return fsFsCreateDirectory(&sdmc, path);
    }

    Result AppendFile(const char* path, const void* data, size_t size) {
        if (path == nullptr || data == nullptr || size == 0)
            return MAKERESULT(Module_Libnx, LibnxError_BadInput);

        /* FsOpenMode_Append only lets writes grow the file; the offset passed to
           fsFileWrite is still honoured. Write at the current end of the file,
           otherwise every call overwrites the start (minGlue.c tracks its own
           offset for the same reason). */
        FsFile file;
        Result rc = fsFsOpenFile(&sdmc, path, FsOpenMode_Write | FsOpenMode_Append, &file);
        if (R_FAILED(rc)) {
            rc = fsFsCreateFile(&sdmc, path, 0, 0);
            if (R_FAILED(rc))
                return rc;
            rc = fsFsOpenFile(&sdmc, path, FsOpenMode_Write | FsOpenMode_Append, &file);
            if (R_FAILED(rc))
                return rc;
        }

        s64 offset = 0;
        rc = fsFileGetSize(&file, &offset);
        if (R_SUCCEEDED(rc))
            rc = fsFileWrite(&file, offset, data, size, FsWriteOption_Flush);
        fsFileClose(&file);
        return rc;
    }

}
