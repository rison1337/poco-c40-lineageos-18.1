// No phone image writes: probe and host-stream metadata transforms only.
// Existing Keymaster keys are read; never generated, upgraded or exported.
#include <android-base/file.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/unique_fd.h>
#include <android/gsi/IImageService.h>
#include <android/gsi/MappedImage.h>
#include <libgsi/libgsid.h>
#include <libdm/dm.h>
#include <Keymaster.h>
#include <keymasterV4_1/keymaster_utils.h>
#include <openssl/crypto.h>
#include <openssl/aes.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <linux/fscrypt.h>
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

using android::base::unique_fd;
using android::dm::DeviceMapper;
using android::dm::DmDeviceState;
using android::vold::KeyBuffer;
namespace km=android::vold::km;
static constexpr char kKeyDir[]="/metadata/vold/metadata_encryption/gsi/key/";
static constexpr char kProbeName[]="c40_dsu_readonly_probe";

static std::string metadata_digest(){
    SHA256_CTX sha;SHA256_Init(&sha);
    for(const char* name:{"version","stretching","secdiscardable","keymaster_key_blob","encrypted_key"}){
        std::string data;
        if(!android::base::ReadFileToString(std::string(kKeyDir)+name,&data))return {};
        SHA256_Update(&sha,name,std::strlen(name)+1);SHA256_Update(&sha,data.data(),data.size());
    }
    unsigned char hash[SHA256_DIGEST_LENGTH];SHA256_Final(hash,&sha);
    return std::string(reinterpret_cast<char*>(hash),sizeof(hash));
}

static bool read_existing_key(KeyBuffer* key){
    std::string version,stretching,discardable,blob,ciphertext;
    auto read=[&](const char* name,std::string* out){return android::base::ReadFileToString(std::string(kKeyDir)+name,out);};
    if(!read("version",&version) || version!="1" || !read("stretching",&stretching) || stretching!="nopassword" ||
       !read("secdiscardable",&discardable) || discardable.size()!=16384 ||
       !read("keymaster_key_blob",&blob) || !read("encrypted_key",&ciphertext) || ciphertext.size()!=92)return false;
    // Same app-id derivation as vold KeyStorage.cpp; no credential secret in this key class.
    std::string prefix="Android secdiscardable SHA512";prefix.resize(SHA512_CBLOCK,'\0');
    SHA512_CTX hash;SHA512_Init(&hash);SHA512_Update(&hash,prefix.data(),prefix.size());
    SHA512_Update(&hash,discardable.data(),discardable.size());
    std::string application_id(SHA512_DIGEST_LENGTH,'\0');
    SHA512_Final(reinterpret_cast<unsigned char*>(&application_id[0]),&hash);
    auto params=km::AuthorizationSetBuilder().GcmModeMacLen(128)
        .Authorization(km::TAG_APPLICATION_ID,km::support::blob2hidlVec(application_id))
        .Authorization(km::TAG_NONCE,km::support::blob2hidlVec(ciphertext.substr(0,12)));
    android::vold::Keymaster keymaster;
    if(!keymaster)return false;
    auto operation=keymaster.begin(km::KeyPurpose::DECRYPT,blob,params,km::HardwareAuthToken(),nullptr);
    // Unlike retrieveKey(), this code explicitly refuses KEY_REQUIRES_UPGRADE.
    if(!operation){std::fprintf(stderr,"KEYMASTER_READ_STATUS=%d\n",static_cast<int>(operation.errorCode()));return false;}
    std::string body=ciphertext.substr(12);
    return operation.updateCompletely(body,key) && operation.finish(nullptr) && key->size()==64;
}

