#ifndef VXN_DRIVER_PATHS_H
#define VXN_DRIVER_PATHS_H

static bool driver_readable(const char *path) {
    FILE *file=fopen(path,"rb");
    if(!file) return false;
    fclose(file); return true;
}
static const char *driver_tool(const char *configured,const char *name) {
    return driver_readable(configured)?configured:name;
}
static char *driver_library(const char *installed,const char *built) {
    char executable[32768]; size_t length=0;
#if defined(_WIN32)
    length=(size_t)GetModuleFileNameA(NULL,executable,sizeof executable);
    if(length>=sizeof executable) length=0;
#elif defined(__linux__)
    ssize_t count=readlink("/proc/self/exe",executable,sizeof executable-1);
    if(count>0 && (size_t)count<sizeof executable-1) length=(size_t)count;
#endif
    if(length) {
        executable[length]=0;
        if(!vx_same_file(executable,VXN_BUILD_COMPILER_PATH)) {
            while(length && executable[length-1]!='/' && executable[length-1]!='\\') --length;
            if(length) {
                size_t size=strlen(installed)+1;
                char *result=malloc(length+size);
                if(result) { memcpy(result,executable,length); memcpy(result+length,installed,size); }
                return result;
            }
        }
    }
    size_t size=strlen(built)+1;
    char *result=malloc(size); if(result) memcpy(result,built,size);
    return result;
}
#endif
