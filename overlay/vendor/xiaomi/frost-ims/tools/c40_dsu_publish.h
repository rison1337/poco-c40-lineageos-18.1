#pragma once
// Shared by the Android helper and the host crash-injection test. No keys.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <functional>
#include <string>

namespace c40 {
inline bool PublishFile(const std::string& directory,const std::string& name,
                        const std::string& value,
                        const std::function<void(const char*)>& fault={}) {
    if(name.empty() || name[0]=='.' || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz._")!=std::string::npos)return false;
    std::string normalized=directory;
    while(normalized.size()>1 && normalized.back()=='/')normalized.pop_back();
    int dir=open(normalized.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if(dir<0)return false;
    struct Guard {int fd;~Guard(){if(fd>=0)close(fd);}} parent{dir};
    struct stat ds{};
    if(fstat(dir,&ds) || ds.st_uid!=geteuid() || (ds.st_mode&0022))return false;
    auto hook=[&](const char* point){if(fault)fault(point);};
    auto read_regular=[&](const std::string& file,std::string* contents,bool* absent){
        *absent=false;
        int fd=openat(dir,file.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if(fd<0){*absent=errno==ENOENT;return *absent;}
        Guard item{fd};struct stat st{};
        if(fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() ||
           st.st_nlink!=1 || (st.st_mode&0077) || st.st_size<0 ||
           static_cast<unsigned long long>(st.st_size)>value.size())return false;
        contents->resize(st.st_size);size_t offset=0;
        while(offset<contents->size()){
            ssize_t n=read(fd,&(*contents)[offset],contents->size()-offset);
            if(n<0 && errno==EINTR)continue;
            if(n<=0)return false;
            offset+=n;
        }
        return true;
    };
    std::string existing;bool absent=false;
    if(!read_regular(name,&existing,&absent))return false;
    if(!absent){
        if(existing!=value)return false;
        return fsync(dir)==0;
    }
    std::string temporary=name+".new",pending;bool no_pending=false;
    if(!read_regular(temporary,&pending,&no_pending))return false;
    if(!no_pending){
        // A killed writer may have left an exact prefix. A different
        // transaction's contents are never overwritten.
        if(value.compare(0,pending.size(),pending)!=0)return false;
        if(unlinkat(dir,temporary.c_str(),0))return false;
    }
    hook("before_open");
    int fd=openat(dir,temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return false;
    Guard output{fd};hook("after_open");
    size_t offset=0;
    while(offset<value.size()){
        ssize_t n=write(fd,value.data()+offset,value.size()-offset);
        if(n<0 && errno==EINTR)continue;
        if(n<=0)return false;
        offset+=n;hook("after_write");
    }
    if(fsync(fd))return false;
    hook("after_file_fsync");
    if(close(fd))return false;
    output.fd=-1;
    // Single-writer transaction: the host operation lock owns the protocol.
    if(renameat(dir,temporary.c_str(),dir,name.c_str()))return false;
    hook("after_rename");
    if(fsync(dir))return false;
    hook("after_directory_fsync");
    return true;
}
}
