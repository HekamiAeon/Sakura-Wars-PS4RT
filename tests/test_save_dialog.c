/* Exercise the real service lifecycle without linking unrelated service APIs.
 * Section GC discards their resolver table; only save-storage/UI edges are
 * isolated below. The running game separately verifies NID dispatch. */
#include "src/runtime_services.c"
#include <assert.h>

static int choice = -1;
int runtime_savedata_param(int32_t user, const char *dir, void *param) {
    (void)user; (void)dir; (void)param; return -1;
}
int runtime_savedata_icon(int32_t user, const char *dir, char *out, size_t size) {
    (void)user; (void)dir; (void)out; (void)size; return -1;
}
int ps4rtgpu_choice_begin(const char *title, const char *const *items,
                         const char *const *images, int count, int focus) {
    (void)title; (void)items; (void)images; (void)focus;
    assert(count==1); return 1;
}
int ps4rtgpu_choice_poll(void) { return choice; }

static void mode(unsigned char *param, uint32_t value) { memcpy(param+52,&value,4); }

int main(void) {
    unsigned char param[128]={0}, result[40]={0};
    char selected[32];
    char names[32]="SAVE01";
    unsigned char items[48]={0};
    const char *names_ptr=names;
    const void *items_ptr=items;
    uint32_t count=1;
    memcpy(items+16,&names_ptr,8); memcpy(items+24,&count,4);
    assert(save_init()==0);
    mode(param,2);
    assert(save_open(param)==0);
    assert(save_result(result)==DIALOG_NOT_FINISHED);
    /* Startup must observe RUNNING before it can request readiness. */
    assert(save_status()==DIALOG_RUNNING && save_ready()==1);
    assert(save_result(result)==DIALOG_NOT_FINISHED);
    assert(save_status()==DIALOG_FINISHED && save_result(result)==0);
    uint32_t words[3]; memcpy(words,result,12);
    assert(words[0]==2 && words[1]==0 && words[2]==1);
    assert(save_status()==DIALOG_FINISHED);

    /* An explicit close wins over a queued automatic acknowledgement. */
    assert(save_open(param)==0 && save_status()==DIALOG_RUNNING);
    assert(save_close()==0 && save_status()==DIALOG_FINISHED);
    assert(save_term()==0 && save_init()==0 && save_status()==1);

    /* A progress bar stays running until the application closes it. */
    mode(param,5);
    assert(save_open(param)==0);
    for (int i=0;i<4;++i) assert(save_status()==DIALOG_RUNNING);
    assert(save_progress(0,75)==0 && save_close()==0);
    assert(save_status()==DIALOG_FINISHED);

    /* Button-less user messages must stay RUNNING until explicitly closed. */
    mode(param,SAVE_MODE_USER_MSG);
    unsigned char message_param[8]={0};
    const unsigned char *message_ptr=message_param;
    uint32_t button_type=SAVE_BUTTON_NONE;
    memcpy(message_param,&button_type,4);
    memcpy(param+80,&message_ptr,8);
    assert(save_open(param)==0);
    for (int i=0;i<6;++i) {
        assert(save_status()==DIALOG_RUNNING);
        assert(save_ready()==1);
        assert(save_result(result)==DIALOG_NOT_FINISHED);
    }
    assert(save_close()==0 && save_status()==DIALOG_FINISHED);

    /* User messages with an OK button retain their auto-acknowledgement. */
    button_type=0; /* OK */
    memcpy(message_param,&button_type,4);
    assert(save_open(param)==0);
    assert(save_status()==DIALOG_RUNNING);
    assert(save_status()==DIALOG_FINISHED);
    assert(save_result(result)==0);
    memset(param+80,0,8);

    /* List selection still waits for the UI, rather than auto-completing. */
    mode(param,1); memcpy(param+72,&items_ptr,8);
    assert(save_open(param)==0);
    for (int i=0;i<3;++i) assert(save_status()==DIALOG_RUNNING);
    choice=0;
    assert(save_status()==DIALOG_FINISHED);
    char *selected_ptr=selected;
    memset(result,0,sizeof(result)); memcpy(result+16,&selected_ptr,8);
    assert(save_result(result)==0 && !strcmp(selected,"SAVE01"));
    assert(save_term()==0);
    puts("Save dialog running, acknowledgement, progress and selection passed");
    return 0;
}