struct MappingCleanup{
    android::sp<android::gsi::IImageService> images;
    bool raw_owned=false,probe_owned=false;
    bool finish(){
        bool ok=true;
        if(probe_owned){ok=DeviceMapper::Instance().DeleteDevice(kProbeName,std::chrono::seconds(5));if(ok)probe_owned=false;}
        if(raw_owned && !probe_owned){bool removed=images->unmapImageDevice("userdata_gsi").isOk();ok=removed && ok;if(removed)raw_owned=false;}
        return ok && !raw_owned && !probe_owned;
    }
    ~MappingCleanup(){finish();}
};

static bool f2fs_superblock(const std::array<unsigned char,8192>& data){
    const unsigned char magic[]={0x10,0x20,0xf5,0xf2};
    return std::memcmp(data.data()+1024,magic,4)==0 && std::memcmp(data.data()+5120,magic,4)==0;
}

static bool transform_units(const KeyBuffer& key,const unsigned char* input,unsigned char* output,
                            size_t bytes,uint64_t first_unit,bool encrypt){
    if(bytes%4096)return false;
    // Android11's shared BoringSSL has no EVP XTS export. Whole4096B units need
    // only XEX full-block processing; no ciphertext stealing or padding.
    AES_KEY data_key,tweak_key;
    int status=encrypt?AES_set_encrypt_key(reinterpret_cast<const unsigned char*>(key.data()),256,&data_key):
                       AES_set_decrypt_key(reinterpret_cast<const unsigned char*>(key.data()),256,&data_key);
    if(status || AES_set_encrypt_key(reinterpret_cast<const unsigned char*>(key.data()+32),256,&tweak_key))return false;
    for(size_t offset=0;offset<bytes;offset+=4096){
        unsigned char iv[16]={};uint64_t unit=first_unit+offset/4096;
        for(int i=0;i<8;i++)iv[i]=static_cast<unsigned char>(unit>>(i*8));
        unsigned char tweak[16];AES_encrypt(iv,tweak,&tweak_key);
        for(size_t block=0;block<4096;block+=16){
            unsigned char temporary[16],plain[16];
            for(int i=0;i<16;i++)temporary[i]=input[offset+block+i]^tweak[i];
            if(encrypt)AES_encrypt(temporary,plain,&data_key);else AES_decrypt(temporary,plain,&data_key);
            for(int i=0;i<16;i++)output[offset+block+i]=plain[i]^tweak[i];
            unsigned carry=0;
            for(int i=0;i<16;i++){unsigned next=tweak[i]>>7;tweak[i]=static_cast<unsigned char>((tweak[i]<<1)|carry);carry=next;}
            if(carry)tweak[0]^=0x87;
        }
    }
    OPENSSL_cleanse(&data_key,sizeof(data_key));OPENSSL_cleanse(&tweak_key,sizeof(tweak_key));
    return true;
}

static bool decrypt_units(const KeyBuffer& key,const unsigned char* input,unsigned char* output,
                          size_t bytes,uint64_t first_unit){
    return transform_units(key,input,output,bytes,first_unit,false);
}

static std::string image_prefix(){
    std::string slot;
    if(!android::base::ReadFileToString("/metadata/gsi/dsu/active",&slot) || (slot!="dsu" && slot!="c40grow"))return {};
    return "dsu/"+slot+"/";
}

static bool software_matches_dm(const KeyBuffer& key,const char* raw_path,const char* plain_path){
    unique_fd raw(open(raw_path,O_RDONLY|O_CLOEXEC));
    unique_fd live(open(plain_path,O_RDONLY|O_CLOEXEC));
    std::array<unsigned char,8192> encrypted{},plain{},expected{},roundtrip{};
    return raw>=0 && live>=0 && pread(raw,encrypted.data(),encrypted.size(),0)==static_cast<ssize_t>(encrypted.size()) &&
        pread(live,expected.data(),expected.size(),0)==static_cast<ssize_t>(expected.size()) &&
        decrypt_units(key,encrypted.data(),plain.data(),plain.size(),0) && f2fs_superblock(plain) && plain==expected &&
        transform_units(key,plain.data(),roundtrip.data(),roundtrip.size(),0,true) && roundtrip==encrypted;
}

