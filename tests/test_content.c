#include "runtime.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef int32_t (ABI *Module)(uint16_t);
typedef int32_t (ABI *Init)(const void *,void *);
typedef int32_t (ABI *Param)(uint32_t,int32_t *);
typedef int32_t (ABI *List)(uint32_t,void *,uint32_t,uint32_t *);
typedef int32_t (ABI *Mount)(uint32_t,const char *,char *);
#define GET(t,n) ((t)runtime_content_resolve(n))
static char mounted_guest[64], mounted_host[512];
static unsigned mount_calls;
int runtime_file_mount(const char *guest,const char *host) {
    snprintf(mounted_guest,sizeof(mounted_guest),"%s",guest);
    snprintf(mounted_host,sizeof(mounted_host),"%s",host);
    ++mount_calls; return 0;
}
int main(int argc,char **argv) {
    Module load=GET(Module,"g8cM39EUZ6o#libSceSysmodule");
    Module loaded=GET(Module,"fMP5NHUOaMk#libSceSysmodule");
    Module unload=GET(Module,"eR2bZFAAU0Q#libSceSysmodule");
    Init init=GET(Init,"R9lA82OraNs#libSceAppContent");
    Param param=GET(Param,"99b82IKXpH4#libSceAppContent");
    List list=GET(List,"xnd8BJzAxmk#libSceAppContent");
    assert(load && loaded && unload && init && param && list);
    assert(!runtime_content_resolve("g8cM39EUZ6o#libkernel"));
    if (argc>1 && !strcmp(argv[1],"--missing")) { load(0xb4); return 99; }
    const uint32_t profile[]={1,13,0x80000000,0,7};
    runtime_content_configure(profile);
    if (argc>1 && !strcmp(argv[1],"--addons")) {
        /* Extracted DLC folders: listed as installed, mounted once at /addcontN. */
        Mount mount=GET(Mount,"VANhIWcqYak#libSceAppContent");
        assert(mount);
        assert(runtime_content_addon("SHORT=/dlc/x")==-1 && runtime_content_addon("AAAAAAAAAAAAAAAA=")==-1);
        assert(runtime_content_addon("AAAAAAAAAAAAAA01=/dlc/one")==0);
        assert(runtime_content_addon("AAAAAAAAAAAAAA02=/dlc/two")==0);
        unsigned char initial[32]={0}, boot[40];
        assert(load(0xb4)==0 && init(initial,boot)==0);
        unsigned char entries[3*24]; memset(entries,0xaa,sizeof(entries));
        uint32_t hits=0, status;
        assert(list(0,NULL,0,&hits)==0 && hits==2);
        assert(list(0,entries,1,&hits)==0 && hits==1 && entries[24]==0xaa);
        assert(list(0,entries,3,&hits)==0 && hits==2 && entries[48]==0xaa);
        assert(!memcmp(entries+24,"AAAAAAAAAAAAAA02",16) && !entries[24+16] && !entries[24+19]);
        memcpy(&status,entries+24+20,4); assert(status==4);
        char label[17]="AAAAAAAAAAAAAA02", point[16];
        memset(point,0xaa,sizeof(point));
        assert(mount(0,label,point)==0 && !strcmp(point,"/addcont1") && !point[15]);
        assert(!strcmp(mounted_guest,"/addcont1") && !strcmp(mounted_host,"/dlc/two"));
        assert(mount(0,label,point)==0 && mount_calls==1);
        assert((uint32_t)mount(0,"BBBBBBBBBBBBBB01",point)==0x80d90005);
        assert((uint32_t)mount(0,label,NULL)==0x80d90002);
        puts("PASS: add-on list and mounts"); return 0;
    }
    if (argc>1 && !strcmp(argv[1],"--unknown")) {
        /* Other system modules are host-provided: reference counted, independent. */
        assert((uint32_t)loaded(0xb5)==0x805a1001);
        assert(load(0xb5)==0 && loaded(0xb5)==0 && (uint32_t)loaded(0xb4)==0x805a1001);
        assert(unload(0xb5)==0 && (uint32_t)loaded(0xb5)==0x805a1001);
        puts("PASS: independent module references"); return 0;
    }
    assert((uint32_t)loaded(0)==0x805a1000);
    assert((uint32_t)loaded(0xb4)==0x805a1001);
    assert((uint32_t)unload(0xb4)==0x805a1001);
    assert(load(0xb4)==0 && load(0xb4)==0 && loaded(0xb4)==0);
    unsigned char initial[32]={0};
    struct { unsigned char boot[40]; uint32_t canary; } b;
    memset(&b,0xa5,sizeof(b));
    assert((uint32_t)init(NULL,b.boot)==0x80d90002);
    initial[31]=1; assert((uint32_t)init(initial,b.boot)==0x80d90002); initial[31]=0;
    assert(b.boot[0]==0xa5 && init(initial,b.boot)==0);
    for (unsigned i=0;i<40;++i) assert(b.boot[i]==0);
    assert(b.canary==0xa5a5a5a5);
    b.boot[0]=0xcc; assert((uint32_t)init(initial,b.boot)==0x80d90003 && b.boot[0]==0xcc);
    struct { int32_t value; uint32_t canary; } result={-1,0x1234};
    for (unsigned i=0;i<5;++i) {
        assert(param(i,&result.value)==0 && (uint32_t)result.value==profile[i]);
        assert(result.canary==0x1234);
    }
    result.value=77;
    assert((uint32_t)param(5,&result.value)==0x80d90002 && result.value==77);
    assert((uint32_t)param(0,NULL)==0x80d90002);
    unsigned char entries[48]; memset(entries,0xaa,sizeof(entries));
    struct { uint32_t hits,canary; } h={99,0xabcdef};
    assert(list(0,NULL,0,&h.hits)==0 && h.hits==0 && h.canary==0xabcdef);
    assert(list(0,entries,2,&h.hits)==0 && h.hits==0);
    for (unsigned i=0;i<sizeof(entries);++i) assert(entries[i]==0xaa);
    assert((uint32_t)list(0,NULL,0,NULL)==0x80d90002);
    assert(unload(0xb4)==0 && loaded(0xb4)==0);
    assert((uint32_t)init(initial,b.boot)==0x80d90003);
    assert(unload(0xb4)==0 && (uint32_t)loaded(0xb4)==0x805a1001);
    assert(load(0xb4)==0 && init(initial,b.boot)==0 && unload(0xb4)==0);
    puts("PASS: AppContent lifecycle, profile and output boundaries");
    return 0;
}
