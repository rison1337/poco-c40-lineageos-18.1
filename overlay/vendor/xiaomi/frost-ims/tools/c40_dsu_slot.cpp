// A separate, initially undiscoverable DSU slot; original images are read-only.
// No image deletion, physical-partition flashing, formatting or key operations.
#include <android/gsi/IImageService.h>
#include <android/gsi/MappedImage.h>
#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <libfiemap/image_manager.h>
#include <libfiemap/split_fiemap_writer.h>
#include <libgsi/libgsid.h>
#include <liblp/liblp.h>
#include <openssl/sha.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>
#include "c40_dsu_publish.h"

using android::base::unique_fd;
using android::gsi::IImageService;
using android::gsi::IGsiService;
static constexpr uint64_t kOldBytes=2147483648ULL,kNewBytes=8589934592ULL;
static const std::string kMeta="/metadata/gsi/dsu/c40grow/";
static const std::string kData="/data/gsi/dsu/c40grow/";
static const std::string kOriginalMeta="/metadata/gsi/dsu/dsu/";
static constexpr char kPrefix[]="dsu/c40grow/";
static constexpr char kOriginalPrefix[]="dsu/dsu/";

static bool valid_sha(const std::string& value){
    return value.size()==64 && value.find_first_not_of("0123456789abcdef")==std::string::npos;
}
static std::string finish(SHA256_CTX* sha){
    unsigned char raw[32];SHA256_Final(raw,sha);char hex[65];
    for(size_t i=0;i<32;i++)std::sprintf(hex+i*2,"%02x",raw[i]);return hex;
}
static std::string hash_fd(int fd,uint64_t bytes){
    SHA256_CTX sha;SHA256_Init(&sha);std::array<unsigned char,262144> buffer{};
    uint64_t total=0;
    while(total<bytes){
        ssize_t n=TEMP_FAILURE_RETRY(pread(fd,buffer.data(),std::min<uint64_t>(buffer.size(),bytes-total),total));
        if(n<=0)return {};SHA256_Update(&sha,buffer.data(),n);total+=n;
    }
    return finish(&sha);
}
static std::string hash_file(const std::string& path){
    unique_fd fd(open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC));struct stat st{};
    if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode))return {};return hash_fd(fd,st.st_size);
}
static bool directory(const std::string& path){
    struct stat st{};return lstat(path.c_str(),&st)==0 && S_ISDIR(st.st_mode) && st.st_uid==0;
}
static bool exists(const std::string& path){struct stat st{};return lstat(path.c_str(),&st)==0;}
static bool sync_dir(const std::string& path){
    unique_fd fd(open(path.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));return fd>=0 && fsync(fd)==0;
}
static bool sync_filesystem(const std::string& path){
    unique_fd fd(open(path.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));return fd>=0 && syncfs(fd)==0;
}
static bool publish_file(const std::string& name,const std::string& value){
    return c40::PublishFile(kMeta,name,value);
}
static std::string key_files_digest(){
    SHA256_CTX sha;SHA256_Init(&sha);
    for(const char* name:{"version","stretching","secdiscardable","keymaster_key_blob","encrypted_key"}){
        std::string contents;
        if(!android::base::ReadFileToString(std::string("/metadata/vold/metadata_encryption/gsi/key/")+name,&contents))return {};
        SHA256_Update(&sha,name,std::strlen(name)+1);SHA256_Update(&sha,contents.data(),contents.size());
    }
    return finish(&sha);
}
struct Transaction{
    uint64_t system_bytes=0;
    std::string system_sha,original_userdata_sha,userdata_sha,original_metadata_sha,keys_sha;
    std::string serialize()const{
        return "C40SLOT1\n"+std::to_string(system_bytes)+"\n"+system_sha+"\n"+
            original_userdata_sha+"\n"+userdata_sha+"\n"+original_metadata_sha+"\n"+keys_sha+"\n";
    }
    bool load(){
        std::string text;if(!android::base::ReadFileToString(kMeta+"transaction",&text))return false;
        auto lines=android::base::Split(text,"\n");
        if(lines.size()!=8 || lines[0]!="C40SLOT1" || !lines[7].empty() ||
           !android::base::ParseUint(lines[1],&system_bytes))return false;
        system_sha=lines[2];original_userdata_sha=lines[3];userdata_sha=lines[4];original_metadata_sha=lines[5];keys_sha=lines[6];
        return system_bytes>=512*1024*1024ULL && system_bytes<=3284570112ULL && system_bytes%4096==0 &&
            valid_sha(system_sha) && valid_sha(original_userdata_sha) && valid_sha(userdata_sha) &&
            valid_sha(original_metadata_sha) && valid_sha(keys_sha);
    }
};

