/* Offline AppContent provider for the configured base-game profile.
 * Add-ons are host folders holding extracted DLC packages (--addcont LABEL=DIR): they are
 * listed as installed and mounted read-only at /addcontN. No downloads, entitlement or
 * license emulation. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define INVALID_ID ((int32_t)0x805a1000)
#define NOT_LOADED ((int32_t)0x805a1001)
#define PARAMETER ((int32_t)0x80d90002)
#define BUSY ((int32_t)0x80d90003)
#define NOT_FOUND ((int32_t)0x80d90005)
#define MAX_ADDONS 32
#define INSTALLED 4 /* SCE_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_INSTALLED */
typedef struct { char label[17]; char host[512]; int mounted; } Addon;
static Addon addons[MAX_ADDONS];
static unsigned addon_count, mounts;
static unsigned refs, initialized, configured;
static uint32_t parameters[5];
static unsigned queries, lists;
void runtime_content_configure(const uint32_t values[5]) {
    if (refs || initialized || (values[0]!=1 && values[0]!=3)) {
        fputs("ERROR: invalid or late content profile\n",stderr); exit(1);
    }
    memcpy(parameters,values,sizeof(parameters)); configured=1;
}
/* LABEL=DIR: the 16-character entitlement label (the end of the DLC's content ID). */
int runtime_content_addon(const char *spec) {
    const char *eq=strchr(spec,'=');
    if (addon_count==MAX_ADDONS || !eq || eq-spec!=16 || !eq[1] || strlen(eq+1)>=512) return -1;
    Addon *a=&addons[addon_count++];
    memcpy(a->label,spec,16); a->label[16]=0;
    snprintf(a->host,sizeof(a->host),"%s",eq+1);
    printf("Runtime: add-on %s -> %s\n",a->label,a->host);
    return 0;
}
static void require_provider(void) {
    if (!configured) { fputs("STOP: AppContent needs --content-profile\n",stderr); exit(21); }
}
/* System modules: every library this eboot imports is provided by the host
 * runtime, so loading one only tracks a reference count. AppContent (0xb4)
 * additionally requires the configured content profile. */
static unsigned module_refs[0x10000];
static ABI int32_t module_load(uint16_t id) {
    if (!id) return INVALID_ID;
    if (id==0xb4) require_provider();
    if (module_refs[id]==UINT32_MAX) { fputs("STOP: module reference overflow\n",stderr); exit(21); }
    ++module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; printf("Runtime: AppContent host provider loaded; references=%u\n",refs); }
    else printf("Runtime: system module 0x%x loaded (host implementation)\n",id);
    return 0;
}
static ABI int32_t module_loaded(uint16_t id) {
    if (!id) return INVALID_ID;
    return module_refs[id] ? 0 : NOT_LOADED;
}
static ABI int32_t module_unload(uint16_t id) {
    int32_t result=module_loaded(id);
    if (result) return result;
    --module_refs[id];
    if (id==0xb4) { refs=module_refs[id]; if (!refs) initialized=0; }
    return 0;
}
static void require_loaded(void) {
    require_provider();
    if (!refs) { fputs("STOP: AppContent called before module load\n",stderr); exit(21); }
}
static ABI int32_t content_init(const unsigned char *init,unsigned char *boot) {
    require_loaded();
    if (!init || !boot) return PARAMETER;
    if (initialized) return BUSY;
    /* Only the reserved-zero init profile is implemented. */
    for (unsigned i=0;i<32;++i) if (init[i]) return PARAMETER;
    memset(boot,0,40); initialized=1;
    printf("Runtime: AppContent initialized; offline base-game profile, %u add-ons\n",addon_count);
    return 0;
}
static void require_initialized(void) {
    require_loaded();
    if (!initialized) { fputs("STOP: AppContent query before initialization\n",stderr); exit(21); }
}
static ABI int32_t param_int(uint32_t id,int32_t *out) {
    require_initialized();
    if (id>4 || !out) return PARAMETER;
    memcpy(out,&parameters[id],4); ++queries;
    printf("Runtime: AppContent parameter %u = %d\n",id,*out);
    return 0;
}
static ABI int32_t addon_list(uint32_t service,void *list,uint32_t capacity,uint32_t *hits) {
    require_initialized();
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); exit(21); }
    if ((!capacity || !list) && !hits) return PARAMETER;
    /* SceAppContentAddcontInfo: 17-byte label, 3 padding, uint32 status. Never touch unused
     * list slots. */
    uint32_t n=addon_count;
    if (list) {
        if (n>capacity) n=capacity;
        for (uint32_t i=0;i<n;++i) {
            unsigned char *e=(unsigned char *)list+i*24;
            uint32_t status=INSTALLED;
            memset(e,0,20); memcpy(e,addons[i].label,16); memcpy(e+20,&status,4);
        }
    }
    if (hits) *hits=n;
    ++lists; return 0;
}
static ABI int32_t addon_mount(uint32_t service,const char *label,char *point) {
    require_initialized();
    if (service) { fputs("STOP: nonzero AppContent service label unsupported\n",stderr); exit(21); }
    if (!label || !point) return PARAMETER;
    for (unsigned i=0;i<addon_count;++i) {
        if (strncmp(addons[i].label,label,16)) continue;
        char guest[24]; snprintf(guest,sizeof(guest),"/addcont%u",i); /* i < 32 */
        if (!addons[i].mounted) {
            if (runtime_file_mount(guest,addons[i].host)) return BUSY;
            addons[i].mounted=1; ++mounts;
            printf("Runtime: add-on %s mounted at %s\n",addons[i].label,guest);
        }
        memset(point,0,16); memcpy(point,guest,strlen(guest));
        return 0;
    }
    return NOT_FOUND;
}
uintptr_t runtime_content_resolve(const char *name) {
    if (!strcmp(name,"g8cM39EUZ6o#libSceSysmodule")) return (uintptr_t)module_load;
    if (!strcmp(name,"fMP5NHUOaMk#libSceSysmodule")) return (uintptr_t)module_loaded;
    if (!strcmp(name,"eR2bZFAAU0Q#libSceSysmodule")) return (uintptr_t)module_unload;
    if (!strcmp(name,"R9lA82OraNs#libSceAppContent")) return (uintptr_t)content_init;
    if (!strcmp(name,"99b82IKXpH4#libSceAppContent")) return (uintptr_t)param_int;
    if (!strcmp(name,"xnd8BJzAxmk#libSceAppContent")) return (uintptr_t)addon_list;
    if (!strcmp(name,"VANhIWcqYak#libSceAppContent")) return (uintptr_t)addon_mount;
    return 0;
}
void runtime_content_report(void) {
    printf("Runtime: AppContent references=%u, initialized=%u, parameter queries=%u, list queries=%u, add-on mounts=%u\n",refs,initialized,queries,lists,mounts);
}