static std::string finish_sha(SHA256_CTX* sha){
    unsigned char digest[SHA256_DIGEST_LENGTH];SHA256_Final(digest,sha);char hex[65];
    for(size_t i=0;i<sizeof(digest);i++)std::sprintf(hex+2*i,"%02x",digest[i]);
    return hex;
}

static bool validate_stream_crypto(const KeyBuffer& key,const android::sp<android::gsi::IGsiService>& service,bool inactive){
    if(!inactive)return software_matches_dm(key,"/dev/block/mapper/userdata_gsi","/dev/block/mapper/userdata");
    MappingCleanup cleanup;auto prefix=image_prefix();
    if(prefix.empty() || !service->openImageService(prefix,&cleanup.images).isOk() || !cleanup.images)return false;
    bool mapped=false;
    if(!cleanup.images->isImageMapped("userdata_gsi",&mapped).isOk())return false;
    android::gsi::MappedImage raw;
    if(mapped){if(!cleanup.images->getMappedImageDevice("userdata_gsi",&raw.path).isOk())return false;}
    else{if(!cleanup.images->mapImageDevice("userdata_gsi",10000,&raw).isOk())return false;cleanup.raw_owned=true;}
    unique_fd fd(open(raw.path.c_str(),O_RDONLY|O_CLOEXEC));uint64_t bytes=0;
    if(fd<0 || ioctl(fd,BLKGETSIZE64,&bytes) || (bytes!=2147483648ULL && bytes!=8589934592ULL))return false;
    std::string hex(key.size()*2,'\0');const char* digits="0123456789abcdef";
    for(size_t i=0;i<key.size();i++){unsigned char b=key[i];hex[i*2]=digits[b>>4];hex[i*2+1]=digits[b&15];}
    auto target=std::make_unique<android::dm::DmTargetDefaultKey>(0,bytes/512,"aes-xts-plain64",hex,raw.path,0);
    target->SetSetDun();android::dm::DmTable table;table.set_readonly(true);table.AddTarget(std::move(target));
    std::string path;
    bool created=DeviceMapper::Instance().CreateDevice(kProbeName,table,&path,std::chrono::seconds(5));
    OPENSSL_cleanse(&hex[0],hex.size());
    if(!created)return false;cleanup.probe_owned=true;
    unique_fd decoded(open(path.c_str(),O_RDONLY|O_CLOEXEC));int readonly=0;
    bool matched=decoded>=0 && ioctl(decoded,BLKROGET,&readonly)==0 && readonly==1 &&
        software_matches_dm(key,raw.path.c_str(),path.c_str());
    decoded.reset();fd.reset();return cleanup.finish() && matched;
}