static android::sp<IImageService> images(const android::sp<IGsiService>& service,const char* prefix){
    android::sp<IImageService> result;
    if(!service->openImageService(prefix,&result).isOk())return {};return result;
}
static bool no_mappings(const android::sp<IImageService>& service){
    for(const char* name:{"system_gsi","userdata_gsi"}){
        bool mapped=true;if(!service->isImageMapped(name,&mapped).isOk() || mapped)return false;
    }
    return true;
}
struct Mapping{
    android::sp<IImageService> service;std::string name,path;bool owned=false;unique_fd fd;
    bool open(const android::sp<IImageService>& svc,const char* image,uint64_t bytes,bool writable){
        service=svc;name=image;bool mapped=true;
        if(!service || !service->isImageMapped(name,&mapped).isOk() || mapped)return false;
        android::gsi::MappedImage result;
        if(!service->mapImageDevice(name,10000,&result).isOk() || result.path.empty())return false;
        owned=true;path=result.path;fd.reset(::open(path.c_str(),(writable?O_RDWR:O_RDONLY)|O_CLOEXEC|O_NOFOLLOW));
        uint64_t actual=0;return fd>=0 && ioctl(fd,BLKGETSIZE64,&actual)==0 && actual==bytes;
    }
    bool close(){
        fd.reset();if(!owned)return true;
        if(!service->unmapImageDevice(name).isOk())return false;owned=false;return true;
    }
    ~Mapping(){close();}
};
static std::string image_sha(const android::sp<IGsiService>& service,const char* prefix,const char* name,uint64_t bytes){
    Mapping map;if(!map.open(images(service,prefix),name,bytes,false))return {};
    auto hash=hash_fd(map.fd,bytes);if(!map.close())return {};return hash;
}
static bool original_pieces_pinned(){
    for(const char* name:{"userdata_gsi","system_gsi"}){
        std::vector<std::string> files;
        std::string header=std::string("/data/gsi/dsu/")+name+".img";
        if(!android::fiemap::SplitFiemap::GetSplitFileList(header,&files) || files.empty())return false;
        for(const auto& file:files){
            if(file.compare(0,header.size()+1,header+".")!=0 ||
               !android::fiemap::FiemapWriter::HasPinnedExtents(file))return false;
        }
    }
    return true;
}

static bool original_control_matches(const Transaction& tx){
    return hash_file(kOriginalMeta+"lp_metadata")==tx.original_metadata_sha && key_files_digest()==tx.keys_sha && original_pieces_pinned();
}
static bool validate_layout(const Transaction& tx){
    auto metadata=android::fs_mgr::ReadFromImageFile(kMeta+"lp_metadata");
    if(!metadata || metadata->partitions.size()!=2 || metadata->block_devices.size()!=1)return false;
    for(const auto& partition:metadata->partitions){
        auto name=android::fs_mgr::GetPartitionName(partition);
        uint64_t bytes=0;
        if(name=="system_gsi"){
            bytes=tx.system_bytes;if(partition.attributes!=LP_PARTITION_ATTR_READONLY)return false;
        }else if(name=="userdata_gsi"){
            bytes=kNewBytes;if(partition.attributes!=0)return false;
        }else return false;
        if(android::fs_mgr::GetPartitionSize(*metadata,partition)!=bytes)return false;
        auto file=android::fiemap::SplitFiemap::Open(kData+name+".img");
        if(!file || file->size()!=bytes || !file->HasPinnedExtents())return false;
        auto device=android::fs_mgr::GetBlockDevicePartitionName(metadata->block_devices[0]);
        struct stat file_device{},metadata_device{};
        if(stat(file->bdev_path().c_str(),&file_device) ||
           stat(("/dev/block/by-name/"+device).c_str(),&metadata_device) ||
           !S_ISBLK(file_device.st_mode) || !S_ISBLK(metadata_device.st_mode) ||
           file_device.st_rdev!=metadata_device.st_rdev)return false;
        uint64_t remaining=bytes/512;size_t index=0;
        for(const auto& extent:file->extents()){
            if(!remaining)break;
            if(extent.fe_physical%512 || extent.fe_length%512 || index>=partition.num_extents)return false;
            size_t position=static_cast<size_t>(partition.first_extent_index)+index++;
            if(position>=metadata->extents.size())return false;
            const auto& entry=metadata->extents[position];
            uint64_t sectors=std::min<uint64_t>(extent.fe_length/512,remaining);
            if(entry.target_type!=LP_TARGET_TYPE_LINEAR || entry.target_source!=0 ||
               entry.target_data!=extent.fe_physical/512 || entry.num_sectors!=sectors)return false;
            remaining-=sectors;
        }
        if(remaining || index!=partition.num_extents)return false;
    }
    return true;
}

int main(int argc,char** argv){
    umask(0077);rlimit core{0,0};setrlimit(RLIMIT_CORE,&core);prctl(PR_SET_DUMPABLE,0);
    if(getuid()!=0 || android::base::GetProperty("ro.product.vendor.device","")!="frost" ||
       android::base::GetProperty("sys.boot_completed","")!="1" || argc<2)return 2;
    auto service=android::gsi::GetGsiService();if(!service)return 3;
    bool running=true;if(!service->isGsiRunning(&running).isOk())return 3;
    std::string active;if(!android::base::ReadFileToString("/metadata/gsi/dsu/active",&active))return 3;
    if(argc==2 && std::strcmp(argv[1],"--inspect")==0){
        std::vector<std::string> slots;if(!service->getInstalledDsuSlots(&slots).isOk())return 3;
        std::printf("{\"running_gsi\":%s,\"active_slot\":\"%s\",\"registered_slot_count\":%zu,\"trial_registered\":%s,\"writes_performed\":false}\n",
            running?"true":"false",active.c_str(),slots.size(),exists(kMeta+"install_dir")?"true":"false");return 0;
    }
    // These checks precede every directory creation, write, map and key-file read.
    if(running)return 4;
    bool enabled=true,installing=true;
    if((active!="dsu" && active!="c40grow") || !service->isGsiEnabled(&enabled).isOk() || enabled ||
       !service->isGsiInstallInProgress(&installing).isOk() || installing)return 5;
    auto original=images(service,kOriginalPrefix);if(!original || !no_mappings(original))return 6;
    if(argc==2 && std::strcmp(argv[1],"--plan")==0){
        auto metadata=android::fs_mgr::ReadFromImageFile(kOriginalMeta+"lp_metadata");
        if(!metadata)return 7;
        auto system=android::fs_mgr::FindPartition(*metadata,"system_gsi");
        struct statvfs space{};if(!system || statvfs("/data/gsi/dsu",&space))return 9;
        uint64_t system_bytes=android::fs_mgr::GetPartitionSize(*metadata,*system);
        uint64_t available=static_cast<uint64_t>(space.f_bavail)*space.f_frsize;
        uint64_t required=kNewBytes+system_bytes+2ULL*1024*1024*1024;
        std::printf("{\"available_bytes\":%llu,\"required_bytes\":%llu,\"bytes_to_free\":%llu,\"total_blocks\":%llu,\"available_blocks\":%llu,\"block_size\":%lu,\"fragment_size\":%lu,\"original_pieces_pinned\":%s,\"writes_performed\":false}\n",
            static_cast<unsigned long long>(available),static_cast<unsigned long long>(required),
            static_cast<unsigned long long>(available<required?required-available:0),
            static_cast<unsigned long long>(space.f_blocks),static_cast<unsigned long long>(space.f_bavail),
            static_cast<unsigned long>(space.f_bsize),static_cast<unsigned long>(space.f_frsize),
            original_pieces_pinned()?"true":"false");return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--rollback-original")==0){
        Transaction tx;
        if(!tx.load() || !original_control_matches(tx) ||
           image_sha(service,kOriginalPrefix,"userdata_gsi",kOldBytes)!=tx.original_userdata_sha ||
           image_sha(service,kOriginalPrefix,"system_gsi",tx.system_bytes)!=tx.system_sha)return 26;
        int error=-1;
        if(!service->enableGsi(false,"dsu",&error).isOk() || error!=IGsiService::INSTALL_OK)return 27;
        sync();
        std::printf("{\"original_slot_selected\":true,\"userdata_written\":false,\"persistent_enabled\":true}\n");return 0;
    }
    if(active!="dsu")return 5;
    if(argc==6 && std::strcmp(argv[1],"--create")==0){
        Transaction tx;
        if(!android::base::ParseUint(argv[2],&tx.system_bytes) || tx.system_bytes<512*1024*1024ULL ||
            tx.system_bytes>3284570112ULL || tx.system_bytes%4096 ||
            !valid_sha(argv[3]) || !valid_sha(argv[4]) || !valid_sha(argv[5]))return 7;
        tx.system_sha=argv[3];tx.original_userdata_sha=argv[4];tx.userdata_sha=argv[5];
        tx.original_metadata_sha=hash_file(kOriginalMeta+"lp_metadata");tx.keys_sha=key_files_digest();
        if(!valid_sha(tx.original_metadata_sha) || !valid_sha(tx.keys_sha) || exists(kMeta) || exists(kData) ||
           !directory("/data/gsi/dsu") || !directory("/metadata/gsi/dsu") || !original_control_matches(tx))return 8;
        struct statvfs space{};
        if(statvfs("/data/gsi/dsu",&space))return 9;
        uint64_t available=static_cast<uint64_t>(space.f_bavail)*space.f_frsize;
        uint64_t required=kNewBytes+tx.system_bytes+2ULL*1024*1024*1024;
        if(available<required){
            std::fprintf(stderr,"{\"available_bytes\":%llu,\"required_bytes\":%llu,\"bytes_to_free\":%llu}\n",
                static_cast<unsigned long long>(available),static_cast<unsigned long long>(required),
                static_cast<unsigned long long>(required-available));return 9;
        }
        if(image_sha(service,kOriginalPrefix,"userdata_gsi",kOldBytes)!=tx.original_userdata_sha ||
           image_sha(service,kOriginalPrefix,"system_gsi",tx.system_bytes)!=tx.system_sha)return 10;
        if(mkdir(kMeta.c_str(),0700) || mkdir(kData.c_str(),0700) ||
           !sync_dir("/metadata/gsi/dsu") || !sync_dir("/data/gsi/dsu") ||
           !publish_file("transaction",tx.serialize()))return 11;
        auto trial=images(service,kPrefix);if(!trial)return 12;
        for(const char* name:{"userdata_gsi","system_gsi"}){
            bool present=true;
            if(!trial->backingImageExists(name,&present).isOk() || present)return 12;
            bool userdata=std::strcmp(name,"userdata_gsi")==0;
            std::fprintf(stderr,"Creating private trial %s, %llu bytes\n",name,
                         static_cast<unsigned long long>(userdata?kNewBytes:tx.system_bytes));std::fflush(stderr);
            if(!trial->createBackingImage(name,userdata?kNewBytes:tx.system_bytes,
                 userdata?IImageService::CREATE_IMAGE_DEFAULT:IImageService::CREATE_IMAGE_READONLY,nullptr).isOk())return 13;
        }
        // Persist allocation and clean the backing-file page cache before any
        // future raw dm-linear writes can replace its physical contents.
        if(!sync_filesystem(kData) || !sync_filesystem(kMeta) ||
           !validate_layout(tx) || !original_control_matches(tx) ||
           image_sha(service,kOriginalPrefix,"userdata_gsi",kOldBytes)!=tx.original_userdata_sha)return 14;
        std::printf("{\"trial_created\":true,\"original_userdata_unchanged\":true,\"original_metadata_and_keys_unchanged\":true,\"trial_registered\":false,\"boot_selection_changed\":false}\n");return 0;
    }
    Transaction tx;if(!tx.load() || !directory(kMeta) || !directory(kData) || !original_control_matches(tx))return 15;
    bool write_userdata=argc==2 && std::strcmp(argv[1],"--write-userdata")==0;
    bool write_system=argc==2 && std::strcmp(argv[1],"--write-system")==0;
    if(write_userdata || write_system){
        if(exists(kMeta+"install_dir") || !sync_filesystem(kData) ||
           !sync_filesystem(kMeta) || !validate_layout(tx))return 16;
        const char* name=write_userdata?"userdata_gsi":"system_gsi";
        uint64_t bytes=write_userdata?kNewBytes:tx.system_bytes;
        std::string expected=write_userdata?tx.userdata_sha:tx.system_sha;
        std::string marker=write_userdata?"userdata.verified":"system.verified";
        if(exists(kMeta+marker))return 17;
        Mapping map;if(!map.open(images(service,kPrefix),name,bytes,true))return 18;
        std::array<unsigned char,262144> buffer{};SHA256_CTX sha;SHA256_Init(&sha);uint64_t total=0;
        while(total<bytes){
            ssize_t n=TEMP_FAILURE_RETRY(read(STDIN_FILENO,buffer.data(),std::min<uint64_t>(buffer.size(),bytes-total)));
            if(n<=0)return 19;
            if(!android::base::WriteFully(map.fd,buffer.data(),n))return 20;
            SHA256_Update(&sha,buffer.data(),n);total+=n;
        }
        unsigned char extra;
        if(TEMP_FAILURE_RETRY(read(STDIN_FILENO,&extra,1))!=0 || finish(&sha)!=expected || fsync(map.fd))return 21;
        if(hash_fd(map.fd,bytes)!=expected || !map.close() || !validate_layout(tx) ||
           !original_control_matches(tx) || !publish_file(marker,expected+"\n"))return 22;
        std::printf("{\"trial_image_written\":\"%s\",\"bytes\":%llu,\"input_and_readback_sha256_match\":true,\"mappings_removed\":true,\"original_metadata_and_keys_unchanged\":true,\"boot_selection_changed\":false}\n",
            name,static_cast<unsigned long long>(bytes));return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--publish")==0){
        std::string userdata_verified,system_verified;
        if(!validate_layout(tx) ||
           !android::base::ReadFileToString(kMeta+"userdata.verified",&userdata_verified) || userdata_verified!=tx.userdata_sha+"\n" ||
           !android::base::ReadFileToString(kMeta+"system.verified",&system_verified) || system_verified!=tx.system_sha+"\n")return 23;
        if(image_sha(service,kOriginalPrefix,"userdata_gsi",kOldBytes)!=tx.original_userdata_sha ||
           image_sha(service,kOriginalPrefix,"system_gsi",tx.system_bytes)!=tx.system_sha ||
           image_sha(service,kPrefix,"userdata_gsi",kNewBytes)!=tx.userdata_sha ||
           image_sha(service,kPrefix,"system_gsi",tx.system_bytes)!=tx.system_sha ||
           !original_control_matches(tx) || !validate_layout(tx))return 24;
        if(!sync_filesystem(kData) || !sync_filesystem(kMeta))return 25;
        // Startup cleanup discovers slots only through install_dir. Publish
        // complete first so it can never discover this slot as incomplete.
        if(!publish_file("complete","OK") || !publish_file("install_dir",kData))return 25;
        std::printf("{\"trial_registered\":true,\"slot\":\"c40grow\",\"all_four_images_verified\":true,\"layout_matches_pinned_file_extents\":true,\"original_images_retained\":true,\"boot_selection_changed\":false}\n");return 0;
    }
    if(argc==2 && std::strcmp(argv[1],"--enable-trial")==0){
        std::string complete,install;
        if(!validate_layout(tx) || !android::base::ReadFileToString(kMeta+"complete",&complete) || complete!="OK" ||
           !android::base::ReadFileToString(kMeta+"install_dir",&install) || install!=kData)return 28;
        // Publication already read all four images. Revalidate original data,
        // metadata and pinning immediately before changing boot selection.
        if(image_sha(service,kOriginalPrefix,"userdata_gsi",kOldBytes)!=tx.original_userdata_sha ||
           !original_control_matches(tx))return 29;
        int error=-1;
        if(!service->enableGsi(true,"c40grow",&error).isOk() || error!=IGsiService::INSTALL_OK)return 30;
        sync();
        std::printf("{\"trial_selected\":true,\"slot\":\"c40grow\",\"single_boot\":true,\"original_images_retained\":true}\n");return 0;
    }
    return 2;
}