static int exercise_new_capacity(bool retain_for_boot=false){
    struct statvfs space{};std::string slot;
    if(statvfs("/data",&space) || static_cast<uint64_t>(space.f_blocks)*space.f_frsize<7ULL*1024*1024*1024 ||
       static_cast<uint64_t>(space.f_bavail)*space.f_frsize<3ULL*1024*1024*1024+128*1024*1024 ||
       !android::base::ReadFileToString("/metadata/gsi/dsu/active",&slot) || slot!="c40grow")return 42;
    unique_fd parent(open("/data/user/0/org.c40.diagnostics/files",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    struct fscrypt_get_policy_ex_arg policy{};policy.policy_size=sizeof(policy.policy);
    if(parent<0 || ioctl(parent,FS_IOC_GET_ENCRYPTION_POLICY_EX,&policy) || policy.policy.version!=FSCRYPT_POLICY_V2 ||
       !(policy.policy.v2.flags&0x10))return 43;
    const char* name=retain_for_boot?"c40_userdata_growth_reboot_test.bin":"c40_userdata_growth_test.bin";
    unique_fd file(openat(parent,name,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));
    if(file<0)return 44;
    struct Cleanup{
        int directory;const char* name;bool owned=true;
        ~Cleanup(){if(owned)unlinkat(directory,name,0);}
    } cleanup{parent,name};
    constexpr uint64_t bytes=3ULL*1024*1024*1024;
    std::array<unsigned char,262144> buffer{};
    if(RAND_bytes(buffer.data(),buffer.size())!=1)return 45;
    SHA256_CTX written,readback;SHA256_Init(&written);SHA256_Init(&readback);
    auto started=std::chrono::steady_clock::now();
    for(uint64_t offset=0;offset<bytes;offset+=buffer.size()){
        // Different header for every chunk also detects accidental block aliases.
        std::memcpy(buffer.data(),&offset,sizeof(offset));
        if(!android::base::WriteFully(file,buffer.data(),buffer.size()))return 46;
        SHA256_Update(&written,buffer.data(),buffer.size());
    }
    struct stat info{};
    if(fsync(file) || fstat(file,&info) || static_cast<uint64_t>(info.st_size)!=bytes)return 47;
    // Read back through storage/fscrypt, rather than the writer's plaintext cache.
    if(posix_fadvise(file,0,bytes,POSIX_FADV_DONTNEED))return 47;
    for(uint64_t offset=0;offset<bytes;offset+=buffer.size()){
        size_t received=0;
        while(received<buffer.size()){
            ssize_t n=TEMP_FAILURE_RETRY(pread(file,buffer.data()+received,buffer.size()-received,offset+received));
            if(n<=0)return 48;received+=n;
        }
        SHA256_Update(&readback,buffer.data(),buffer.size());
    }
    std::string digest=finish_sha(&written);
    if(digest!=finish_sha(&readback))return 49;
    file.reset();
    if((!retain_for_boot && unlinkat(parent,name,0)) || fsync(parent))return 50;
    cleanup.owned=false;
    double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
    std::printf("{\"bytes_written_fsynced_and_read\":%llu,\"sha256\":\"%s\",\"sha256_matches\":true,\"page_cache_drop_requested\":true,\"fscrypt_policy_version\":2,\"policy_flags\":%u,\"iv_ino_lblk_32\":true,\"own_file_deleted\":%s,\"retained_for_reboot\":%s,\"seconds\":%.3f}\n",
        static_cast<unsigned long long>(bytes),digest.c_str(),policy.policy.v2.flags,
        retain_for_boot?"false":"true",retain_for_boot?"true":"false",elapsed);return 0;
}

static int verify_persistent_capacity(const char* expected){
    constexpr uint64_t bytes=3ULL*1024*1024*1024;
    struct statvfs space{};std::string slot;
    if(statvfs("/data",&space) || static_cast<uint64_t>(space.f_blocks)*space.f_frsize<7ULL*1024*1024*1024 ||
       !android::base::ReadFileToString("/metadata/gsi/dsu/active",&slot) || slot!="c40grow")return 42;
    unique_fd parent(open("/data/user/0/org.c40.diagnostics/files",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));
    struct fscrypt_get_policy_ex_arg policy{};policy.policy_size=sizeof(policy.policy);
    if(parent<0 || ioctl(parent,FS_IOC_GET_ENCRYPTION_POLICY_EX,&policy) || policy.policy.version!=FSCRYPT_POLICY_V2 ||
       !(policy.policy.v2.flags&0x10))return 43;
    constexpr char name[]="c40_userdata_growth_reboot_test.bin";
    unique_fd file(openat(parent,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC));struct stat info{};
    if(file<0 || fstat(file,&info) || !S_ISREG(info.st_mode) || info.st_nlink!=1 || info.st_uid!=0 ||
       (info.st_mode&0777)!=0600 || static_cast<uint64_t>(info.st_size)!=bytes)return 51;
    if(posix_fadvise(file,0,bytes,POSIX_FADV_DONTNEED))return 47;
    SHA256_CTX hash;SHA256_Init(&hash);std::array<unsigned char,262144> buffer{};
    uint64_t total=0;
    while(total<bytes){
        ssize_t n=TEMP_FAILURE_RETRY(read(file,buffer.data(),buffer.size()));
        if(n<=0)return 48;total+=n;SHA256_Update(&hash,buffer.data(),n);
    }
    std::string digest=finish_sha(&hash);
    if(digest!=expected)return 49;
    file.reset();if(unlinkat(parent,name,0) || fsync(parent))return 50;
    std::printf("{\"bytes_read_after_reboot\":%llu,\"sha256_matches\":true,\"fscrypt_policy_version\":2,\"policy_flags\":%u,\"own_file_deleted\":true}\n",
        static_cast<unsigned long long>(total),policy.policy.v2.flags);return 0;
}

static int decrypt_backup_stream(const KeyBuffer& key,uint64_t bytes,const char* expected,const std::string& before,bool verified){
    if(!verified)return 20;
    SHA256_CTX encrypted_sha,plain_sha;SHA256_Init(&encrypted_sha);SHA256_Init(&plain_sha);
    std::array<unsigned char,262144> input{},output{};uint64_t total=0;
    while(total<bytes){
        size_t wanted=static_cast<size_t>(std::min<uint64_t>(input.size(),bytes-total)),received=0;
        while(received<wanted){ssize_t n=TEMP_FAILURE_RETRY(read(STDIN_FILENO,input.data()+received,wanted-received));if(n<=0)return 21;received+=n;}
        SHA256_Update(&encrypted_sha,input.data(),wanted);
        if(!decrypt_units(key,input.data(),output.data(),wanted,total/4096))return 22;
        if(total==0){std::array<unsigned char,8192> super{};std::memcpy(super.data(),output.data(),super.size());if(!f2fs_superblock(super))return 23;}
        SHA256_Update(&plain_sha,output.data(),wanted);
        if(std::fwrite(output.data(),1,wanted,stdout)!=wanted)return 24;
        total+=wanted;
    }
    if(std::fflush(stdout))return 25;
    bool source_matches=finish_sha(&encrypted_sha)==expected,keys_unchanged=metadata_digest()==before;
    std::string output_sha=finish_sha(&plain_sha);
    std::fprintf(stderr,"{\"stream_bytes\":%llu,\"output_sha256\":\"%s\",\"input_sha256_matches\":%s,\"software_matches_kernel_dm\":true,\"key_files_unchanged\":%s,\"storage_written\":false,\"inspection_view_only\":true,\"private_key_exported\":false}\n",
        static_cast<unsigned long long>(total),output_sha.c_str(),source_matches?"true":"false",keys_unchanged?"true":"false");
    return source_matches && keys_unchanged?0:26;
}

static bool read_exact(void* buffer,size_t bytes){
    auto out=static_cast<unsigned char*>(buffer);size_t done=0;
    while(done<bytes){ssize_t n=TEMP_FAILURE_RETRY(read(STDIN_FILENO,out+done,bytes-done));if(n<=0)return false;done+=n;}
    return true;
}

static uint64_t little_u64(const unsigned char* p){
    uint64_t value=0;for(int i=0;i<8;i++)value|=static_cast<uint64_t>(p[i])<<(8*i);return value;
}

// Protocol: 24-byte header ("C40XTS1\n", image_bytes:u64le, count:u64le),
// followed by sorted unique records (block_index:u64le, 4096 data bytes).
// Only quarantined host files receive output. No phone block device is opened writable.
static int transform_block_stream(const KeyBuffer& key,uint64_t count,const char* expected,
                                  const std::string& before,bool encrypt,bool verified){
    if(!verified)return 30;
    std::array<unsigned char,24> header{};
    if(!read_exact(header.data(),header.size()) || std::memcmp(header.data(),"C40XTS1\n",8))return 31;
    uint64_t bytes=little_u64(header.data()+8);
    if(bytes<2147483648ULL || bytes>32ULL*1024*1024*1024 || bytes%4096 ||
       little_u64(header.data()+16)!=count || count<2 || count>bytes/4096)return 32;
    SHA256_CTX input_sha,output_sha;SHA256_Init(&input_sha);SHA256_Init(&output_sha);
    SHA256_Update(&input_sha,header.data(),header.size());SHA256_Update(&output_sha,header.data(),header.size());
    if(std::fwrite(header.data(),1,header.size(),stdout)!=header.size())return 33;
    std::array<unsigned char,4104> input{},output{};
    std::array<unsigned char,8192> super{};uint64_t previous=0;
    for(uint64_t i=0;i<count;i++){
        if(!read_exact(input.data(),input.size()))return 34;
        uint64_t block=little_u64(input.data());
        if(block>=bytes/4096 || (i && block<=previous) || (i<2 && block!=i))return 35;
        previous=block;std::memcpy(output.data(),input.data(),8);
        if(!transform_units(key,input.data()+8,output.data()+8,4096,block,encrypt))return 36;
        if(i<2){
            std::memcpy(super.data()+i*4096,(encrypt?input.data():output.data())+8,4096);
            if(i==1 && !f2fs_superblock(super))return 37;
        }
        SHA256_Update(&input_sha,input.data(),input.size());SHA256_Update(&output_sha,output.data(),output.size());
        if(std::fwrite(output.data(),1,output.size(),stdout)!=output.size())return 38;
    }
    // stdin must be closed exactly after the declared records, not a live shell stream.
    unsigned char extra;if(TEMP_FAILURE_RETRY(read(STDIN_FILENO,&extra,1))!=0 || std::fflush(stdout))return 39;
    bool matches=finish_sha(&input_sha)==expected,unchanged=metadata_digest()==before;
    std::string digest=finish_sha(&output_sha);
    std::fprintf(stderr,"{\"transform\":\"%s\",\"image_bytes\":%llu,\"records\":%llu,\"output_sha256\":\"%s\",\"input_sha256_matches\":%s,\"software_matches_kernel_dm\":true,\"key_files_unchanged\":%s,\"phone_storage_written\":false,\"private_key_exported\":false}\n",
        encrypt?"encode":"decode",static_cast<unsigned long long>(bytes),static_cast<unsigned long long>(count),digest.c_str(),matches?"true":"false",unchanged?"true":"false");
    return matches && unchanged?0:40;
}

int main(int argc,char** argv){
    rlimit core{0,0};setrlimit(RLIMIT_CORE,&core);prctl(PR_SET_DUMPABLE,0);
    bool active=argc==2 && std::strcmp(argv[1],"--probe-active")==0;
    bool policy_probe=argc==2 && std::strcmp(argv[1],"--fscrypt-policy")==0;
    bool inactive_probe=argc==2 && std::strcmp(argv[1],"--probe-inactive")==0;
    bool capacity_write=argc==2 && std::strcmp(argv[1],"--capacity-create-persistent")==0;
    bool capacity_read=argc==3 && std::strcmp(argv[1],"--capacity-verify-persistent")==0;
    bool capacity_test=(argc==2 && std::strcmp(argv[1],"--exercise-capacity")==0) || capacity_write || capacity_read;
    bool export_inactive=argc==2 && std::strcmp(argv[1],"--export-inactive")==0;
    bool inactive_stream=argc==4 && (std::strcmp(argv[1],"--decrypt-backup-inactive")==0 ||
        std::strcmp(argv[1],"--encode-blocks-inactive")==0 || std::strcmp(argv[1],"--decode-blocks-inactive")==0);
    bool decrypt_stream=argc==4 && (std::strcmp(argv[1],"--decrypt-backup")==0 || std::strcmp(argv[1],"--decrypt-backup-inactive")==0);
    bool encode_blocks=argc==4 && (std::strcmp(argv[1],"--encode-blocks")==0 || std::strcmp(argv[1],"--encode-blocks-inactive")==0);
    bool decode_blocks=argc==4 && (std::strcmp(argv[1],"--decode-blocks")==0 || std::strcmp(argv[1],"--decode-blocks-inactive")==0);
    bool transform_blocks=encode_blocks || decode_blocks;
    uint64_t stream_bytes=0;
    if(capacity_read && (std::strlen(argv[2])!=64 || std::strspn(argv[2],"0123456789abcdef")!=64))return 2;
    if(decrypt_stream && (!android::base::ParseUint(argv[2],&stream_bytes) || stream_bytes<2147483648ULL ||
        stream_bytes>32ULL*1024*1024*1024 || stream_bytes%4096 || std::strlen(argv[3])!=64 || std::strspn(argv[3],"0123456789abcdef")!=64))return 2;
    if(transform_blocks && (!android::base::ParseUint(argv[2],&stream_bytes) || stream_bytes<2 ||
        stream_bytes>8ULL*1024*1024 || std::strlen(argv[3])!=64 || std::strspn(argv[3],"0123456789abcdef")!=64))return 2;
    if((!active && !inactive_probe && !policy_probe && !capacity_test && !export_inactive && !decrypt_stream && !transform_blocks) || getuid()!=0 || android::base::GetProperty("ro.product.vendor.device","")!="frost" ||
       android::base::GetProperty("sys.boot_completed","")!="1" ||
       android::base::GetProperty("ro.crypto.dm_default_key.options_format.version","")!="2")return 2;
    bool booted_gsi=android::base::GetBoolProperty("ro.gsid.image_running",false);
    bool expected_running=(active || policy_probe || capacity_test || decrypt_stream || transform_blocks) && !inactive_stream;
    if(expected_running!=booted_gsi)return 3;
    auto service=android::gsi::GetGsiService();bool running=!expected_running;
    if(!service || !service->isGsiRunning(&running).isOk() || running!=expected_running)return 4;
    if(capacity_read)return verify_persistent_capacity(argv[2]);
    if(capacity_test)return exercise_new_capacity(capacity_write);
    if(policy_probe){
        unique_fd fd(open("/data/user/0/org.c40.diagnostics",O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW));
        struct fscrypt_get_policy_ex_arg policy{};policy.policy_size=sizeof(policy.policy);
        if(fd<0 || ioctl(fd,FS_IOC_GET_ENCRYPTION_POLICY_EX,&policy) || policy.policy.version!=FSCRYPT_POLICY_V2)return 41;
        const auto& p=policy.policy.v2;
        std::printf("{\"policy_version\":%u,\"contents_mode\":%u,\"filenames_mode\":%u,\"flags\":%u,\"iv_ino_lblk_32\":%s,\"private_key_exported\":false,\"phone_storage_written\":false}\n",
            p.version,p.contents_encryption_mode,p.filenames_encryption_mode,p.flags,(p.flags&0x10)?"true":"false");
        return 0;
    }
    auto& dm=DeviceMapper::Instance();
    if(dm.GetState(kProbeName)!=DmDeviceState::INVALID)return 5;
    std::string before=metadata_digest();if(before.empty())return 6;
    KeyBuffer key;if(!read_existing_key(&key))return 7;
    if(decrypt_stream || transform_blocks){
        bool verified=validate_stream_crypto(key,service,inactive_stream);
        if(decrypt_stream)return decrypt_backup_stream(key,stream_bytes,argv[3],before,verified);
        return transform_block_stream(key,stream_bytes,argv[3],before,encode_blocks,verified);
    }
    MappingCleanup cleanup;
    auto prefix=image_prefix();
    if(prefix.empty() || !service->openImageService(prefix,&cleanup.images).isOk() || !cleanup.images)return 8;
    bool mapped=false;if(!cleanup.images->isImageMapped("userdata_gsi",&mapped).isOk())return 9;
    android::gsi::MappedImage raw;
    if(mapped){if(!cleanup.images->getMappedImageDevice("userdata_gsi",&raw.path).isOk())return 10;}
    else{if(!cleanup.images->mapImageDevice("userdata_gsi",10000,&raw).isOk())return 11;cleanup.raw_owned=true;}
    unique_fd raw_fd(open(raw.path.c_str(),O_RDONLY|O_CLOEXEC));uint64_t bytes=0;
    if(raw_fd<0 || ioctl(raw_fd,BLKGETSIZE64,&bytes) || bytes<2147483648ULL || bytes>32ULL*1024*1024*1024 || bytes%4096)return 12;
    raw_fd.reset();
    std::string hex(key.size()*2,'\0');const char* digits="0123456789abcdef";
    for(size_t i=0;i<key.size();i++){unsigned char b=key[i];hex[2*i]=digits[b>>4];hex[2*i+1]=digits[b&15];}
    auto target=std::make_unique<android::dm::DmTargetDefaultKey>(0,bytes/512,"aes-xts-plain64",hex,raw.path,0);
    target->SetSetDun();android::dm::DmTable table;table.set_readonly(true);table.AddTarget(std::move(target));
    std::string decrypted;
    bool created=dm.CreateDevice(kProbeName,table,&decrypted,std::chrono::seconds(5));
    OPENSSL_cleanse(&hex[0],hex.size());
    if(!created)return 13;cleanup.probe_owned=true;
    unique_fd fd(open(decrypted.c_str(),O_RDONLY|O_CLOEXEC));int readonly=0;
    if(fd<0 || ioctl(fd,BLKROGET,&readonly) || readonly!=1)return 14;
    std::array<unsigned char,8192> super{};
    if(pread(fd,super.data(),super.size(),0)!=static_cast<ssize_t>(super.size()) || !f2fs_superblock(super))return 15;
    bool matches_active=false;
    if(active){
        unique_fd live(open("/dev/block/mapper/userdata",O_RDONLY|O_CLOEXEC));std::array<unsigned char,8192> expected{};
        matches_active=live>=0 && pread(live,expected.data(),expected.size(),0)==static_cast<ssize_t>(expected.size()) && super==expected;
        if(!matches_active)return 16;
    }
    if(export_inactive){
        std::array<unsigned char,262144> buffer{};uint64_t total=0;SHA256_CTX sha;SHA256_Init(&sha);
        while(total<bytes){ssize_t n=TEMP_FAILURE_RETRY(read(fd,buffer.data(),buffer.size()));
            if(n<=0 || std::fwrite(buffer.data(),1,n,stdout)!=static_cast<size_t>(n))return 17;
            SHA256_Update(&sha,buffer.data(),n);total+=n;
        }
        if(std::fflush(stdout))return 18;
        unsigned char digest[SHA256_DIGEST_LENGTH];SHA256_Final(digest,&sha);char digest_hex[65];
        for(size_t i=0;i<sizeof(digest);i++)std::sprintf(digest_hex+2*i,"%02x",digest[i]);
        std::fprintf(stderr,"{\"metadata_decrypted_bytes\":%llu,\"sha256\":\"%s\",\"fbe_content_remains_encrypted\":true}\n",static_cast<unsigned long long>(total),digest_hex);
    }
    fd.reset();bool removed=cleanup.finish();bool keys_unchanged=metadata_digest()==before;
    std::fprintf(export_inactive?stderr:stdout,
        "{\"read_only\":true,\"f2fs_both_superblocks\":true,\"mapped_bytes\":%llu,\"active_superblocks_match\":%s,\"key_files_unchanged\":%s,\"temporary_mappings_removed\":%s,\"storage_written\":false}\n",
        static_cast<unsigned long long>(bytes),matches_active?"true":"false",keys_unchanged?"true":"false",removed?"true":"false");
    return removed && keys_unchanged?0:19;
}
